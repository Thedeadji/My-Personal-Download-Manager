"""Tests for the native host's download-popup launch (no window is opened)."""
import json
import os
import sys
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm import native_host as nh


def test_popup_command_shape():
    cmd = nh._popup_command("/usr/bin/python3", "/tmp/job.json")
    assert cmd[-3:] == ["-m", "mpdm.ui.progress", "/tmp/job.json"]


def test_is_stream():
    assert nh._is_stream({"stream": True, "url": "https://h/x"}) is True
    assert nh._is_stream({"url": "https://h/playlist.m3u8"}) is True
    assert nh._is_stream({"url": "https://h/manifest.mpd?x=1"}) is True
    assert nh._is_stream({"url": "https://h/movie.mp4"}) is False
    assert nh._is_stream({"url": "https://h/file.zip"}) is False


def test_launch_disabled_by_env():
    old = os.environ.get("MDM_NO_POPUP")
    os.environ["MDM_NO_POPUP"] = "1"
    try:
        assert nh._launch_popup({"url": "https://h/f"}) is None
    finally:
        if old is None:
            os.environ.pop("MDM_NO_POPUP", None)
        else:
            os.environ["MDM_NO_POPUP"] = old


def test_launch_writes_job_and_spawns():
    os.environ.pop("MDM_NO_POPUP", None)
    captured = {}

    def fake_popen(cmd, **kw):
        captured["cmd"] = cmd
        return mock.Mock()

    with mock.patch("subprocess.Popen", fake_popen):
        job = {"url": "https://h/f", "headers": {"Cookie": "x"}, "connections": 4}
        proc = nh._launch_popup(job)
    assert proc is not None
    cmd = captured["cmd"]
    assert cmd[-3:-1] == ["-m", "mpdm.ui.progress"]
    job_file = cmd[-1]
    assert os.path.exists(job_file)
    data = json.load(open(job_file, encoding="utf-8"))
    assert data["url"] == "https://h/f" and data["connections"] == 4
    os.remove(job_file)


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("all native-popup tests passed")
