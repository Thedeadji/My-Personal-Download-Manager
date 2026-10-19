"""Native messaging host for the MyPersonalDownloadManager browser extension.

Browsers (Chrome, Edge, Firefox) talk to a native program using a very simple
protocol on stdin/stdout: each message is a 4-byte little-endian length prefix
followed by that many bytes of UTF-8 JSON. The browser launches this script; we
read a download request, run it through the MPDM engine, and stream status
messages back the same way.

Message from extension -> host:
    {"type": "download",
     "url": "https://...",
     "filename": "optional.ext",
     "headers": {"Cookie": "...", "Referer": "...", "User-Agent": "..."},
     "connections": 8}

Messages host -> extension:
    {"type": "accepted", "id": "<id>", "filename": "..."}
    {"type": "progress", "id": "<id>", "percent": 42.0, "speed": 1234567,
     "downloaded": 123, "total": 456}
    {"type": "done", "id": "<id>", "path": "C:\\Users\\..\\Downloads\\file"}
    {"type": "error", "id": "<id>", "message": "..."}

Run standalone for a quick self-test:  python -m mpdm.native_host --selftest
"""
from __future__ import annotations

import json
import os
import struct
import sys
import threading
import time
import uuid
from typing import Optional

from .config import Settings, state_dir
from .engine import Download
from .models import Progress, Status


# ---- framing -------------------------------------------------------------
def read_message(stream=None) -> Optional[dict]:
    """Read one length-prefixed JSON message. Returns None on EOF."""
    stream = stream or sys.stdin.buffer
    raw_len = stream.read(4)
    if len(raw_len) < 4:
        return None
    (msg_len,) = struct.unpack("<I", raw_len)
    data = stream.read(msg_len)
    if len(data) < msg_len:
        return None
    return json.loads(data.decode("utf-8"))


_write_lock = threading.Lock()


def write_message(obj: dict, stream=None) -> None:
    """Write one length-prefixed JSON message (thread-safe)."""
    stream = stream or sys.stdout.buffer
    data = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    with _write_lock:
        stream.write(struct.pack("<I", len(data)))
        stream.write(data)
        stream.flush()


# ---- logging (browsers hide the host's stderr; log to a file) ------------
def _log(msg: str) -> None:
    try:
        with open(state_dir() / "native_host.log", "a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')}  {msg}\n")
    except OSError:
        pass


# ---- job handling --------------------------------------------------------
def _run_job(job: dict) -> None:
    job_id = job.get("id") or uuid.uuid4().hex[:12]
    url = job.get("url")
    if not url:
        write_message({"type": "error", "id": job_id, "message": "no url"})
        return

    settings = Settings.from_env()
    if job.get("connections"):
        try:
            settings.connections = max(1, int(job["connections"]))
        except (TypeError, ValueError):
            pass
    if job.get("output_dir"):
        settings.output_dir = job["output_dir"]

    dest = None
    if job.get("filename"):
        from .http_client import sanitize_filename
        os.makedirs(settings.output_dir, exist_ok=True)
        from . import storage
        dest = storage.unique_dest(settings.output_dir, sanitize_filename(job["filename"]))

    headers = job.get("headers") or {}

    # Throttle progress messages to keep the pipe light.
    last_emit = [0.0]

    def on_progress(p: Progress):
        now = time.time()
        if p.status not in (Status.COMPLETED, Status.FAILED) and (now - last_emit[0]) < 0.3:
            return
        last_emit[0] = now
        write_message({
            "type": "progress",
            "id": job_id,
            "percent": p.percent,
            "speed": p.speed_bps,
            "downloaded": p.downloaded,
            "total": p.total,
            "status": p.status.value,
            "filename": p.filename,
        })

    try:
        dl = Download(url, settings=settings, dest_path=dest,
                      progress_cb=on_progress, headers=headers)
        write_message({"type": "accepted", "id": job_id,
                       "filename": os.path.basename(dest) if dest else ""})
        path = dl.start()
        write_message({"type": "done", "id": job_id, "path": path})
        _log(f"done {job_id} -> {path}")
    except Exception as e:  # noqa: BLE001
        write_message({"type": "error", "id": job_id, "message": str(e)})
        _log(f"error {job_id}: {e}")


def serve() -> int:
    """Main loop: read jobs from the browser until the pipe closes."""
    _log("host started")
    threads = []
    while True:
        try:
            msg = read_message()
        except Exception as e:  # noqa: BLE001
            _log(f"read error: {e}")
            break
        if msg is None:
            break
        if msg.get("type") == "ping":
            write_message({"type": "pong"})
            continue
        if msg.get("type") == "download":
            t = threading.Thread(target=_run_job, args=(msg,), daemon=True)
            t.start()
            threads.append(t)
        else:
            write_message({"type": "error", "message": f"unknown type: {msg.get('type')}"})
    # Let in-flight downloads finish before exiting.
    for t in threads:
        t.join(timeout=0.1)
    _log("host stopping")
    return 0


def _selftest() -> int:
    """Round-trip the framing without a browser: encode a message, decode it."""
    import io
    buf = io.BytesIO()
    write_message({"type": "download", "url": "https://example.com/x", "connections": 4}, stream=buf)
    buf.seek(0)
    got = read_message(stream=buf)
    assert got["type"] == "download" and got["connections"] == 4, got
    print("native messaging framing OK:", got)
    return 0


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if "--selftest" in argv:
        return _selftest()
    return serve()


if __name__ == "__main__":
    raise SystemExit(main())
