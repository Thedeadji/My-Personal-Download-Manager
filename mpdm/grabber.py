"""High-level stream grabber: HLS/DASH manifest -> downloaded MP4.

Ties the manifest parsers (hls, dash), the segment downloader and the muxer
together. Segments are fetched concurrently (each is a normal HTTP GET) and
then assembled with ffmpeg.
"""
from __future__ import annotations

import os
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Callable, Optional

import requests

from .config import Settings
from .http_client import build_session
from . import hls, dash, muxer


ProgressCb = Callable[[int, int], None]  # (segments_done, segments_total)


def _download_one(url: str, path: str, settings: Settings, session: requests.Session) -> str:
    attempt = 0
    while True:
        try:
            r = session.get(url, stream=True, timeout=(settings.connect_timeout, settings.read_timeout),
                            allow_redirects=True, verify=settings.verify_tls)
            r.raise_for_status()
            with open(path, "wb") as f:
                for chunk in r.iter_content(settings.chunk_size):
                    if chunk:
                        f.write(chunk)
            r.close()
            return path
        except (requests.RequestException, OSError):
            attempt += 1
            if attempt > settings.max_retries:
                raise


def _fetch_segments(seg_urls: list[str], workdir: str, settings: Settings,
                    session: requests.Session, progress_cb: Optional[ProgressCb]) -> list[str]:
    paths = [os.path.join(workdir, f"seg_{i:06d}") for i in range(len(seg_urls))]
    done = 0
    total = len(seg_urls)
    with ThreadPoolExecutor(max_workers=min(settings.connections, max(1, total))) as pool:
        futs = {pool.submit(_download_one, u, p, settings, session): p
                for u, p in zip(seg_urls, paths)}
        for fut in as_completed(futs):
            fut.result()
            done += 1
            if progress_cb:
                progress_cb(done, total)
    return paths


def grab(url: str, out_path: str, settings: Optional[Settings] = None,
         kind: str = "auto", prefer: str = "best",
         progress_cb: Optional[ProgressCb] = None) -> str:
    """Download an HLS or DASH stream to a single MP4 at out_path.

    kind: "hls", "dash", or "auto" (guess from the URL/extension).
    prefer: "best", "worst", or a resolution hint like "720".
    """
    settings = settings or Settings()
    session = build_session(settings)

    if kind == "auto":
        low = url.lower()
        kind = "dash" if ".mpd" in low else "hls"

    workdir = tempfile.mkdtemp(prefix="mdm_grab_")
    init_path = None
    try:
        if kind == "hls":
            media, media_url = hls.resolve_media_playlist(url, settings, session, prefer=prefer)
            if not media.segments:
                raise ValueError("no media segments found in HLS playlist")
            if media.key_method not in ("NONE", ""):
                # Extension point: fetch key from media.key_uri and AES-128-CBC
                # decrypt each segment before muxing.
                raise NotImplementedError(
                    f"encrypted HLS ({media.key_method}) not supported in this build; "
                    "segment decryption is a marked extension point in hls.py/grabber.py"
                )
            seg_urls = media.segments
            init_url = media.init_segment
        elif kind == "dash":
            stream = dash.parse_mpd(url, settings, session, prefer=prefer)
            seg_urls = stream.segment_urls
            init_url = stream.init_url
            if not seg_urls:
                raise ValueError("no segments found in MPD")
        else:
            raise ValueError(f"unknown kind: {kind}")

        if init_url:
            init_path = os.path.join(workdir, "init.mp4")
            _download_one(init_url, init_path, settings, session)

        seg_paths = _fetch_segments(seg_urls, workdir, settings, session, progress_cb)
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        muxer.assemble_segments(seg_paths, out_path, init_path=init_path, cleanup=True)
        return out_path
    finally:
        # Clean the temp workdir (segments already removed by assemble on success).
        try:
            for name in os.listdir(workdir):
                os.remove(os.path.join(workdir, name))
            os.rmdir(workdir)
        except OSError:
            pass
