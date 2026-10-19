"""Minimal MPEG-DASH (.mpd) parsing for the common SegmentTemplate case.

DASH manifests are XML. The widely used layout is a SegmentTemplate with
$Number$ (or $Time$) placeholders and a count derived from the
SegmentTimeline. This parser handles SegmentTemplate with SegmentTimeline and
with a start/duration count; SegmentList and SegmentBase are marked as
extension points.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Optional
from urllib.parse import urljoin
from xml.etree import ElementTree as ET

import requests

from .config import Settings
from .http_client import build_session

_NS = re.compile(r"\{[^}]+\}")


def _tag(el) -> str:
    return _NS.sub("", el.tag)


def _find_all(root, name):
    return [e for e in root.iter() if _tag(e) == name]


@dataclass
class DashStream:
    init_url: Optional[str]
    segment_urls: list[str] = field(default_factory=list)
    mime: str = ""
    bandwidth: int = 0


def _expand(template: str, rep_id: str, number: int = None, time: int = None, bandwidth: int = None) -> str:
    out = template.replace("$RepresentationID$", rep_id)
    if bandwidth is not None:
        out = out.replace("$Bandwidth$", str(bandwidth))
    if number is not None:
        def repl(m):
            fmt = m.group(1)
            return format(number, fmt.lstrip("%0").rstrip("d").join(("0", "d")) if fmt else "d") if fmt else str(number)
        out = re.sub(r"\$Number(%0\d+d)?\$", lambda m: _fmt_num(m.group(1), number), out)
    if time is not None:
        out = out.replace("$Time$", str(time))
    return out


def _fmt_num(fmt: Optional[str], number: int) -> str:
    if not fmt:
        return str(number)
    m = re.match(r"%0(\d+)d", fmt)
    if m:
        return str(number).zfill(int(m.group(1)))
    return str(number)


def parse_mpd(url: str, settings: Settings, session: Optional[requests.Session] = None,
              prefer: str = "best") -> DashStream:
    sess = session or build_session(settings)
    r = sess.get(url, timeout=(settings.connect_timeout, settings.read_timeout),
                 allow_redirects=True, verify=settings.verify_tls)
    r.raise_for_status()
    root = ET.fromstring(r.text)
    base_url = url

    # Honour a top-level <BaseURL> if present.
    for b in _find_all(root, "BaseURL"):
        base_url = urljoin(url, b.text.strip())
        break

    # Collect video representations with a SegmentTemplate.
    reps = []
    for rep in _find_all(root, "Representation"):
        bw = int(rep.get("bandwidth", "0") or 0)
        rep_id = rep.get("id", "")
        # SegmentTemplate may live on the Representation or its AdaptationSet parent.
        tmpl = None
        for cand in list(rep) + []:
            if _tag(cand) == "SegmentTemplate":
                tmpl = cand
        reps.append((bw, rep_id, rep, tmpl))

    if not reps:
        raise ValueError("no Representations found in MPD")

    reps.sort(key=lambda t: t[0], reverse=True)
    chosen = reps[0] if prefer != "worst" else reps[-1]
    bw, rep_id, rep, tmpl = chosen

    # Search AdaptationSet ancestor for a SegmentTemplate if needed.
    if tmpl is None:
        for ad in _find_all(root, "AdaptationSet"):
            if rep in list(ad.iter()):
                for cand in ad:
                    if _tag(cand) == "SegmentTemplate":
                        tmpl = cand
                break

    if tmpl is None:
        raise NotImplementedError(
            "This MPD doesn't use SegmentTemplate (SegmentList/SegmentBase). "
            "Extension point: add SegmentList handling here."
        )

    media = tmpl.get("media", "")
    init = tmpl.get("initialization", "")
    start_number = int(tmpl.get("startNumber", "1") or 1)

    init_url = urljoin(base_url, _expand(init, rep_id, bandwidth=bw)) if init else None

    seg_urls: list[str] = []
    timeline = None
    for c in tmpl:
        if _tag(c) == "SegmentTimeline":
            timeline = c
            break

    if timeline is not None:
        number = start_number
        for s in timeline:
            if _tag(s) != "S":
                continue
            repeat = int(s.get("r", "0") or 0)
            for _ in range(repeat + 1):
                seg_urls.append(urljoin(base_url, _fmt_media(media, rep_id, number, bw)))
                number += 1
    else:
        # Count derived from duration/timescale vs total duration (best effort).
        duration = int(tmpl.get("duration", "0") or 0)
        timescale = int(tmpl.get("timescale", "1") or 1)
        total = _mpd_duration_seconds(root)
        if duration and total:
            count = int((total * timescale) // duration) + 1
            for i in range(count):
                seg_urls.append(urljoin(base_url, _fmt_media(media, rep_id, start_number + i, bw)))
        else:
            raise NotImplementedError("could not determine segment count for this MPD")

    return DashStream(init_url=init_url, segment_urls=seg_urls,
                      mime=rep.get("mimeType", ""), bandwidth=bw)


def _fmt_media(media: str, rep_id: str, number: int, bw: int) -> str:
    out = media.replace("$RepresentationID$", rep_id).replace("$Bandwidth$", str(bw))
    out = re.sub(r"\$Number(%0\d+d)?\$", lambda m: _fmt_num(m.group(1), number), out)
    return out


def _mpd_duration_seconds(root) -> float:
    dur = root.get("mediaPresentationDuration") or ""
    m = re.match(r"PT(?:(\d+)H)?(?:(\d+)M)?(?:([\d.]+)S)?", dur)
    if not m:
        return 0.0
    h, mi, s = m.groups()
    return (int(h or 0) * 3600) + (int(mi or 0) * 60) + float(s or 0)
