"""Segment assembly and muxing to MP4 via ffmpeg.

After HLS/DASH segments are downloaded, they must be joined (and often
remuxed from .ts into a proper .mp4 container). We shell out to ffmpeg, which
is the standard tool for this. MPDM does not bundle ffmpeg; it uses whatever is
on PATH and tells the user clearly if it's missing.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from typing import Optional


def ffmpeg_path() -> Optional[str]:
    return shutil.which("ffmpeg")


def have_ffmpeg() -> bool:
    return ffmpeg_path() is not None


def concat_binary(segment_paths: list[str], out_path: str, init_path: Optional[str] = None) -> str:
    """Byte-concatenate segments (optionally prefixed by an init segment).

    For fragmented MP4 (init + m4s) and MPEG-TS, raw concatenation produces a
    valid elementary stream that ffmpeg can then remux losslessly.
    """
    with open(out_path, "wb") as out:
        if init_path:
            with open(init_path, "rb") as f:
                shutil.copyfileobj(f, out)
        for p in segment_paths:
            with open(p, "rb") as f:
                shutil.copyfileobj(f, out)
    return out_path


def remux_to_mp4(input_path: str, out_path: str, extra_args: Optional[list[str]] = None) -> str:
    """Remux a concatenated stream into MP4 without re-encoding (-c copy)."""
    if not have_ffmpeg():
        raise RuntimeError(
            "ffmpeg not found on PATH. Install it (https://ffmpeg.org/download.html) "
            "to mux stream segments into MP4."
        )
    cmd = [ffmpeg_path(), "-y", "-i", input_path, "-c", "copy",
           "-bsf:a", "aac_adtstoasc", out_path]
    if extra_args:
        cmd = [ffmpeg_path(), "-y", "-i", input_path, *extra_args, out_path]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg failed:\n{proc.stderr[-2000:]}")
    return out_path


def assemble_segments(segment_paths: list[str], out_mp4: str,
                      init_path: Optional[str] = None, cleanup: bool = True) -> str:
    """Concatenate downloaded segments and remux to a final MP4."""
    tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
    tmp.close()
    try:
        concat_binary(segment_paths, tmp.name, init_path=init_path)
        remux_to_mp4(tmp.name, out_mp4)
    finally:
        if os.path.exists(tmp.name):
            os.remove(tmp.name)
    if cleanup:
        for p in segment_paths:
            try:
                os.remove(p)
            except OSError:
                pass
        if init_path and os.path.exists(init_path):
            try:
                os.remove(init_path)
            except OSError:
                pass
    return out_mp4
