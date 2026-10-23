"""Tests for dynamic segmentation (work-stealing split logic)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm.engine import Download, plan_segments
from mpdm.config import Settings
from mpdm.models import Segment


def _covers(segments, size) -> bool:
    """True if the segments tile [0, size) contiguously with no gaps/overlaps."""
    s = sorted(segments, key=lambda x: x.start)
    if s[0].start != 0 or s[-1].end != size - 1:
        return False
    for a, b in zip(s, s[1:]):
        if b.start != a.end + 1:
            return False
    return sum(x.size for x in s) == size


def test_split_produces_valid_partition():
    size = 1_000_000
    d = Download("http://x", settings=Settings(connections=4, min_segment_size=50_000,
                                               dynamic_segmentation=True))
    d.segments = [Segment(index=0, start=0, end=size - 1)]
    d._claimed = set()
    d._seg_counter = 0

    handed = []
    for _ in range(200):
        seg = d._claim_or_split()
        if seg is None:
            break
        handed.append(seg)

    assert len(d.segments) > 1                      # stealing actually split work
    assert _covers(d.segments, size)                # still a clean partition
    assert min(s.size for s in d.segments) >= d.settings.min_segment_size
    # Every handed-out segment is unique (no double-claim).
    assert len({id(s) for s in handed}) == len(handed)


def test_no_split_when_disabled():
    size = 1_000_000
    d = Download("http://x", settings=Settings(connections=4, min_segment_size=50_000,
                                               dynamic_segmentation=False))
    d.segments = plan_segments(size, 4, 50_000)
    d._claimed = set()
    d._seg_counter = max(s.index for s in d.segments)

    claimed = [d._claim_or_split() for _ in range(len(d.segments))]
    assert all(c is not None for c in claimed)      # each existing segment handed out once
    assert d._claim_or_split() is None              # no stealing when disabled
    assert len(d.segments) == 4


def test_no_split_below_threshold():
    # A segment with less than 2*min_segment_size left must not be split.
    d = Download("http://x", settings=Settings(min_segment_size=100_000,
                                               dynamic_segmentation=True))
    d.segments = [Segment(index=0, start=0, end=150_000 - 1)]  # 150k < 200k threshold
    d._claimed = set()
    d._seg_counter = 0
    assert d._claim_or_split() is d.segments[0]      # claimed
    assert d._claim_or_split() is None               # too small to split
    assert len(d.segments) == 1


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("all dynamic-segmentation tests passed")
