"""Multi-download manager: a queue of downloads with categories and concurrency.

Sits on top of the single-file `engine.Download` and coordinates many of them:
a bounded number run at once, the rest wait in a queue, and each can be paused,
resumed (via the engine's sidecar resume) or removed. The list is persisted to
~/.mypersonaldownloadmanager/downloads.json so it survives restarts.

This is the backend used by the desktop GUI (mpdm/ui/app.py). It is UI-agnostic:
it just exposes items with live fields the UI polls.
"""
from __future__ import annotations

import json
import os
import threading
import time
import uuid
from dataclasses import dataclass, field, asdict
from typing import Optional

from .config import Settings, state_dir
from .engine import Download, DownloadError
from .models import Progress, Status


# ---- categories (by file extension) --------------------------------------
CATEGORY_EXT = {
    "Video": {"mp4", "mkv", "webm", "avi", "mov", "flv", "m4v", "ts", "wmv", "mpg", "mpeg"},
    "Audio": {"mp3", "m4a", "aac", "flac", "wav", "ogg", "opus", "wma"},
    "Documents": {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "epub", "csv"},
    "Archives": {"zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "iso", "cab"},
    "Programs": {"exe", "msi", "msix", "dmg", "pkg", "deb", "rpm", "apk", "appimage"},
}
CATEGORIES = ["All", "Downloading", "Finished", "Video", "Audio", "Documents", "Archives", "Programs", "Other"]


def classify(filename: str) -> str:
    ext = os.path.splitext(filename or "")[1].lower().lstrip(".")
    for cat, exts in CATEGORY_EXT.items():
        if ext in exts:
            return cat
    return "Other"


ST_QUEUED = "queued"
ST_DOWNLOADING = "downloading"
ST_PAUSED = "paused"
ST_COMPLETED = "completed"
ST_ERROR = "error"


@dataclass
class DownloadItem:
    id: str
    url: str
    filename: str = ""
    dest_path: Optional[str] = None
    size: Optional[int] = None
    downloaded: int = 0
    speed: float = 0.0
    eta: Optional[float] = None
    status: str = ST_QUEUED
    connections: Optional[int] = None
    headers: dict = field(default_factory=dict)
    error: str = ""
    added_at: float = field(default_factory=time.time)

    # runtime-only (not persisted)
    _dl: Optional[Download] = field(default=None, repr=False, compare=False)

    @property
    def category(self) -> str:
        return classify(self.filename or self.url)

    @property
    def percent(self) -> Optional[float]:
        if not self.size:
            return None
        return min(100.0, self.downloaded * 100.0 / self.size)

    def to_dict(self) -> dict:
        # Build manually — never asdict(), which would try to deep-copy the live
        # Download object in _dl (it holds thread locks and can't be pickled).
        return {
            "id": self.id, "url": self.url, "filename": self.filename,
            "dest_path": self.dest_path, "size": self.size,
            "downloaded": self.downloaded, "speed": self.speed, "eta": self.eta,
            "status": self.status, "connections": self.connections,
            "headers": dict(self.headers), "error": self.error, "added_at": self.added_at,
        }


class DownloadManager:
    def __init__(self, settings: Optional[Settings] = None, max_concurrent: int = 3):
        self.settings = settings or Settings()
        self.max_concurrent = max_concurrent
        self.items: dict[str, DownloadItem] = {}
        self._order: list[str] = []
        self._lock = threading.RLock()
        self._store = state_dir() / "downloads.json"
        self.load()

    # ---- persistence -----------------------------------------------------
    def load(self):
        try:
            data = json.loads(self._store.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return
        with self._lock:
            for d in data.get("items", []):
                d.pop("_dl", None)
                it = DownloadItem(**{k: v for k, v in d.items() if k in DownloadItem.__dataclass_fields__})
                # anything that was mid-flight comes back paused, never auto-resumes
                if it.status in (ST_DOWNLOADING, ST_QUEUED):
                    it.status = ST_PAUSED
                self.items[it.id] = it
                self._order.append(it.id)

    def save(self):
        with self._lock:
            data = {"items": [self.items[i].to_dict() for i in self._order if i in self.items]}
        try:
            tmp = str(self._store) + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
            os.replace(tmp, self._store)
        except OSError:
            pass

    # ---- queue listing ---------------------------------------------------
    def ordered(self) -> list[DownloadItem]:
        with self._lock:
            return [self.items[i] for i in self._order if i in self.items]

    def active_count(self) -> int:
        return sum(1 for it in self.items.values() if it.status == ST_DOWNLOADING)

    # ---- mutations -------------------------------------------------------
    def add(self, url: str, filename: str = "", connections: Optional[int] = None,
            headers: Optional[dict] = None, start: bool = True) -> DownloadItem:
        item = DownloadItem(
            id=uuid.uuid4().hex[:12],
            url=url.strip(),
            filename=filename or "",
            connections=connections,
            headers=headers or {},
            status=ST_QUEUED if start else ST_PAUSED,
        )
        with self._lock:
            self.items[item.id] = item
            self._order.append(item.id)
        self.save()
        if start:
            self.pump()
        return item

    def pump(self):
        """Start queued downloads up to the concurrency limit."""
        with self._lock:
            if self.active_count() >= self.max_concurrent:
                return
            slots = self.max_concurrent - self.active_count()
            to_start = [it for it in self.ordered() if it.status == ST_QUEUED][:slots]
            for it in to_start:
                it.status = ST_DOWNLOADING
                t = threading.Thread(target=self._run, args=(it,), daemon=True)
                t.start()

    def _run(self, item: DownloadItem):
        settings = Settings(**self.settings.as_dict())
        if item.connections:
            settings.connections = item.connections

        def cb(p: Progress):
            item.downloaded = p.downloaded
            if p.total:
                item.size = p.total
            item.speed = p.speed_bps
            item.eta = p.eta_seconds
            if item._dl and item._dl.dest_path:
                item.dest_path = item._dl.dest_path
                if not item.filename:
                    item.filename = os.path.basename(item._dl.dest_path)

        dl = Download(item.url, settings=settings, dest_path=item.dest_path,
                      progress_cb=cb, headers=item.headers)
        item._dl = dl
        try:
            path = dl.start()
            item.dest_path = path
            item.filename = os.path.basename(path)
            item.status = ST_COMPLETED
            item.speed = 0.0
            item.eta = 0
        except DownloadError as e:
            msg = str(e)
            if "paused" in msg:
                item.status = ST_PAUSED
            elif "cancelled" in msg:
                return  # removed; don't touch
            else:
                item.status = ST_ERROR
                item.error = msg
            item.speed = 0.0
        except Exception as e:  # noqa: BLE001
            item.status = ST_ERROR
            item.error = str(e)
            item.speed = 0.0
        finally:
            item._dl = None
            self.save()
            self.pump()

    def pause(self, item_id: str):
        it = self.items.get(item_id)
        if not it:
            return
        if it.status == ST_DOWNLOADING and it._dl:
            it._dl.pause()          # engine checkpoints to sidecar, raises -> _run sets paused
        elif it.status == ST_QUEUED:
            it.status = ST_PAUSED
            self.save()

    def resume(self, item_id: str):
        it = self.items.get(item_id)
        if not it:
            return
        if it.status in (ST_PAUSED, ST_ERROR):
            it.error = ""
            it.status = ST_QUEUED
            self.save()
            self.pump()

    def remove(self, item_id: str, delete_file: bool = False):
        it = self.items.get(item_id)
        if not it:
            return
        if it.status == ST_DOWNLOADING and it._dl:
            it._dl.cancel()
        with self._lock:
            self.items.pop(item_id, None)
            if item_id in self._order:
                self._order.remove(item_id)
        if delete_file and it.dest_path:
            from . import storage
            for p in (it.dest_path, storage.sidecar_path(it.dest_path)):
                try:
                    if os.path.exists(p):
                        os.remove(p)
                except OSError:
                    pass
        self.save()
        self.pump()

    def clear_finished(self):
        for it in list(self.items.values()):
            if it.status == ST_COMPLETED:
                with self._lock:
                    self.items.pop(it.id, None)
                    if it.id in self._order:
                        self._order.remove(it.id)
        self.save()

    def pause_all(self):
        for it in self.ordered():
            self.pause(it.id)

    def resume_all(self):
        for it in self.ordered():
            if it.status in (ST_PAUSED, ST_ERROR):
                it.error = ""
                it.status = ST_QUEUED
        self.save()
        self.pump()
