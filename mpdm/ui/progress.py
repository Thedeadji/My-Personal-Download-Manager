"""IDM-style per-download progress popup (launched by the native host).

When a download is sent from the browser extension, the native host launches
this small window for it:  python -m mpdm.ui.progress <job.json>

The job file is JSON written by the host:
    {"url": "...", "filename": "...", "headers": {...},
     "connections": 8, "output_dir": "..."}

The popup runs the download itself (engine.Download, with the browser's cookies
/ referer / user-agent in `headers`) and shows filename, a progress bar, speed,
downloaded / total, and Stop/Resume — resume continues from the .mpdm sidecar.
It is a standalone process, so it stays open after the host exits.
"""
from __future__ import annotations

import json
import os
import sys
import threading
import tkinter as tk
from tkinter import ttk

from ..config import Settings
from ..engine import Download, DownloadError
from ..models import Progress


def _human(n) -> str:
    if n is None:
        return "?"
    n = float(n)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if abs(n) < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} PB"


def _human_time(s) -> str:
    if s is None:
        return "?"
    s = int(s)
    h, rem = divmod(s, 3600)
    m, sec = divmod(rem, 60)
    if h:
        return f"{h}h {m:02d}m"
    if m:
        return f"{m}m {sec:02d}s"
    return f"{sec}s"


class ProgressPopup:
    def __init__(self, root: tk.Tk, job: dict):
        self.root = root
        self.url = job.get("url", "")
        self._job = job

        # Build settings from the job (connections, output dir, browser headers).
        self.settings = Settings.from_env()
        if job.get("connections"):
            try:
                self.settings.connections = max(1, int(job["connections"]))
            except (TypeError, ValueError):
                pass
        if job.get("output_dir"):
            self.settings.output_dir = job["output_dir"]
        self.headers = job.get("headers") or {}

        # Resolve a destination up front if the browser supplied a filename.
        self._dest = None
        if job.get("filename"):
            from ..http_client import sanitize_filename
            from ..categories import categorized_dir
            from .. import storage
            try:
                fname = sanitize_filename(job["filename"])
                out_dir = categorized_dir(self.settings.output_dir, fname,
                                          self.settings.categorize)
                os.makedirs(out_dir, exist_ok=True)
                self._dest = storage.unique_dest(out_dir, fname)
            except OSError:
                self._dest = None

        self._lock = threading.Lock()
        self._latest = None
        self._dl = None
        self._state = "running"       # running | paused | done | error
        self._message = ""

        root.title("MPDM — downloading")
        root.geometry("440x170")
        root.minsize(380, 150)
        self._build()
        self._begin()
        self.root.after(200, self._refresh)

    def _build(self):
        pad = {"padx": 12}
        self.name_lbl = ttk.Label(self.root, text=os.path.basename(self._dest) if self._dest
                                  else "(resolving…)", font=("", 10, "bold"))
        self.name_lbl.pack(anchor="w", pady=(12, 2), **pad)
        self.bar = ttk.Progressbar(self.root, mode="indeterminate", maximum=100)
        self.bar.pack(fill="x", pady=2, **pad)
        self.bar.start(60)
        self.info = ttk.Label(self.root, text="Starting…")
        self.info.pack(anchor="w", **pad)
        self.sub = ttk.Label(self.root, text="", foreground="#555")
        self.sub.pack(anchor="w", **pad)

        btns = ttk.Frame(self.root)
        btns.pack(fill="x", pady=12, **pad)
        self.pause_btn = ttk.Button(btns, text="Stop", command=self.on_pause_resume)
        self.pause_btn.pack(side="left")
        self.action_btn = ttk.Button(btns, text="Open folder", command=self.on_open_folder,
                                     state="disabled")
        self.action_btn.pack(side="left", padx=6)
        ttk.Button(btns, text="Close", command=self.root.destroy).pack(side="right")

    # ---- download control ------------------------------------------------
    def _begin(self):
        self._state = "running"
        self.pause_btn.config(text="Stop", state="normal")

        def run():
            def cb(p: Progress):
                with self._lock:
                    self._latest = p
            dl = Download(self.url, settings=self.settings, dest_path=self._dest,
                          progress_cb=cb, headers=self.headers)
            self._dl = dl
            try:
                path = dl.start()
                self._finish("done", f"Completed — {path}")
            except DownloadError as e:
                if "paused" in str(e):
                    self._finish("paused", "Stopped.")
                elif "cancelled" in str(e):
                    self._finish("error", "Cancelled.")
                else:
                    self._finish("error", f"Error: {e}")
            except Exception as e:  # noqa: BLE001
                self._finish("error", f"Error: {e}")

        threading.Thread(target=run, daemon=True).start()

    def on_pause_resume(self):
        if self._state == "running" and self._dl is not None:
            self._dl.pause()
            self._state = "paused"
            self.pause_btn.config(text="Resume")
        elif self._state == "paused":
            self._begin()

    def on_open_folder(self):
        path = self._dest or (self._dl.dest_path if self._dl else None)
        if not path:
            return
        folder = os.path.dirname(path)
        try:
            if os.name == "nt":
                os.startfile(folder)  # noqa: S606
            elif sys.platform == "darwin":
                os.system(f'open "{folder}"')
            else:
                os.system(f'xdg-open "{folder}"')
        except Exception:
            pass

    def _finish(self, state: str, message: str):
        with self._lock:
            self._state = state
            self._message = message

    # ---- periodic UI update ---------------------------------------------
    def _refresh(self):
        with self._lock:
            p = self._latest
            state = self._state
            message = self._message
            self._message = ""
        if self._dl is not None and self._dl.dest_path:
            self._dest = self._dl.dest_path
            self.name_lbl.config(text=os.path.basename(self._dl.dest_path))

        if p is not None and state in ("running", "paused"):
            pct = p.percent
            total = _human(p.total) if p.total else "?"
            if pct is not None:
                if self.bar["mode"] != "determinate":
                    self.bar.stop()
                    self.bar.config(mode="determinate")
                self.bar["value"] = pct
                self.info.config(text=f"{_human(p.downloaded)} of {total}  ({pct:.1f}%)")
            else:
                self.info.config(text=f"{_human(p.downloaded)} downloaded")
            if state == "running":
                self.sub.config(text=f"{_human(p.speed_bps)}/s   "
                                     f"{p.active_segments} connections   "
                                     f"ETA {_human_time(p.eta_seconds)}")
            else:
                self.sub.config(text="Stopped")

        if message:
            self.info.config(text=message)

        if state == "done":
            self.bar.stop()
            self.bar.config(mode="determinate")
            self.bar["value"] = 100
            self.sub.config(text="Done")
            self.pause_btn.config(state="disabled")
            self.action_btn.config(state="normal")
        elif state == "error":
            self.bar.stop()
            self.pause_btn.config(text="Retry", state="normal", command=self._retry)
        elif state == "paused":
            self.pause_btn.config(text="Resume", state="normal")

        self.root.after(200, self._refresh)

    def _retry(self):
        self.pause_btn.config(command=self.on_pause_resume)
        self._begin()


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        print("usage: python -m mpdm.ui.progress <job.json>", file=sys.stderr)
        return 2
    job_path = argv[0]
    try:
        with open(job_path, "r", encoding="utf-8") as f:
            job = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"could not read job file: {e}", file=sys.stderr)
        return 2
    finally:
        # The job file may hold cookies; remove it as soon as it's read.
        try:
            os.remove(job_path)
        except OSError:
            pass

    root = tk.Tk()
    ProgressPopup(root, job)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
