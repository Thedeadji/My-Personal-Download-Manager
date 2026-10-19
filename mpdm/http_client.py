"""HTTP helpers: probing a URL and opening ranged byte streams.

Uses `requests`, which every reader can follow. The key capability a download
manager needs from HTTP is the byte-range request (RFC 7233): asking the server
for `Range: bytes=START-END` so the file can be fetched in parallel pieces and
resumed after interruption.
"""
from __future__ import annotations

import re
import os
from email.utils import unquote
from typing import Optional
from urllib.parse import urlsplit, unquote as url_unquote

import requests

from .config import Settings
from .models import RemoteInfo


_FILENAME_STAR = re.compile(r"filename\*\s*=\s*([^']*)'[^']*'([^;]+)", re.I)
_FILENAME = re.compile(r'filename\s*=\s*"?([^";]+)"?', re.I)


def _filename_from_disposition(cd: str) -> Optional[str]:
    if not cd:
        return None
    m = _FILENAME_STAR.search(cd)
    if m:
        return url_unquote(m.group(2)).strip()
    m = _FILENAME.search(cd)
    if m:
        return m.group(1).strip()
    return None


def _filename_from_url(url: str) -> str:
    path = urlsplit(url).path
    name = url_unquote(os.path.basename(path)) or "download"
    return name


def sanitize_filename(name: str) -> str:
    """Strip path separators and characters illegal on common filesystems."""
    name = name.replace("\\", "_").replace("/", "_")
    name = re.sub(r'[<>:"|?*\x00-\x1f]', "_", name).strip().strip(".")
    return name or "download"


def build_session(settings: Settings, extra_headers: Optional[dict] = None) -> requests.Session:
    s = requests.Session()
    s.headers.update({"User-Agent": settings.user_agent, "Accept": "*/*"})
    if extra_headers:
        # Caller-supplied headers (Cookie, Referer, custom User-Agent from the
        # browser) override our defaults so authenticated downloads replay.
        s.headers.update({k: v for k, v in extra_headers.items() if v})
    return s


def probe(url: str, settings: Settings, session: Optional[requests.Session] = None) -> RemoteInfo:
    """Discover size, range support and filename without downloading the body.

    Strategy: a ranged GET of the first byte. This is more reliable than HEAD
    (many servers lie about or disallow HEAD) and directly proves whether the
    server honours Range by returning HTTP 206 + Content-Range.
    """
    sess = session or build_session(settings)
    headers = {"Range": "bytes=0-0"}
    r = sess.get(
        url,
        headers=headers,
        stream=True,
        allow_redirects=True,
        timeout=(settings.connect_timeout, settings.read_timeout),
        verify=settings.verify_tls,
    )
    try:
        final_url = r.url
        accept_ranges = False
        size: Optional[int] = None

        if r.status_code == 206:
            accept_ranges = True
            # Content-Range: bytes 0-0/12345  -> total is after the slash
            cr = r.headers.get("Content-Range", "")
            m = re.search(r"/(\d+)$", cr)
            if m:
                size = int(m.group(1))
        elif r.status_code == 200:
            # Server ignored Range -> no partial support.
            accept_ranges = r.headers.get("Accept-Ranges", "").lower() == "bytes"
            cl = r.headers.get("Content-Length")
            if cl is not None:
                size = int(cl)
        else:
            r.raise_for_status()

        # If size still unknown but server advertised it via Accept-Ranges HEAD-style
        if size is None and r.headers.get("Content-Length") and r.status_code == 200:
            size = int(r.headers["Content-Length"])

        fname = (
            _filename_from_disposition(r.headers.get("Content-Disposition", ""))
            or _filename_from_url(final_url)
        )
        return RemoteInfo(
            url=url,
            final_url=final_url,
            size=size,
            accept_ranges=accept_ranges,
            filename=sanitize_filename(fname),
            etag=r.headers.get("ETag"),
            last_modified=r.headers.get("Last-Modified"),
            content_type=r.headers.get("Content-Type"),
        )
    finally:
        r.close()


def open_range(
    url: str,
    start: int,
    end: Optional[int],
    settings: Settings,
    session: Optional[requests.Session] = None,
) -> requests.Response:
    """Open a streaming response for bytes [start, end] (end inclusive).

    If `end` is None, requests the open-ended range start-. Caller must close.
    """
    sess = session or build_session(settings)
    rng = f"bytes={start}-" if end is None else f"bytes={start}-{end}"
    r = sess.get(
        url,
        headers={"Range": rng},
        stream=True,
        allow_redirects=True,
        timeout=(settings.connect_timeout, settings.read_timeout),
        verify=settings.verify_tls,
    )
    r.raise_for_status()
    return r
