"""Tests for settings persistence (settings.json) and per-category folders."""
import os
import sys
import tempfile

_TMP = tempfile.mkdtemp()
os.environ["HOME"] = _TMP
os.environ["USERPROFILE"] = _TMP

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm import config
from mpdm.manager import DownloadManager


def test_prefs_roundtrip():
    config.save_prefs({
        "settings": {"proxy": "http://p:8080", "categorize": False,
                     "category_dirs": {"Video": "D:/Movies"}},
        "max_concurrent": 5,
        "global_rate_bps": 1024,
        "on_done_action": "sleep",
    })
    s = config.settings_from_prefs(config.load_prefs())
    assert s.proxy == "http://p:8080"
    assert s.categorize is False
    assert s.category_dirs == {"Video": "D:/Movies"}


def test_manager_loads_and_saves_prefs():
    # A fresh manager should pick up what the previous test persisted.
    m = DownloadManager()
    assert m.settings.proxy == "http://p:8080"
    assert m.max_concurrent == 5
    assert m.global_rate_bps == 1024
    assert m.on_done_action == "sleep"

    # Changing + saving should persist for the next manager.
    m.settings.proxy = "http://q:9090"
    m.settings.category_dirs = {"Music": "E:/Audio"}
    m.set_on_complete("run", "echo done")
    m.save_settings()

    m2 = DownloadManager()
    assert m2.settings.proxy == "http://q:9090"
    assert m2.settings.category_dirs == {"Music": "E:/Audio"}
    assert m2.on_done_action == "run" and m2.on_done_command == "echo done"


def test_env_overrides_file():
    config.save_prefs({"settings": {"proxy": "http://file:1"}})
    os.environ["MDM_PROXY"] = "http://env:2"
    try:
        s = config.settings_from_prefs(config.load_prefs())
        assert s.proxy == "http://env:2"      # env wins over the file
    finally:
        del os.environ["MDM_PROXY"]


if __name__ == "__main__":
    # Order matters (roundtrip persists what the manager test reads).
    test_prefs_roundtrip(); print("ok test_prefs_roundtrip")
    test_manager_loads_and_saves_prefs(); print("ok test_manager_loads_and_saves_prefs")
    test_env_overrides_file(); print("ok test_env_overrides_file")
    print("all prefs tests passed")
