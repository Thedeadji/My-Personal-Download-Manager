"""A minimal, temporary download window — paste a URL, Start, Pause/Resume.

This is intentionally throwaway (not the full manager GUI in app.py): one URL at
a time, a live KB/MB + speed readout, and pause/resume backed by the engine's
sidecar resume. Run it with:

    python -m mpdm.ui.simple

Design notes:
- The download runs on a worker thread; its progress callback only stashes the
  latest snapshot. Tk widgets are touched solely from the main thread, via a
  periodic root.after() poll — the safe way to bridge threads in Tkinter.
- Pause calls Download.pause() (the engine checkpoints to the .mpdm sidecar and
  stops). Resume just starts a fresh Download at the same dest_path, which the
  engine continues from the sidecar instead of restarting.
"""
from __future__ import annotations

import threading
import tkinter as tk
from tkinter import ttk

from ..config import Settings, BROWSER_USER_AGENT
from ..engine import Download, DownloadError
from ..models import Progress, Status


def _human(n: float) -> str:
    n = float(n)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if abs(n) < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} PB"


class SimpleDownloader:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("MPDM — quick download")
        root.geometry("560x200")
        root.minsize(480, 200)

        self._lock = threading.Lock()
        self._latest: Progress | None = None
        self._dl: Download | None = None
        self._dest: str | None = None
        self._worker: threading.Thread | None = None
        self._state = "idle"      # idle | running | paused | done | error
        self._message = ""

        self._build()
        self.root.after(200, self._refresh)

    # ---- layout ----------------------------------------------------------
    def _build(self):
        pad = {"padx": 10, "pady": 6}
        top = ttk.Frame(self.root)
        top.pack(fill="x", **pad)
        ttk.Label(top, text="URL:").pack(side="left")
        self.url_var = tk.StringVar()
        self.url_entry = ttk.Entry(top, textvariable=self.url_var)
        self.url_entry.pack(side="left", fill="x", expand=True, padx=(6, 6))
        ttk.Label(top, text="conns:").pack(side="left")
        self.conn_var = tk.StringVar(value="8")
        ttk.Spinbox(top, from_=1, to=32, width=4, textvariable=self.conn_var).pack(side="left")

        opts = ttk.Frame(self.root)
        opts.pack(fill="x", padx=10)
        self.ua_browser = tk.BooleanVar(value=False)
        ttk.Checkbutton(opts, text="Send browser User-Agent (for sites that block non-browsers)",
                        variable=self.ua_browser).pack(side="left")

        self.bar = ttk.Progressbar(self.root, mode="determinate", maximum=100)
        self.bar.pack(fill="x", padx=10, pady=(4, 2))

        self.info = ttk.Label(self.root, text="Idle — paste a URL and press Start.")
        self.info.pack(fill="x", padx=10, pady=(0, 2))
        self.sub = ttk.Label(self.root, text="", foreground="#555")
        self.sub.pack(fill="x", padx=10)

        btns = ttk.Frame(self.root)
        btns.pack(fill="x", **pad)
        self.start_btn = ttk.Button(btns, text="Start", command=self.on_start)
        self.start_btn.pack(side="left")
        self.pause_btn = ttk.Button(btns, text="Pause", command=self.on_pause_resume,
                                    state="disabled")
        self.pause_btn.pack(side="left", padx=6)

    # ---- actions ---------------------------------------------------------
    def on_start(self):
        if self._state == "running":
            return
        url = self.url_var.get().strip()
        if not url:
            self._set_message("Enter a URL first.")
            return
        self._dest = None            # fresh download -> let the engine pick the path
        self._begin(url)

    def on_pause_resume(self):
        if self._state == "running" and self._dl is not None:
            self._dl.pause()         # engine checkpoints + stops; worker will end
            self._state = "paused"
            self.pause_btn.config(text="Resume")
            self._set_message("Pausing…")
        elif self._state == "paused":
            url = self.url_var.get().strip()
            self._begin(url)         # resumes at self._dest via the sidecar

    def _begin(self, url: str):
        try:
            conns = max(1, int(self.conn_var.get()))
        except ValueError:
            conns = 8
        settings = Settings(connections=conns)
        if self.ua_browser.get():
            settings.user_agent = BROWSER_USER_AGENT
        dest = self._dest
        self._state = "running"
        self.start_btn.config(state="disabled")
        self.pause_btn.config(state="normal", text="Pause")
        self._set_message("Starting…")

        def run():
            def cb(p: Progress):
                with self._lock:
                    self._latest = p
            dl = Download(url, settings=settings, dest_path=dest, progress_cb=cb)
            self._dl = dl
            try:
                path = dl.start()
                self._finish("done", f"Completed — saved to {path}")
            except DownloadError as e:
                if "paused" in str(e):
                    self._finish("paused", "Paused.")
                elif "cancelled" in str(e):
                    self._finish("idle", "Cancelled.")
                else:
                    self._finish("error", f"Error: {e}")
            except Exception as e:  # noqa: BLE001
                self._finish("error", f"Error: {e}")

        self._worker = threading.Thread(target=run, daemon=True)
        self._worker.start()

    def _finish(self, state: str, message: str):
        # Called from the worker thread; only stash state for the UI poll to apply.
        with self._lock:
            self._state = state
            self._message = message

    # ---- periodic UI update (main thread) --------------------------------
    def _set_message(self, text: str):
        self.info.config(text=text)

    def _refresh(self):
        with self._lock:
            p = self._latest
            state = self._state
            message = self._message
            self._message = ""
        # Capture the chosen path once the engine resolves it (for resume).
        if self._dl is not None and self._dl.dest_path:
            self._dest = self._dl.dest_path

        if p is not None and state in ("running", "paused"):
            pct = p.percent
            total = _human(p.total) if p.total else "?"
            name = p.filename or "(resolving…)"
            if pct is not None:
                self.bar.config(mode="determinate")
                self.bar["value"] = pct
                self.info.config(text=f"{name}  —  {_human(p.downloaded)} / {total}  ({pct:.1f}%)")
            else:
                self.bar.config(mode="indeterminate")
                self.info.config(text=f"{name}  —  {_human(p.downloaded)} downloaded")
            eta = f"  ETA {int(p.eta_seconds)}s" if p.eta_seconds else ""
            self.sub.config(text=f"{_human(p.speed_bps)}/s   {p.active_segments} connections{eta}"
                                 if state == "running" else "Paused")

        if message:
            self.info.config(text=message)

        if state in ("done", "error", "idle"):
            self.start_btn.config(state="normal")
            self.pause_btn.config(state="disabled", text="Pause")
            if state == "done":
                self.bar["value"] = 100
                self.sub.config(text="")
        elif state == "paused":
            self.start_btn.config(state="normal")
            self.pause_btn.config(state="normal", text="Resume")

        self.root.after(200, self._refresh)


def main():
    root = tk.Tk()
    SimpleDownloader(root)
    root.mainloop()


if __name__ == "__main__":
    main()
