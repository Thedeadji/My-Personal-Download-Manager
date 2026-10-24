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


if __name__ == "__main__":
    test_incognito_item_not_persisted(); print("ok test_incognito_item_not_persisted")
    test_ensure_hidden_dir_creates_folder(); print("ok test_ensure_hidden_dir_creates_folder")
    print("all incognito tests passed")
