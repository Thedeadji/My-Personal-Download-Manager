"""File-type categories and IDM-style download subfolders.

Two related jobs live here:
  * classify(filename) -> a category used by the manager/UI sidebar
    ("Video", "Audio", "Documents", "Archives", "Programs", "Other").
  * categorized_dir(base, filename) -> the actual folder a download is saved in,
    routing by type into IDM-style subfolders (Video, Music, Documents,
    Compressed, Programs). "Other" stays in the base download folder.

Kept in its own module so both the engine and the manager can use it without an
import cycle.
"""
from __future__ import annotations

import os

# Extension -> category. Lowercase, no leading dot.
CATEGORY_EXT = {
    "Video": {"mp4", "mkv", "webm", "avi", "mov", "flv", "m4v", "ts", "wmv", "mpg", "mpeg"},
    "Audio": {"mp3", "m4a", "aac", "flac", "wav", "ogg", "opus", "wma"},
    "Documents": {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "epub", "csv"},
    "Archives": {"zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "iso", "cab"},
    "Programs": {"exe", "msi", "msix", "dmg", "pkg", "deb", "rpm", "apk", "appimage"},
}

# Sidebar filter list used by the GUI.
CATEGORIES = ["All", "Downloading", "Finished",
              "Video", "Audio", "Documents", "Archives", "Programs", "Other"]

# Category -> on-disk subfolder name (IDM-style). "" means "leave in base dir".
CATEGORY_FOLDER = {
    "Video": "Video",
    "Audio": "Music",
    "Documents": "Documents",
    "Archives": "Compressed",
    "Programs": "Programs",
    "Other": "",
}


def classify(filename: str) -> str:
    ext = os.path.splitext(filename or "")[1].lower().lstrip(".")
    for cat, exts in CATEGORY_EXT.items():
        if ext in exts:
            return cat
    return "Other"


def category_subdir(filename: str) -> str:
    """The subfolder name for a file's type, or "" for uncategorized."""
    return CATEGORY_FOLDER.get(classify(filename), "")


def categorized_dir(base_dir: str, filename: str, enabled: bool = True,
                    overrides: dict = None) -> str:
    """Resolve the save directory for a file.

    When `enabled`, route by file type: a custom path from `overrides` (keyed by
    category name, e.g. {"Video": "D:/Movies"}; absolute used as-is, relative
    joined to base_dir), else base_dir/<default subfolder>. "Other" stays in
    base_dir unless overridden.
    """
    if not enabled:
        return base_dir
    cat = classify(filename)
    if overrides and overrides.get(cat):
        p = overrides[cat]
        return p if os.path.isabs(p) else os.path.join(base_dir, p)
    sub = CATEGORY_FOLDER.get(cat, "")
    return os.path.join(base_dir, sub) if sub else base_dir
