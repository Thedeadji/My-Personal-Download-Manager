"""Batch and wildcard URL expansion (IDM-style).

Turns one pattern — or a pasted list — into many concrete URLs:

    https://host/img[1-100].jpg          -> img1.jpg … img100.jpg
    https://host/f[001-010].zip          -> f001.zip … f010.zip   (zero-padded)
    https://host/ch[a-e]/[1-3].ts        -> cha/1.ts … che/3.ts   (cartesian)

Multiple ranges combine (cartesian product), capped to avoid blow-ups. Text with
several URLs (newlines / spaces / commas) is split and each part expanded.
"""
from __future__ import annotations

import re
import string

_RANGE = re.compile(r"\[(?:(\d+)-(\d+)|([a-zA-Z])-([a-zA-Z]))\]")
MAX_EXPANSION = 10000  # safety cap on how many URLs one pattern may produce


def _tokens(a_num, b_num, a_ch, b_ch):
    """Yield the string values for one [range] token."""
    if a_num is not None:
        start, end = int(a_num), int(b_num)
        step = 1 if end >= start else -1
        width = len(a_num) if a_num.startswith("0") else 0  # zero-pad to start width
        for n in range(start, end + step, step):
            yield str(n).zfill(width) if width else str(n)
    else:
        seq = string.ascii_uppercase if a_ch.isupper() else string.ascii_lowercase
        i, j = seq.index(a_ch), seq.index(b_ch)
        step = 1 if j >= i else -1
        for k in range(i, j + step, step):
            yield seq[k]


def expand_pattern(url: str) -> list:
    """Expand all [a-b] range tokens in a single URL into concrete URLs."""
    m = _RANGE.search(url)
    if not m:
        return [url]
    head, tail = url[:m.start()], url[m.end():]
    out = []
    for tok in _tokens(*m.groups()):
        for rest in expand_pattern(head + tok + tail):
            out.append(rest)
            if len(out) > MAX_EXPANSION:
                raise ValueError(f"pattern expands to more than {MAX_EXPANSION} URLs")
    return out


def expand_urls(text: str) -> list:
    """Split text into URLs (newlines/spaces/commas) and expand each pattern.

    Duplicates are removed while preserving order.
    """
    parts = re.split(r"[\s,]+", (text or "").strip())
    seen, out = set(), []
    for part in parts:
        if not part:
            continue
        for u in expand_pattern(part):
            if u not in seen:
                seen.add(u)
                out.append(u)
    return out
