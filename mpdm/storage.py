"""Resume metadata: a small JSON sidecar next to the output file.

When a download is interrupted, MPDM can pick up where it left off because it
persists each segment's progress. The sidecar is named `<file>.mpdm` and is
removed once the download completes successfully.
"""
from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path
from typing import Optional

from .models import TaskRecord, Segment


def sidecar_path(dest_path: str) -> str:
    return dest_path + ".mpdm"


def save_record(dest_path: str, record: TaskRecord) -> None:
    """Atomically write the sidecar so a crash can't corrupt it."""
    path = sidecar_path(dest_path)
    d = os.path.dirname(path) or "."
    fd, tmp = tempfile.mkstemp(dir=d, suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(record.to_dict(), f, indent=2)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def load_record(dest_path: str) -> Optional[TaskRecord]:
    path = sidecar_path(dest_path)
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        rec = TaskRecord(**{k: v for k, v in data.items() if k in TaskRecord.__dataclass_fields__})
        return rec
    except (json.JSONDecodeError, TypeError, ValueError):
        return None


def clear_record(dest_path: str) -> None:
    path = sidecar_path(dest_path)
    if os.path.exists(path):
        os.remove(path)


def segments_from_record(rec: TaskRecord) -> list[Segment]:
    out = []
    for s in rec.segments:
        out.append(Segment(index=s["index"], start=s["start"], end=s["end"], downloaded=s.get("downloaded", 0)))
    return out


def segments_to_dicts(segments: list[Segment]) -> list[dict]:
    return [{"index": s.index, "start": s.start, "end": s.end, "downloaded": s.downloaded} for s in segments]


def find_resumable(output_dir: str, url: str, size, etag) -> Optional[str]:
    """Look for an interrupted download of the SAME remote we can resume.

    Scans sidecars in `output_dir` and returns the dest_path of one whose
    url/size/etag match and whose target file still exists and is unfinished.
    This stops re-runs from piling up (1), (2), ... copies.
    """
    import glob
    for side in glob.glob(os.path.join(output_dir, "*.mpdm")):
        dest = side[:-len(".mpdm")]
        rec = load_record(dest)
        if not rec:
            continue
        if rec.url == url and rec.size == size and rec.etag == etag \
                and rec.status != "completed" and os.path.exists(dest):
            return dest
    return None


def clean_dir(output_dir: str, purge_partials: bool = False) -> dict:
    """Remove orphaned resume sidecars (and .tmp checkpoints) in a directory.

    With purge_partials=True, also delete the incomplete target file each stale
    sidecar describes. Returns a summary of what was removed/kept.
    """
    import glob
    removed_sidecars, removed_partials, removed_tmp, kept = [], [], [], []
    for side in glob.glob(os.path.join(output_dir, "*.mpdm")):
        dest = side[:-len(".mpdm")]
        rec = load_record(dest)
        incomplete = True
        if rec:
            try:
                segs = segments_from_record(rec)
                incomplete = not (segs and all(s.complete for s in segs))
            except Exception:
                incomplete = True
        if purge_partials and incomplete and os.path.exists(dest):
            try:
                os.remove(dest)
                removed_partials.append(dest)
            except OSError:
                kept.append(dest)
        try:
            os.remove(side)
            removed_sidecars.append(side)
        except OSError:
            kept.append(side)
    for tmp in glob.glob(os.path.join(output_dir, "*.tmp")):
        try:
            os.remove(tmp)
            removed_tmp.append(tmp)
        except OSError:
            kept.append(tmp)
    return {
        "sidecars": removed_sidecars,
        "partials": removed_partials,
        "tmp": removed_tmp,
        "kept_locked": kept,
    }


def unique_dest(output_dir: str, filename: str) -> str:
    """Avoid clobbering an existing finished file: foo.zip -> foo (1).zip."""
    base = Path(output_dir) / filename
    if not base.exists() and not Path(sidecar_path(str(base))).exists():
        return str(base)
    stem, ext = os.path.splitext(filename)
    i = 1
    while True:
        cand = Path(output_dir) / f"{stem} ({i}){ext}"
        if not cand.exists() and not Path(sidecar_path(str(cand))).exists():
            return str(cand)
        i += 1
