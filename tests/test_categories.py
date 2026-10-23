"""Tests for file-type categorization and IDM-style folder routing."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm.categories import classify, category_subdir, categorized_dir


def test_classify():
    assert classify("movie.mp4") == "Video"
    assert classify("song.MP3") == "Audio"
    assert classify("report.pdf") == "Documents"
    assert classify("bundle.zip") == "Archives"
    assert classify("setup.exe") == "Programs"
    assert classify("mystery.xyz") == "Other"
    assert classify("") == "Other"


def test_category_subdir_idm_names():
    assert category_subdir("a.mp4") == "Video"
    assert category_subdir("a.mp3") == "Music"       # Audio -> Music
    assert category_subdir("a.pdf") == "Documents"
    assert category_subdir("a.zip") == "Compressed"  # Archives -> Compressed
    assert category_subdir("a.exe") == "Programs"
    assert category_subdir("a.xyz") == ""            # Other -> base dir


def test_categorized_dir():
    base = os.path.join("C:", os.sep, "dl")
    assert categorized_dir(base, "a.mp4") == os.path.join(base, "Video")
    assert categorized_dir(base, "a.mp3") == os.path.join(base, "Music")
    assert categorized_dir(base, "a.zip") == os.path.join(base, "Compressed")
    assert categorized_dir(base, "a.xyz") == base         # uncategorized stays
    assert categorized_dir(base, "a.mp4", enabled=False) == base  # disabled


def test_categorized_dir_overrides():
    base = os.path.join("C:", os.sep, "dl")
    abs_movies = os.path.join("D:", os.sep, "Movies")
    overrides = {"Video": abs_movies, "Audio": "tunes"}   # keyed by category name
    # Absolute override used as-is.
    assert categorized_dir(base, "a.mp4", overrides=overrides) == abs_movies
    # Relative override joined to base.
    assert categorized_dir(base, "a.mp3", overrides=overrides) == os.path.join(base, "tunes")
    # Category with no override falls back to the default subfolder.
    assert categorized_dir(base, "a.zip", overrides=overrides) == os.path.join(base, "Compressed")


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("all category tests passed")
