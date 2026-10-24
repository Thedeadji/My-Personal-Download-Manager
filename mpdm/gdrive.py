"""Google Drive download resolution.

Drive doesn't serve large files directly: the normal download URL returns an HTML
"Google Drive can't scan this file for viruses — download anyway?" interstitial
instead of the bytes, so a plain download manager saves that tiny HTML page. This
module rewrites a Drive share/download URL to the real file URL, following the
confirm-token flow (and carrying cookies in the session), so the engine can
download — and correctly name — the file.

Handled URL forms (public / link-shared files):
    https://drive.google.com/file/d/<ID>/view?usp=sharing
    https://drive.google.com/open?id=<ID>
    https://drive.google.com/uc?export=download&id=<ID>
    https://drive.usercontent.google.com/download?id=<ID>&export=download
"""
from __future__ import annotations

import re
from typing import Optional
from urllib.parse import urlparse, parse_qs, urlencode

import requests

from .config import Settings
from .http_client import build_session, _filename_from_disposition

_HOSTS = ("drive.google.com", "drive.usercontent.google.com")
_DOWNLOAD = "https://drive.usercontent.google.com/download"


def is_drive_url(url: str) -> bool:
    try:
        host = urlparse(url).netloc.lower()
    except Exception:
        return False
    return any(host == h or host.endswith("." + h) for h in _HOSTS)


def file_id(url: str) -> Optional[str]:
    """Extract the Drive file id from the common URL shapes."""
    m = re.search(r"/file/d/([A-Za-z0-9_-]{10,})", url)
    if m:
        return m.group(1)
    q = parse_qs(urlparse(url).query)
    if q.get("id"):
        return q["id"][0]
    m = re.search(r"/d/([A-Za-z0-9_-]{10,})", url)
    if m:
        return m.group(1)
    return None


def _is_html(resp: requests.Response) -> bool:
    return "text/html" in (resp.headers.get("Content-Type", "") or "").lower()


def _name_from_html(html: str) -> Optional[str]:
    """Best-effort filename from a Drive interstitial page."""
    m = re.search(r'<span[^>]*class="uc-name-size"[^>]*>.*?>([^<]+)</a>', html, re.S | re.I)
    if m:
        return m.group(1).strip()
    m = re.search(r"<title>([^<]+)</title>", html, re.I)
    if m:
        t = m.group(1).strip()
        for suffix in (" - Google Drive", " – Google Drive"):
            if t.endswith(suffix):
                return t[: -len(suffix)].strip()
    return None


def _parse_download_form(html: str) -> Optional[str]:
    """Build the real download URL from the interstitial's <form>."""
    m = re.search(r'id="download-form"[^>]*action="([^"]+)"', html, re.I) \
        or re.search(r'<form[^>]+action="([^"]*(?:download|uc)[^"]*)"', html, re.I)
    if not m:
        return None
    action = m.group(1).replace("&amp;", "&")
    inputs = dict(re.findall(r'<input[^>]+name="([^"]+)"[^>]+value="([^"]*)"', html, re.I))
    if inputs:
        sep = "&" if "?" in action else "?"
        return action + sep + urlencode(inputs)
    return action


def resolve(url: str, settings: Optional[Settings] = None,
            session: Optional[requests.Session] = None):
    """Resolve a Drive link to (download_url, filename_or_None).

    Makes one header request so it can both confirm the real download URL and read
    the file's name from Content-Disposition (the Drive download page omits it from
    the URL, so a plain save becomes "download"). Uses `session` so Drive cookies
    persist for the engine's later probe/range requests. Never raises on a miss.
    """
    settings = settings or Settings()
    sess = session or build_session(settings)
    fid = file_id(url)
    if not fid:
        return url, None

    tokenized = bool(parse_qs(urlparse(url).query).get("confirm"))
    target = url if tokenized else _DOWNLOAD
    params = None if tokenized else {"id": fid, "export": "download", "confirm": "t"}
    try:
        r = sess.get(target, params=params, stream=True,
                     timeout=(settings.connect_timeout, settings.read_timeout),
                     allow_redirects=True, verify=settings.verify_tls)
    except requests.RequestException:
        return url, None
    try:
        if not _is_html(r):
            name = _filename_from_disposition(r.headers.get("Content-Disposition", ""))
            return r.url, name           # the file itself; name from its headers
        html = r.text                    # small interstitial page
        final_url = r.url
    finally:
        r.close()

    name = _name_from_html(html)
    parsed = _parse_download_form(html)
    if parsed:
        return parsed, name
    m = re.search(r"confirm=([0-9A-Za-z_\-]+)", html)
    if m:
        return _DOWNLOAD + "?" + urlencode({"id": fid, "export": "download",
                                            "confirm": m.group(1)}), name
    return final_url, name
