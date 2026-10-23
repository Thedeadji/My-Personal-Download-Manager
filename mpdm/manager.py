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

from .config import Settings, state_dir, load_prefs, save_prefs, settings_from_prefs
from .engine import Download, DownloadError
from .models import Progress, Status
from .ratelimit import RateLimiter
from .scheduler import Scheduler
from .batch import expand_urls


# Categories live in categories.py; re-exported here for existing importers.
from .categories import CATEGORY_EXT, CATEGORIES, classify  # noqa: F401,E402


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
    max_rate_bps: int = 0          # per-download speed limit (0 = unlimited)
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
            "max_rate_bps": self.max_rate_bps,
            "headers": dict(self.headers), "error": self.error, "added_at": self.added_at,
        }


class DownloadManager:
    def __init__(self, settings: Optional[Settings] = None, max_concurrent: int = 3,
                 global_rate_bps: int = 0):
        prefs = load_prefs()
        self.settings = settings or settings_from_prefs(prefs)
        self.max_concurrent = int(prefs.get("max_concurrent", max_concurrent))
        # Remembered per-session UI pref the GUI reads (clipboard monitoring).
        self.clip_monitor_pref = bool(prefs.get("clip_monitor", False))
        self.items: dict[str, DownloadItem] = {}
        self._order: list[str] = []
        self._lock = threading.RLock()
        self._store = state_dir() / "downloads.json"
        # Shared across every running download so their combined rate is capped.
        self._global_limiter = RateLimiter(prefs.get("global_rate_bps", global_rate_bps))
        # Time-based scheduler (pause/resume the queue, change the global rate).
        # Created and loaded here but not started until start_scheduler().
        self.scheduler = Scheduler(
            actions={
                "pause_all": lambda r: self.pause_all(),
                "resume_all": lambda r: self.resume_all(),
                "set_global_rate": lambda r: self.set_global_rate(r.rate_bps),
            },
            store=state_dir() / "schedule.json",
        )
        self.scheduler.load()
        # Action to run once the whole queue finishes: none | run | sleep | shutdown.
        self.on_done_action = prefs.get("on_done_action", "none")
        self.on_done_command = prefs.get("on_done_command", "")
        self._on_complete_fired = False
        self.load()

    def save_settings(self):
        """Persist settings + manager-level prefs to settings.json."""
        save_prefs({
            "settings": self.settings.as_dict(),
            "max_concurrent": self.max_concurrent,
            "global_rate_bps": self.global_rate_bps,
            "on_done_action": self.on_done_action,
            "on_done_command": self.on_done_command,
            "clip_monitor": getattr(self, "clip_monitor_pref", False),
        })

    # ---- "when everything finishes" action -------------------------------
    def set_on_complete(self, action: str, command: str = ""):
        """Run a program / sleep / shut down when all downloads finish."""
        self.on_done_action = action if action in ("none", "run", "sleep", "shutdown") else "none"
        self.on_done_command = command or ""
        self._on_complete_fired = False

    def _queue_all_done(self) -> bool:
        """True if nothing is active/queued and at least one download completed."""
        statuses = [it.status for it in self.items.values()]
        if any(s in (ST_DOWNLOADING, ST_QUEUED) for s in statuses):
            return False
        return any(s == ST_COMPLETED for s in statuses)

    def _maybe_on_complete(self):
        if self.on_done_action == "none":
            return
        with self._lock:
            if self._on_complete_fired or not self._queue_all_done():
                return
            self._on_complete_fired = True
            action, cmd = self.on_done_action, self.on_done_command
        self._run_completion_action(action, cmd)

    def _run_completion_action(self, action: str, command: str):
        import subprocess
        import sys
        try:
            if action == "run" and command:
                subprocess.Popen(command, shell=True)
            elif action == "shutdown":
                if os.name == "nt":
                    subprocess.Popen(["shutdown", "/s", "/t", "60"])  # cancel with: shutdown /a
                elif sys.platform == "darwin":
                    subprocess.Popen(["osascript", "-e",
                                      'tell application "System Events" to shut down'])
                else:
                    subprocess.Popen(["shutdown", "-h", "+1"])
            elif action == "sleep":
                if os.name == "nt":
                    subprocess.Popen(["rundll32.exe", "powrprof.dll,SetSuspendState", "0,1,0"])
                elif sys.platform == "darwin":
                    subprocess.Popen(["pmset", "sleepnow"])
                else:
                    subprocess.Popen(["systemctl", "suspend"])
        except Exception:  # noqa: BLE001
            pass

    # ---- scheduling (thin wrappers over self.scheduler) ------------------
    def start_scheduler(self):
        self.scheduler.start()

    def stop_scheduler(self):
        self.scheduler.stop()

    def add_schedule(self, action: str, at: str, repeat: str = "daily",
                     days=None, rate_bps: int = 0):
        """e.g. add_schedule('resume_all','01:00'); add_schedule('pause_all','07:00')."""
        return self.scheduler.add_rule(action, at, repeat=repeat, days=days,
                                       rate_bps=rate_bps)

    def remove_schedule(self, rule_id: str):
        self.scheduler.remove_rule(rule_id)

    def set_schedule_enabled(self, rule_id: str, enabled: bool):
        self.scheduler.set_enabled(rule_id, enabled)

    def list_schedules(self):
        return self.scheduler.list_rules()

    # ---- speed limits ----------------------------------------------------
    @property
    def global_rate_bps(self) -> float:
        return self._global_limiter.rate

    def set_global_rate(self, rate_bps: float):
        """Set the combined cap across all downloads (0 = unlimited); live."""
        self._global_limiter.set_rate(rate_bps)

    def set_item_rate(self, item_id: str, rate_bps: int):
        """Set one download's limit (0 = unlimited); applies live if running."""
        it = self.items.get(item_id)
        if not it:
            return
        it.max_rate_bps = max(0, int(rate_bps))
        if it._dl is not None:
            it._dl.set_rate(it.max_rate_bps)
        self.save()

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
            headers: Optional[dict] = None, start: bool = True,
            max_rate_bps: int = 0) -> DownloadItem:
        item = DownloadItem(
            id=uuid.uuid4().hex[:12],
            url=url.strip(),
            filename=filename or "",
            connections=connections,
            max_rate_bps=max(0, int(max_rate_bps)),
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

    def add_batch(self, text: str, connections: Optional[int] = None,
                  headers: Optional[dict] = None, start: bool = True,
                  max_rate_bps: int = 0) -> list:
        """Expand a pattern / list of URLs and queue each (IDM-style batch add)."""
        items = []
        for url in expand_urls(text):
            items.append(self.add(url, connections=connections, headers=headers,
                                  start=start, max_rate_bps=max_rate_bps))
        return items

    def pump(self):
        """Start queued downloads up to the concurrency limit."""
        with self._lock:
            if self.active_count() >= self.max_concurrent:
                return
            slots = self.max_concurrent - self.active_count()
            to_start = [it for it in self.ordered() if it.status == ST_QUEUED][:slots]
            if to_start:
                self._on_complete_fired = False   # new work -> allow the action to fire again
            for it in to_start:
                it.status = ST_DOWNLOADING
                t = threading.Thread(target=self._run, args=(it,), daemon=True)
                t.start()

    def _run(self, item: DownloadItem):
        settings = Settings(**self.settings.as_dict())
        if item.connections:
            settings.connections = item.connections
        # Per-item limit overrides the manager default; global cap is separate.
        if item.max_rate_bps:
            settings.max_rate_bps = item.max_rate_bps

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
                      progress_cb=cb, headers=item.headers,
                      global_limiter=self._global_limiter)
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
            self._maybe_on_complete()

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
