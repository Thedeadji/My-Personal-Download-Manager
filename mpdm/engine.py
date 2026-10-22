"""The segmented download engine — MPDM's core.

Design (the same idea IDM and aria2 use, implemented from scratch):

1. Probe the URL to learn its size and whether the server supports byte ranges.
2. If ranges are supported and the size is known, split the file into N
   contiguous segments and download each over its own connection/thread,
   writing directly to the correct offset in a pre-allocated output file.
3. Persist each segment's progress to a sidecar so an interrupted download can
   resume instead of starting over.
4. If ranges are NOT supported (or size unknown), fall back to a single
   streamed download.

A single pre-allocated file with positioned writes (seek+write) avoids the need
to merge temporary part-files afterwards.
"""
from __future__ import annotations

import os
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Callable, Optional

import requests

from .config import Settings
from .http_client import build_session, probe, open_range
from .models import Segment, Status, Progress, RemoteInfo, TaskRecord
from .ratelimit import RateLimiter
from .categories import categorized_dir
from . import storage

ProgressCb = Callable[[Progress], None]


class DownloadError(Exception):
    pass


def plan_segments(size: int, connections: int, min_segment_size: int) -> list[Segment]:
    """Divide [0, size) into up to `connections` contiguous segments."""
    if size <= 0:
        return [Segment(index=0, start=0, end=-1)]  # unknown/empty handled by caller
    n = max(1, min(connections, (size + min_segment_size - 1) // min_segment_size))
    base = size // n
    segments = []
    start = 0
    for i in range(n):
        end = size - 1 if i == n - 1 else start + base - 1
        segments.append(Segment(index=i, start=start, end=end))
        start = end + 1
    return segments


class Download:
    """Represents a single download task and drives it to completion."""

    def __init__(
        self,
        url: str,
        settings: Optional[Settings] = None,
        dest_path: Optional[str] = None,
        progress_cb: Optional[ProgressCb] = None,
        headers: Optional[dict] = None,
        global_limiter: Optional[RateLimiter] = None,
    ):
        self.url = url
        self.settings = settings or Settings()
        self.dest_path = dest_path
        self.progress_cb = progress_cb
        self.headers = headers or {}

        # Per-download throttle (shared by this download's segment workers) plus
        # an optional global throttle shared across all downloads.
        self._limiter = RateLimiter(self.settings.max_rate_bps)
        self._global_limiter = global_limiter

        self.info: Optional[RemoteInfo] = None
        self.segments: list[Segment] = []
        self.status = Status.PENDING

        self._lock = threading.Lock()
        self._pause = threading.Event()
        self._cancel = threading.Event()
        self._session = build_session(self.settings, extra_headers=self.headers)
        self._downloaded_at_start = 0
        self._t0 = 0.0
        self._last_emit = 0.0
        self._active = 0

    # ---- control ---------------------------------------------------------
    def pause(self):
        self._pause.set()

    def cancel(self):
        self._cancel.set()

    def set_rate(self, rate_bps: float):
        """Change this download's speed limit (bytes/sec, 0 = unlimited) live."""
        self._limiter.set_rate(rate_bps)

    def _interruptible_sleep(self, duration: float):
        """Sleep up to `duration`, but wake early if paused or cancelled."""
        end = time.time() + duration
        while True:
            remaining = end - time.time()
            if remaining <= 0 or self._cancel.is_set() or self._pause.is_set():
                return
            time.sleep(min(0.1, remaining))

    # ---- helpers ---------------------------------------------------------
    def _throttle(self, nbytes: int):
        """Block as needed to honor the per-download and global speed limits."""
        self._limiter.consume(nbytes)
        if self._global_limiter is not None:
            self._global_limiter.consume(nbytes)

    @property
    def total_downloaded(self) -> int:
        return sum(s.downloaded for s in self.segments)

    def _emit(self, force: bool = False):
        if not self.progress_cb:
            return
        now = time.time()
        if not force and (now - self._last_emit) < 0.2:
            return
        self._last_emit = now
        done = self.total_downloaded
        elapsed = max(1e-6, now - self._t0)
        speed = (done - self._downloaded_at_start) / elapsed
        total = self.info.size if self.info else None
        eta = None
        if total and speed > 0:
            eta = max(0.0, (total - done) / speed)
        self.progress_cb(
            Progress(
                total=total,
                downloaded=done,
                speed_bps=speed,
                status=self.status,
                segments=len(self.segments),
                active_segments=self._active,
                eta_seconds=eta,
                filename=os.path.basename(self.dest_path or (self.info.filename if self.info else "")),
            )
        )

    def _preallocate(self, size: int):
        with open(self.dest_path, "wb") as f:
            if size > 0:
                f.truncate(size)

    def _save(self):
        rec = TaskRecord(
            url=self.url,
            dest_path=self.dest_path,
            size=self.info.size if self.info else None,
            accept_ranges=self.info.accept_ranges if self.info else False,
            segments=storage.segments_to_dicts(self.segments),
            status=self.status.value,
            etag=self.info.etag if self.info else None,
            last_modified=self.info.last_modified if self.info else None,
        )
        storage.save_record(self.dest_path, rec)

    # ---- segment worker --------------------------------------------------
    def _download_segment(self, seg: Segment):
        if seg.complete:
            return
        with self._lock:
            self._active += 1
        try:
            attempt = 0
            while not seg.complete:
                if self._cancel.is_set() or self._pause.is_set():
                    return
                try:
                    start = seg.current_offset
                    resp = open_range(self.url, start, seg.end, self.settings, self._session)
                    with open(self.dest_path, "r+b") as f:
                        f.seek(start)
                        for chunk in resp.iter_content(self.settings.chunk_size):
                            if self._cancel.is_set() or self._pause.is_set():
                                resp.close()
                                return
                            if not chunk:
                                continue
                            f.write(chunk)
                            with self._lock:
                                seg.downloaded += len(chunk)
                            self._throttle(len(chunk))
                            self._emit()
                    resp.close()
                    if seg.complete:
                        return
                    # Stream ended early; loop will re-request the remainder.
                except (requests.RequestException, OSError) as e:
                    attempt += 1
                    if attempt > self.settings.max_retries:
                        raise DownloadError(f"segment {seg.index} failed: {e}") from e
                    self._interruptible_sleep(self.settings.retry_backoff ** attempt)
        finally:
            with self._lock:
                self._active -= 1

    def _download_single_stream(self):
        """Fallback when ranges aren't supported or size is unknown."""
        self.status = Status.DOWNLOADING
        seg = Segment(index=0, start=0, end=(self.info.size - 1) if self.info and self.info.size else 2**63)
        self.segments = [seg]
        resume_from = 0
        if os.path.exists(self.dest_path) and self.info and self.info.accept_ranges:
            resume_from = os.path.getsize(self.dest_path)
        mode = "r+b" if resume_from else "wb"
        attempt = 0
        while True:
            try:
                resp = open_range(self.url, resume_from, None, self.settings, self._session) if resume_from \
                    else self._session.get(self.url, stream=True, allow_redirects=True,
                                           timeout=(self.settings.connect_timeout, self.settings.read_timeout),
                                           verify=self.settings.verify_tls)
                resp.raise_for_status()
                with open(self.dest_path, mode) as f:
                    if resume_from:
                        f.seek(resume_from)
                    for chunk in resp.iter_content(self.settings.chunk_size):
                        if self._cancel.is_set() or self._pause.is_set():
                            resp.close()
                            return
                        if chunk:
                            f.write(chunk)
                            with self._lock:
                                seg.downloaded += len(chunk)
                            self._throttle(len(chunk))
                            self._emit()
                resp.close()
                return
            except (requests.RequestException, OSError) as e:
                attempt += 1
                if attempt > self.settings.max_retries:
                    raise DownloadError(f"single-stream download failed: {e}") from e
                resume_from = os.path.getsize(self.dest_path) if os.path.exists(self.dest_path) else resume_from
                mode = "r+b"
                self._interruptible_sleep(self.settings.retry_backoff ** attempt)

    # ---- orchestration ---------------------------------------------------
    def start(self) -> str:
        """Run the download to completion. Returns the output path."""
        self._t0 = time.time()

        # 1. Probe
        self.status = Status.PROBING
        self._emit(force=True)
        self.info = probe(self.url, self.settings, self._session)

        # 2. Decide output path. Prefer resuming a matching interrupted download
        #    in the output dir over creating a new (1), (2), ... copy.
        if not self.dest_path:
            # Route into an IDM-style category subfolder (Video, Music, …) by
            # file type when enabled; "Other" types stay in output_dir.
            out_dir = categorized_dir(self.settings.output_dir, self.info.filename,
                                      self.settings.categorize)
            os.makedirs(out_dir, exist_ok=True)
            resumable = storage.find_resumable(
                out_dir, self.url, self.info.size, self.info.etag
            )
            self.dest_path = resumable or storage.unique_dest(
                out_dir, self.info.filename
            )

        # 3. Resume if a valid sidecar exists and the remote hasn't changed
        resumed = False
        rec = storage.load_record(self.dest_path)
        if rec and rec.size == self.info.size and rec.etag == self.info.etag and os.path.exists(self.dest_path):
            self.segments = storage.segments_from_record(rec)
            self._downloaded_at_start = sum(s.downloaded for s in self.segments)
            resumed = bool(self.segments)

        # 4. Plan segments (fresh)
        if not resumed:
            if self.info.accept_ranges and self.info.size and self.info.size > 0:
                self.segments = plan_segments(self.info.size, self.settings.connections, self.settings.min_segment_size)
                self._preallocate(self.info.size)
            else:
                # Single stream path
                self._single = True
                try:
                    self._download_single_stream()
                    self._finish_single()
                    return self.dest_path
                finally:
                    pass

        # 5. Parallel segmented download
        self.status = Status.DOWNLOADING
        self._emit(force=True)
        self._save()

        # Background checkpoint: periodically persist resume metadata while the
        # workers run, so a hard crash loses at most a second of progress.
        stop_ckpt = threading.Event()

        def _checkpoint():
            while not stop_ckpt.wait(1.0):
                self._save()

        ckpt = threading.Thread(target=_checkpoint, daemon=True)
        ckpt.start()

        errors = []
        try:
            with ThreadPoolExecutor(max_workers=len(self.segments)) as pool:
                futures = {pool.submit(self._download_segment, s): s
                           for s in self.segments if not s.complete}
                for fut in as_completed(futures):
                    try:
                        fut.result()
                    except Exception as e:  # noqa: BLE001
                        errors.append(e)
        finally:
            stop_ckpt.set()
            ckpt.join(timeout=2.0)
            self._save()

        if self._cancel.is_set():
            self.status = Status.FAILED
            self._emit(force=True)
            raise DownloadError("cancelled")
        if self._pause.is_set():
            self.status = Status.PAUSED
            self._save()
            self._emit(force=True)
            raise DownloadError("paused")
        if errors:
            self.status = Status.FAILED
            self._save()
            self._emit(force=True)
            raise errors[0]

        # 6. Verify and finish
        if not all(s.complete for s in self.segments):
            self.status = Status.FAILED
            self._save()
            raise DownloadError("incomplete after workers finished")

        self._finish_common()
        return self.dest_path

    def _finish_single(self):
        self.status = Status.COMPLETED
        storage.clear_record(self.dest_path)
        self._emit(force=True)

    def _finish_common(self):
        self.status = Status.COMPLETED
        if self.info and self.info.size:
            actual = os.path.getsize(self.dest_path)
            if actual != self.info.size:
                self.status = Status.FAILED
                raise DownloadError(f"size mismatch: expected {self.info.size}, got {actual}")
        storage.clear_record(self.dest_path)
        self._emit(force=True)


def download(url: str, settings: Optional[Settings] = None, dest_path: Optional[str] = None,
             progress_cb: Optional[ProgressCb] = None, headers: Optional[dict] = None) -> str:
    """Convenience one-shot download."""
    return Download(url, settings=settings, dest_path=dest_path,
                    progress_cb=progress_cb, headers=headers).start()
