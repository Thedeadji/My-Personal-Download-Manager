"""Tests for Google Drive URL resolution (no network — fake session)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm import gdrive
from mpdm.config import Settings


def test_is_drive_url():
    assert gdrive.is_drive_url("https://drive.google.com/file/d/ABC/view")
    assert gdrive.is_drive_url("https://drive.usercontent.google.com/download?id=ABC")
    assert not gdrive.is_drive_url("https://example.com/file.rar")


def test_file_id_forms():
    assert gdrive.file_id("https://drive.google.com/file/d/1A2B3C4D5E6F/view?usp=sharing") == "1A2B3C4D5E6F"
    assert gdrive.file_id("https://drive.google.com/open?id=1A2B3C4D5E6F") == "1A2B3C4D5E6F"
    assert gdrive.file_id("https://drive.google.com/uc?export=download&id=1A2B3C4D5E6F") == "1A2B3C4D5E6F"
    assert gdrive.file_id("https://example.com/no-id") is None


class _Resp:
    def __init__(self, url, content_type, text="", disposition=None):
        self.url = url
        self.headers = {"Content-Type": content_type}
        if disposition:
            self.headers["Content-Disposition"] = disposition
        self.text = text

    def close(self):
        pass


class _Session:
    def __init__(self, resp):
        self._resp = resp
        self.calls = []

    def get(self, url, params=None, **kw):
        self.calls.append((url, params))
        # Reflect the id/params into the response url like Drive would.
        return self._resp


def test_resolve_direct_file():
    # confirm=t endpoint returns the file directly (non-HTML) -> url + name from CD.
    resp = _Resp("https://drive.usercontent.google.com/download?id=ID&export=download&confirm=t",
                 "application/x-rar-compressed",
                 disposition='attachment; filename="Mabar First.zip"')
    out, name = gdrive.resolve("https://drive.google.com/file/d/ID1234567890/view",
                               Settings(), _Session(resp))
    assert out == resp.url
    assert name == "Mabar First.zip"


INTERSTITIAL = """<!doctype html><html><body>
<form id="download-form" action="https://drive.usercontent.google.com/download" method="get">
  <input type="hidden" name="id" value="ID1234567890">
  <input type="hidden" name="export" value="download">
  <input type="hidden" name="confirm" value="abcTOKEN">
  <input type="hidden" name="uuid" value="xyz-uuid-1">
</form></body></html>"""


def test_resolve_interstitial_form():
    resp = _Resp("https://drive.usercontent.google.com/download?...", "text/html; charset=utf-8",
                 text=INTERSTITIAL)
    out, name = gdrive.resolve("https://drive.google.com/file/d/ID1234567890/view",
                               Settings(), _Session(resp))
    assert out.startswith("https://drive.usercontent.google.com/download?")
    assert "id=ID1234567890" in out
    assert "confirm=abcTOKEN" in out
    assert "uuid=xyz-uuid-1" in out


def test_resolve_tokenized_reads_name():
    # A tokenized URL downloads directly; we still fetch headers to get its name.
    resp = _Resp("https://drive.usercontent.google.com/download?id=ID&confirm=t&at=T",
                 "application/zip", disposition="attachment; filename=\"Mabar First.zip\"")
    url = ("https://drive.usercontent.google.com/download?id=ID1234567890"
           "&export=download&confirm=t&uuid=abc&at=TOKEN")
    out, name = gdrive.resolve(url, Settings(), _Session(resp))
    assert out == resp.url
    assert name == "Mabar First.zip"


def test_resolve_non_drive_passthrough():
    # Not a Drive file id -> return input unchanged (no request made).
    sess = _Session(_Resp("x", "text/html"))
    url = "https://drive.google.com/drive/folders/XYZ"
    assert gdrive.resolve(url, Settings(), sess) == (url, None)
    assert sess.calls == []


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("all gdrive tests passed")
