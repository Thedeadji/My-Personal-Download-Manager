"""Minimal HLS (.m3u8) parsing and segment collection.

HLS describes a stream as a playlist of short media segments. A *master*
playlist lists quality variants; each variant points to a *media* playlist that
lists the actual segment URIs (often .ts or fragmented .mp4), plus an optional
init segment (#EXT-X-MAP) and encryption info (#EXT-X-KEY).

This parser is intentionally compact and covers the common cases, including
AES-128 full-segment encryption (keys and IVs are tracked per segment, since a
playlist may rotate keys via multiple #EXT-X-KEY tags). SAMPLE-AES — which
encrypts only parts of each frame and needs container-aware handling — is left
as a clearly marked extension point.
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
class SegmentKey:
    """Encryption parameters that apply to one or more media segments."""
    method: str = "NONE"               # "NONE", "AES-128", "SAMPLE-AES", ...
    uri: Optional[str] = None          # absolute URI of the key bytes
    iv: Optional[bytes] = None         # explicit 16-byte IV, or None (use seq)


@dataclass
class MediaSegment:
    uri: str
    seq: int = 0                       # media sequence number (for default IV)
    key: Optional[SegmentKey] = None   # None / method "NONE" => unencrypted


@dataclass
class MediaPlaylist:
    segments: list[str] = field(default_factory=list)       # URIs (back-compat)
    seg_infos: list[MediaSegment] = field(default_factory=list)
    init_segment: Optional[str] = None
    key_uri: Optional[str] = None      # first key seen (back-compat)
    key_method: str = "NONE"           # "NONE" if no segment is encrypted
    media_sequence: int = 0
    target_duration: float = 0.0
    is_endlist: bool = False

    @property
    def is_encrypted(self) -> bool:
        return any(s.key and s.key.method not in ("NONE", "") for s in self.seg_infos)


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


def _parse_iv(raw: str) -> Optional[bytes]:
    """Parse an #EXT-X-KEY IV attribute (a 0x-prefixed 128-bit hex value)."""
    raw = raw.strip()
    if raw[:2].lower() == "0x":
        raw = raw[2:]
    try:
        iv = bytes.fromhex(raw)
    except ValueError:
        return None
    return iv.rjust(16, b"\x00")[-16:] if iv else None


def parse_media(text: str, base_url: str) -> MediaPlaylist:
    pl = MediaPlaylist()
    current_key: Optional[SegmentKey] = None   # applies to following segments
    seq = 0
    for ln in text.splitlines():
        ln = ln.strip()
        if not ln:
            continue
        if ln.startswith("#EXT-X-TARGETDURATION"):
            try:
                pl.target_duration = float(ln.split(":", 1)[1])
            except (ValueError, IndexError):
                pass
        elif ln.startswith("#EXT-X-MEDIA-SEQUENCE"):
            try:
                pl.media_sequence = int(ln.split(":", 1)[1])
                seq = pl.media_sequence
            except (ValueError, IndexError):
                pass
        elif ln.startswith("#EXT-X-MAP"):
            attrs = _parse_attributes(ln.split(":", 1)[1])
            if "URI" in attrs:
                pl.init_segment = urljoin(base_url, attrs["URI"])
        elif ln.startswith("#EXT-X-KEY"):
            attrs = _parse_attributes(ln.split(":", 1)[1])
            method = attrs.get("METHOD", "NONE")
            if method == "NONE":
                current_key = None
            else:
                current_key = SegmentKey(
                    method=method,
                    uri=urljoin(base_url, attrs["URI"]) if "URI" in attrs else None,
                    iv=_parse_iv(attrs["IV"]) if "IV" in attrs else None,
                )
                # Back-compat scalar fields: remember the first encrypting key.
                if pl.key_method == "NONE":
                    pl.key_method = method
                    pl.key_uri = current_key.uri
        elif ln.startswith("#EXT-X-ENDLIST"):
            pl.is_endlist = True
        elif not ln.startswith("#"):
            seg_url = urljoin(base_url, ln)
            pl.segments.append(seg_url)
            pl.seg_infos.append(MediaSegment(uri=seg_url, seq=seq, key=current_key))
            seq += 1
    return pl


def fetch_key(uri: str, settings: Settings, session: requests.Session) -> bytes:
    """Fetch a 16-byte AES-128 key from a key URI."""
    r = session.get(uri, timeout=(settings.connect_timeout, settings.read_timeout),
                    allow_redirects=True, verify=settings.verify_tls)
    r.raise_for_status()
    key = r.content
    if len(key) != 16:
        raise ValueError(f"AES-128 key at {uri} was {len(key)} bytes, expected 16")
    return key


def segment_iv(seg: MediaSegment) -> bytes:
    """IV for a segment: the explicit key IV, or the media sequence number."""
    if seg.key and seg.key.iv is not None:
        return seg.key.iv
    return seg.seq.to_bytes(16, "big")


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
