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


def _child(el, name):
    """First direct child of `el` with the given (namespace-stripped) tag."""
    for c in el:
        if _tag(c) == name:
            return c
    return None


def _adaptation_of(root, rep):
    """The AdaptationSet element that contains `rep`, if any."""
    for ad in _find_all(root, "AdaptationSet"):
        if rep in list(ad.iter()):
            return ad
    return None


def _seg_node(root, rep, name):
    """A Segment* node for `rep`: on the Representation, else its AdaptationSet."""
    node = _child(rep, name)
    if node is not None:
        return node
    ad = _adaptation_of(root, rep)
    return _child(ad, name) if ad is not None else None


def _rep_base_url(root, rep, base_url: str) -> tuple:
    """Resolve the Representation's media BaseURL (chaining Adaptation/Rep BaseURLs).

    Returns (resolved_url, had_explicit_baseurl).
    """
    url = base_url
    present = False
    ad = _adaptation_of(root, rep)
    if ad is not None:
        b = _child(ad, "BaseURL")
        if b is not None and b.text:
            url = urljoin(url, b.text.strip())
            present = True
    b = _child(rep, "BaseURL")
    if b is not None and b.text:
        url = urljoin(url, b.text.strip())
        present = True
    return url, present


def _parse_segment_list(seg_list, base_url: str) -> tuple:
    """Parse <SegmentList>: an <Initialization> plus explicit <SegmentURL>s."""
    init_url = None
    seg_urls: list[str] = []
    for c in seg_list:
        t = _tag(c)
        if t == "Initialization":
            src = c.get("sourceURL")
            if src:
                init_url = urljoin(base_url, src)
        elif t == "SegmentURL":
            media = c.get("media")
            if media:
                seg_urls.append(urljoin(base_url, media))
    return init_url, seg_urls


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
    return parse_manifest(r.text, url, prefer=prefer)


def parse_manifest(text: str, url: str, prefer: str = "best") -> DashStream:
    """Parse MPD XML (already fetched) into a DashStream. Network-free."""
    root = ET.fromstring(text)
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
    bw, rep_id, rep, _ = chosen

    # The media BaseURL for this representation (chains Adaptation/Rep BaseURLs).
    rep_base, had_baseurl = _rep_base_url(root, rep, base_url)

    # Pick the addressing mode: SegmentTemplate > SegmentList > SegmentBase /
    # single-file BaseURL. (SegmentTemplate may sit on the Representation or its
    # AdaptationSet; same for the others.)
    tmpl = _seg_node(root, rep, "SegmentTemplate")
    seg_list = _seg_node(root, rep, "SegmentList")
    seg_base = _seg_node(root, rep, "SegmentBase")

    if tmpl is not None:
        init_url, seg_urls = _parse_segment_template(tmpl, root, rep_id, bw, rep_base)
    elif seg_list is not None:
        init_url, seg_urls = _parse_segment_list(seg_list, rep_base)
        if not seg_urls:
            raise ValueError("SegmentList had no <SegmentURL> entries")
    elif seg_base is not None or had_baseurl:
        # SegmentBase (or a bare Representation BaseURL) means one self-contained
        # file: its init box is at the start, so fetching the whole file suffices.
        init_url, seg_urls = None, [rep_base]
    else:
        raise NotImplementedError(
            "Unsupported MPD: no SegmentTemplate, SegmentList, SegmentBase or BaseURL "
            "found for the chosen representation."
        )

    return DashStream(init_url=init_url, segment_urls=seg_urls,
                      mime=rep.get("mimeType", ""), bandwidth=bw)


def _parse_segment_template(tmpl, root, rep_id: str, bw: int, base_url: str) -> tuple:
    """Build (init_url, seg_urls) for a SegmentTemplate (timeline or duration)."""
    media = tmpl.get("media", "")
    init = tmpl.get("initialization", "")
    start_number = int(tmpl.get("startNumber", "1") or 1)

    init_url = urljoin(base_url, _expand(init, rep_id, bandwidth=bw)) if init else None

    seg_urls: list[str] = []
    timeline = _child(tmpl, "SegmentTimeline")

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
    return init_url, seg_urls


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
