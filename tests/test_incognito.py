"""Tests for incognito downloads (no history record + hidden folder)."""
import json
import os
import sys
import tempfile

_TMP = tempfile.mkdtemp()
os.environ["HOME"] = _TMP
os.environ["USERPROFILE"] = _TMP

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm import storage
from mpdm.config import INCOGNITO_DIRNAME
from mpdm.manager import DownloadManager


def test_incognito_item_not_persisted():
    m = DownloadManager()
    normal = m.add("http://127.0.0.1:1/normal.zip", start=False)
    secret = m.add("http://127.0.0.1:1/secret.zip", start=False, incognito=True)
    assert secret.incognito is True
    m.save()

    data = json.loads(m._store.read_text(encoding="utf-8"))
    urls = [d["url"] for d in data["items"]]
    assert "http://127.0.0.1:1/normal.zip" in urls      # normal is kept
    assert "http://127.0.0.1:1/secret.zip" not in urls  # incognito is omitted

    # A fresh manager (restart) must not see the incognito download at all.
    m2 = DownloadManager()
    assert any(i.url == normal.url for i in m2.items.values())
    assert not any(i.url == secret.url for i in m2.items.values())


def test_ensure_hidden_dir_creates_folder():
    base = tempfile.mkdtemp()
    hidden = storage.ensure_hidden_dir(os.path.join(base, INCOGNITO_DIRNAME))
    assert os.path.isdir(hidden)
    assert os.path.basename(hidden) == "InvisibleFile"


def _make_partial(d, name="secret.zip"):
    dest = os.path.join(d, name)
    open(dest, "wb").write(b"partial data")
    open(storage.sidecar_path(dest), "w").write("{}")   # .mpdm sidecar
    return dest


def test_remove_incognito_deletes_partial_and_sidecar():
    m = DownloadManager()
    d = tempfile.mkdtemp()
    it = m.add("http://127.0.0.1:1/secret.zip", start=False, incognito=True)
    it.dest_path = _make_partial(d)             # simulate an interrupted partial
    assert os.path.exists(it.dest_path)
    m.remove(it.id)                             # no delete_file flag — incognito still purges
    assert not os.path.exists(it.dest_path)
    assert not os.path.exists(storage.sidecar_path(it.dest_path))


def test_remove_normal_keeps_file_without_flag():
    m = DownloadManager()
    d = tempfile.mkdtemp()
    it = m.add("http://127.0.0.1:1/keep.zip", start=False)   # not incognito
    it.dest_path = _make_partial(d, "keep.zip")
    m.remove(it.id)                             # default: keep the file
    assert os.path.exists(it.dest_path)


def test_purge_incognito_incomplete_on_close():
    m = DownloadManager()
    d = tempfile.mkdtemp()
    it = m.add("http://127.0.0.1:1/x.zip", start=False, incognito=True)
    it.dest_path = _make_partial(d, "x.zip")
    m.purge_incognito_incomplete()
    assert not os.path.exists(it.dest_path)
    assert it.id not in m.items


if __name__ == "__main__":
    # Definition order (globals() preserves it); the persist test runs first.
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("all incognito tests passed")
