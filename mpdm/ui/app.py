"""MyPersonalDownloadManager — desktop GUI (download manager window).

An original Tkinter interface over mpdm.manager.DownloadManager: a toolbar, a
category sidebar, and a live table of downloads with per-row progress. Multiple
downloads run at once (bounded by a concurrency setting); each can be paused,
resumed or removed. No license or registration — it's free and open.

Run with:  python -m mpdm.ui.app
"""
from __future__ import annotations

import os
import threading

try:
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox, simpledialog
except Exception as e:  # pragma: no cover
    raise SystemExit(f"Tkinter is not available in this Python environment: {e}")

from ..config import Settings
from ..manager import (
    DownloadManager, CATEGORIES, classify,
    ST_DOWNLOADING, ST_PAUSED, ST_COMPLETED, ST_ERROR, ST_QUEUED,
)
from .. import __app_name__, __app_short__, __version__


def human(n) -> str:
    if n is None:
        return "—"
    n = float(n)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if abs(n) < 1024:
            return f"{n:.0f}{unit}" if unit == "B" else f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}PB"


def human_time(s) -> str:
    if s is None:
        return "—"
    s = int(s)
    h, rem = divmod(s, 3600)
    m, sec = divmod(rem, 60)
    if h:
        return f"{h}h {m:02d}m"
    if m:
        return f"{m}m {sec:02d}s"
    return f"{sec}s"


STATUS_LABEL = {
    ST_QUEUED: "Queued",
    ST_DOWNLOADING: "Downloading",
    ST_PAUSED: "Paused",
    ST_COMPLETED: "Complete",
    ST_ERROR: "Error",
}


class AddDialog(tk.Toplevel):
    """Modal: paste a URL (+ optional filename / connections)."""

    def __init__(self, parent, default_conn):
        super().__init__(parent)
        self.title("Add download")
        self.resizable(False, False)
        self.result = None
        self.transient(parent)
        self.grab_set()

        frm = ttk.Frame(self, padding=12)
        frm.pack(fill="both", expand=True)

        ttk.Label(frm, text="URL").grid(row=0, column=0, sticky="w")
        self.url = tk.StringVar()
        e = ttk.Entry(frm, textvariable=self.url, width=56)
        e.grid(row=1, column=0, columnspan=3, sticky="ew", pady=(2, 8))

        ttk.Label(frm, text="Save as (optional)").grid(row=2, column=0, sticky="w")
        self.fn = tk.StringVar()
        ttk.Entry(frm, textvariable=self.fn, width=40).grid(row=3, column=0, columnspan=2, sticky="ew", pady=(2, 8))

        ttk.Label(frm, text="Connections").grid(row=2, column=2, sticky="w")
        self.conn = tk.IntVar(value=default_conn)
        ttk.Spinbox(frm, from_=1, to=32, textvariable=self.conn, width=6).grid(row=3, column=2, sticky="e", pady=(2, 8))

        btns = ttk.Frame(frm)
        btns.grid(row=4, column=0, columnspan=3, sticky="e", pady=(4, 0))
        ttk.Button(btns, text="Cancel", command=self.destroy).pack(side="right", padx=4)
        ttk.Button(btns, text="Add", command=self._ok).pack(side="right")

        frm.columnconfigure(0, weight=1)
        # Prefill from clipboard if it looks like a URL.
        try:
            clip = parent.clipboard_get()
            if clip and clip.strip().lower().startswith(("http://", "https://", "ftp://")):
                self.url.set(clip.strip())
        except tk.TclError:
            pass
        e.focus_set()
        self.bind("<Return>", lambda _e: self._ok())
        self.bind("<Escape>", lambda _e: self.destroy())

    def _ok(self):
        url = self.url.get().strip()
        if not url:
            messagebox.showwarning(__app_name__, "Please enter a URL.", parent=self)
            return
        self.result = (url, self.fn.get().strip(), int(self.conn.get()))
        self.destroy()


