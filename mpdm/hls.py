"""Minimal HLS (.m3u8) parsing and segment collection.

HLS describes a stream as a playlist of short media segments. A *master*
playlist lists quality variants; each variant points to a *media* playlist that
lists the actual segment URIs (often .ts or fragmented .mp4), plus an optional
init segment (#EXT-X-MAP) and encryption info (#EXT-X-KEY).

This parser is intentionally compact and covers the common cases. It is not a
full HLS implementation — notably, full AES-128 segment decryption and
SAMPLE-AES are left as clearly marked extension points.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional
from urllib.parse import urljoin

import requests

from .config import Settings
from .http_client import build_session


@dataclass
class Variant:
    uri: str
    bandwidth: int = 0
    resolution: str = ""
    codecs: str = ""


@dataclass
class MediaPlaylist:
    segments: list[str] = field(default_factory=list)
    init_segment: Optional[str] = None
    key_uri: Optional[str] = None
    key_method: str = "NONE"
    target_duration: float = 0.0
    is_endlist: bool = False


def _fetch_text(url: str, settings: Settings, session: requests.Session) -> str:
    r = session.get(url, timeout=(settings.connect_timeout, settings.read_timeout),
                    allow_redirects=True, verify=settings.verify_tls)
    r.raise_for_status()
    return r.text


def _parse_attributes(line: str) -> dict:
    """Parse comma-separated KEY=VALUE pairs, respecting quoted values."""
    attrs, i, n = {}, 0, len(line)
    while i < n:
        eq = line.find("=", i)
        if eq < 0:
            break
        key = line[i:eq].strip()
        j = eq + 1
        if j < n and line[j] == '"':
            end = line.find('"', j + 1)
            val = line[j + 1:end]
            i = end + 2
        else:
            comma = line.find(",", j)
            if comma < 0:
                comma = n
            val = line[j:comma].strip()
            i = comma + 1
        attrs[key.upper()] = val
    return attrs


def is_master(text: str) -> bool:
    return "#EXT-X-STREAM-INF" in text


def parse_master(text: str, base_url: str) -> list[Variant]:
    variants: list[Variant] = []
    lines = [ln.strip() for ln in text.splitlines()]
    for idx, ln in enumerate(lines):
        if ln.startswith("#EXT-X-STREAM-INF"):
            attrs = _parse_attributes(ln.split(":", 1)[1])
            # The URI is the next non-comment line.
            uri = ""
            for nxt in lines[idx + 1:]:
                if nxt and not nxt.startswith("#"):
                    uri = nxt
                    break
            variants.append(Variant(
                uri=urljoin(base_url, uri),
                bandwidth=int(attrs.get("BANDWIDTH", "0") or 0),
                resolution=attrs.get("RESOLUTION", ""),
                codecs=attrs.get("CODECS", ""),
            ))
    variants.sort(key=lambda v: v.bandwidth, reverse=True)
    return variants


def parse_media(text: str, base_url: str) -> MediaPlaylist:
    pl = MediaPlaylist()
    for ln in text.splitlines():
        ln = ln.strip()
        if not ln:
            continue
        if ln.startswith("#EXT-X-TARGETDURATION"):
            try:
                pl.target_duration = float(ln.split(":", 1)[1])
            except (ValueError, IndexError):
                pass
        elif ln.startswith("#EXT-X-MAP"):
            attrs = _parse_attributes(ln.split(":", 1)[1])
            if "URI" in attrs:
                pl.init_segment = urljoin(base_url, attrs["URI"])
        elif ln.startswith("#EXT-X-KEY"):
            attrs = _parse_attributes(ln.split(":", 1)[1])
            pl.key_method = attrs.get("METHOD", "NONE")
            if "URI" in attrs:
                pl.key_uri = urljoin(base_url, attrs["URI"])
        elif ln.startswith("#EXT-X-ENDLIST"):
            pl.is_endlist = True
        elif not ln.startswith("#"):
            pl.segments.append(urljoin(base_url, ln))
    return pl


def resolve_media_playlist(url: str, settings: Settings,
                           session: Optional[requests.Session] = None,
                           prefer: str = "best") -> tuple[MediaPlaylist, str]:
    """Fetch `url`; if it's a master playlist, pick a variant and fetch that.

    `prefer` may be "best", "worst", or a resolution substring like "720".
    Returns (MediaPlaylist, resolved_media_url).
    """
    sess = session or build_session(settings)
    text = _fetch_text(url, settings, sess)
    if is_master(text):
        variants = parse_master(text, url)
        if not variants:
            raise ValueError("master playlist had no variants")
        chosen = variants[0]
        if prefer == "worst":
            chosen = variants[-1]
        elif prefer not in ("best", ""):
            for v in variants:
                if prefer in v.resolution:
                    chosen = v
                    break
        media_text = _fetch_text(chosen.uri, settings, sess)
        return parse_media(media_text, chosen.uri), chosen.uri
    return parse_media(text, url), url
