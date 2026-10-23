"""Tests for the 'when all downloads finish' action (no real program/shutdown)."""
import os
import sys
import tempfile

_TMP = tempfile.mkdtemp()
os.environ["HOME"] = _TMP
os.environ["USERPROFILE"] = _TMP

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm.manager import DownloadManager, ST_COMPLETED, ST_PAUSED, ST_DOWNLOADING


def _mgr_recording():
    m = DownloadManager()
    calls = []
    m._run_completion_action = lambda action, cmd: calls.append((action, cmd))
    return m, calls


def test_fires_once_when_all_done():
    m, calls = _mgr_recording()
    m.set_on_complete("run", "echo hi")
    it = m.add("http://127.0.0.1:1/x", start=False)
    it.status = ST_COMPLETED
    m._maybe_on_complete()
    assert calls == [("run", "echo hi")]
    m._maybe_on_complete()                 # must not fire twice
    assert len(calls) == 1


def test_none_action_never_fires():
    m, calls = _mgr_recording()
    m.set_on_complete("none")
    it = m.add("http://127.0.0.1:1/x", start=False)
    it.status = ST_COMPLETED
    m._maybe_on_complete()
    assert calls == []


def test_not_done_while_active():
    m, calls = _mgr_recording()
    m.set_on_complete("shutdown")
    a = m.add("http://127.0.0.1:1/a", start=False)
    b = m.add("http://127.0.0.1:1/b", start=False)
    a.status = ST_COMPLETED
    b.status = ST_DOWNLOADING            # still working
    m._maybe_on_complete()
    assert calls == []
    assert m._queue_all_done() is False


def test_requires_a_completion():
    m, calls = _mgr_recording()
    m.set_on_complete("sleep")
    it = m.add("http://127.0.0.1:1/x", start=False)
    it.status = ST_PAUSED                # nothing completed
    assert m._queue_all_done() is False
    m._maybe_on_complete()
    assert calls == []


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("all on-complete tests passed")