class SettingsDialog(tk.Toplevel):
    def __init__(self, parent, mgr: DownloadManager):
        super().__init__(parent)
        self.title("Settings")
        self.resizable(False, False)
        self.mgr = mgr
        self.transient(parent)
        self.grab_set()
        frm = ttk.Frame(self, padding=12)
        frm.pack(fill="both", expand=True)

        ttk.Label(frm, text="Download folder").grid(row=0, column=0, sticky="w")
        self.dir = tk.StringVar(value=mgr.settings.output_dir)
        ttk.Entry(frm, textvariable=self.dir, width=44).grid(row=1, column=0, sticky="ew")
        ttk.Button(frm, text="Browse…", command=self._browse).grid(row=1, column=1, padx=(6, 0))

        ttk.Label(frm, text="Max simultaneous downloads").grid(row=2, column=0, sticky="w", pady=(10, 0))
        self.maxc = tk.IntVar(value=mgr.max_concurrent)
        ttk.Spinbox(frm, from_=1, to=10, textvariable=self.maxc, width=6).grid(row=3, column=0, sticky="w")

        ttk.Label(frm, text="Default connections per download").grid(row=2, column=1, sticky="w", pady=(10, 0))
        self.conn = tk.IntVar(value=mgr.settings.connections)
        ttk.Spinbox(frm, from_=1, to=32, textvariable=self.conn, width=6).grid(row=3, column=1, sticky="w")

        btns = ttk.Frame(frm)
        btns.grid(row=4, column=0, columnspan=2, sticky="e", pady=(12, 0))
        ttk.Button(btns, text="Cancel", command=self.destroy).pack(side="right", padx=4)
        ttk.Button(btns, text="Save", command=self._save).pack(side="right")
        frm.columnconfigure(0, weight=1)

    def _browse(self):
        d = filedialog.askdirectory(initialdir=self.dir.get() or os.getcwd(), parent=self)
        if d:
            self.dir.set(d)

    def _save(self):
        self.mgr.settings.output_dir = self.dir.get().strip() or self.mgr.settings.output_dir
        self.mgr.settings.connections = max(1, int(self.conn.get()))
        self.mgr.max_concurrent = max(1, int(self.maxc.get()))
        self.mgr.pump()
        self.destroy()


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(f"{__app_name__} ({__app_short__}) {__version__}")
        self.geometry("900x480")
        self.minsize(760, 400)

        self.mgr = DownloadManager(settings=Settings(), max_concurrent=3)
        self.filter = "All"

        self._build_toolbar()
        self._build_body()
        self._build_statusbar()
        self._refresh()

    # ---- UI construction -------------------------------------------------
    def _build_toolbar(self):
        bar = ttk.Frame(self, padding=(8, 6))
        bar.pack(fill="x")
        defs = [
            ("＋ Add URL", self.on_add),
            ("▶ Resume", self.on_resume),
            ("❚❚ Pause", self.on_pause),
            ("✕ Remove", self.on_remove),
            ("🗑 Clear finished", self.on_clear),
            ("📂 Open folder", self.on_open_folder),
            ("⚙ Settings", self.on_settings),
        ]
        for text, cmd in defs:
            ttk.Button(bar, text=text, command=cmd).pack(side="left", padx=2)
        ttk.Button(bar, text="⏸ Pause all", command=lambda: (self.mgr.pause_all(), self._refresh())).pack(side="right", padx=2)
        ttk.Button(bar, text="⏵ Resume all", command=lambda: (self.mgr.resume_all(), self._refresh())).pack(side="right", padx=2)

    def _build_body(self):
        body = ttk.Panedwindow(self, orient="horizontal")
        body.pack(fill="both", expand=True, padx=8, pady=(0, 6))

        # categories
        left = ttk.Frame(body, width=160)
        self.cats = tk.Listbox(left, activestyle="none", exportselection=False, borderwidth=0, highlightthickness=0)
        for c in CATEGORIES:
            self.cats.insert("end", c)
        self.cats.selection_set(0)
        self.cats.bind("<<ListboxSelect>>", self._on_cat)
        self.cats.pack(fill="both", expand=True)
        body.add(left, weight=0)

        # download table
        right = ttk.Frame(body)
        cols = ("name", "size", "pct", "status", "speed", "eta")
        self.tree = ttk.Treeview(right, columns=cols, show="headings", selectmode="extended")
        headings = {"name": "File name", "size": "Size", "pct": "%", "status": "Status",
                    "speed": "Speed", "eta": "Time left"}
        widths = {"name": 300, "size": 80, "pct": 55, "status": 100, "speed": 90, "eta": 90}
        for c in cols:
            self.tree.heading(c, text=headings[c])
            self.tree.column(c, width=widths[c], anchor=("w" if c == "name" else "center"))
        vs = ttk.Scrollbar(right, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vs.set)
        self.tree.pack(side="left", fill="both", expand=True)
        vs.pack(side="right", fill="y")
        body.add(right, weight=1)

        # row context menu + double-click
        self.menu = tk.Menu(self, tearoff=0)
        self.menu.add_command(label="Resume", command=self.on_resume)
        self.menu.add_command(label="Pause", command=self.on_pause)
        self.menu.add_separator()
        self.menu.add_command(label="Open folder", command=self.on_open_folder)
        self.menu.add_command(label="Copy URL", command=self.on_copy_url)
        self.menu.add_separator()
        self.menu.add_command(label="Remove from list", command=self.on_remove)
        self.menu.add_command(label="Remove + delete file", command=lambda: self.on_remove(delete=True))
        self.tree.bind("<Button-3>", self._popup)
        self.tree.bind("<Double-1>", lambda _e: self.on_open_folder())

    def _build_statusbar(self):
        self.status = tk.StringVar(value="Ready")
        ttk.Label(self, textvariable=self.status, anchor="w", relief="sunken", padding=(8, 3)).pack(fill="x")

    # ---- helpers ---------------------------------------------------------
    def _on_cat(self, _e):
        sel = self.cats.curselection()
        if sel:
            self.filter = CATEGORIES[sel[0]]

    def _popup(self, event):
        row = self.tree.identify_row(event.y)
        if row:
            if row not in self.tree.selection():
                self.tree.selection_set(row)
            self.menu.tk_popup(event.x_root, event.y_root)

    def _selected_ids(self):
        return list(self.tree.selection())

    def _visible_items(self):
        items = self.mgr.ordered()
        f = self.filter
        if f == "All":
            return items
        if f == "Downloading":
            return [i for i in items if i.status in (ST_DOWNLOADING, ST_QUEUED)]
        if f == "Finished":
            return [i for i in items if i.status == ST_COMPLETED]
        return [i for i in items if i.category == f]

    # ---- actions ---------------------------------------------------------
    def on_add(self):
        dlg = AddDialog(self, self.mgr.settings.connections)
        self.wait_window(dlg)
        if dlg.result:
            url, fn, conn = dlg.result
            self.mgr.add(url, filename=fn, connections=conn, start=True)
            self._refresh()

    def on_resume(self):
        for i in self._selected_ids():
            self.mgr.resume(i)
        self._refresh()

    def on_pause(self):
        for i in self._selected_ids():
            self.mgr.pause(i)
        self._refresh()

    def on_remove(self, delete=False):
        ids = self._selected_ids()
        if not ids:
            return
        if delete and not messagebox.askyesno(__app_name__,
                                              f"Delete {len(ids)} file(s) from disk?"):
            return
        for i in ids:
            self.mgr.remove(i, delete_file=delete)
        self._refresh()

    def on_clear(self):
        self.mgr.clear_finished()
        self._refresh()

    def on_open_folder(self):
        ids = self._selected_ids()
        if not ids:
            return
        it = self.mgr.items.get(ids[0])
        if not it or not it.dest_path:
            return
        folder = os.path.dirname(it.dest_path)
        try:
            if os.name == "nt":
                os.startfile(folder)  # noqa: S606
            elif sys_is_mac():
                os.system(f'open "{folder}"')
            else:
                os.system(f'xdg-open "{folder}"')
        except Exception:
            pass

    def on_copy_url(self):
        ids = self._selected_ids()
        if not ids:
            return
        it = self.mgr.items.get(ids[0])
        if it:
            self.clipboard_clear()
            self.clipboard_append(it.url)

    def on_settings(self):
        dlg = SettingsDialog(self, self.mgr)
        self.wait_window(dlg)
        self._refresh()

    # ---- live refresh ----------------------------------------------------
    def _refresh(self):
        visible = self._visible_items()
        existing = set(self.tree.get_children())
        wanted = []
        for it in visible:
            wanted.append(it.id)
            vals = (
                it.filename or it.url,
                human(it.size),
                f"{it.percent:.0f}%" if it.percent is not None else "—",
                STATUS_LABEL.get(it.status, it.status),
                (human(it.speed) + "/s") if it.status == ST_DOWNLOADING and it.speed else "—",
                human_time(it.eta) if it.status == ST_DOWNLOADING else "—",
            )
            if it.id in existing:
                self.tree.item(it.id, values=vals)
            else:
                self.tree.insert("", "end", iid=it.id, values=vals)
        for rid in existing:
            if rid not in wanted:
                self.tree.delete(rid)

        total = len(self.mgr.items)
        active = self.mgr.active_count()
        done = sum(1 for i in self.mgr.items.values() if i.status == ST_COMPLETED)
        agg_speed = sum(i.speed for i in self.mgr.items.values() if i.status == ST_DOWNLOADING)
        self.status.set(f"{total} downloads · {active} active · {done} finished · "
                        f"{human(agg_speed)}/s total")
        self.after(500, self._refresh)


def sys_is_mac() -> bool:
    import sys
    return sys.platform == "darwin"


def main():
    App().mainloop()


if __name__ == "__main__":
    main()
