"""Unit tests for segment planning and resume metadata (no network required)."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm.engine import plan_segments
from mpdm.models import Segment, TaskRecord
from mpdm import storage


def test_plan_segments_covers_full_range():
    size = 10_000_000
    segs = plan_segments(size, connections=8, min_segment_size=256 * 1024)
    assert segs[0].start == 0
    assert segs[-1].end == size - 1
    # contiguous, non-overlapping
    for a, b in zip(segs, segs[1:]):
        assert b.start == a.end + 1
    assert sum(s.size for s in segs) == size


def test_plan_segments_small_file_single_segment():
    size = 1000
    segs = plan_segments(size, connections=8, min_segment_size=256 * 1024)
    assert len(segs) == 1
    assert segs[0].size == size


def test_segment_progress_math():
    s = Segment(index=0, start=100, end=199)
    assert s.size == 100
    s.downloaded = 40
    assert s.remaining == 60
    assert s.current_offset == 140
    assert not s.complete
    s.downloaded = 100
    assert s.complete


def test_sidecar_roundtrip():
    with tempfile.TemporaryDirectory() as d:
        dest = os.path.join(d, "file.bin")
        segs = plan_segments(5000, 4, 256 * 1024)
        segs[0].downloaded = 123
        rec = TaskRecord(url="http://x/y", dest_path=dest, size=5000, accept_ranges=True,
                         segments=storage.segments_to_dicts(segs), etag='"abc"')
        storage.save_record(dest, rec)
        loaded = storage.load_record(dest)
        assert loaded is not None
        assert loaded.size == 5000
        restored = storage.segments_from_record(loaded)
        assert restored[0].downloaded == 123
        storage.clear_record(dest)
        assert storage.load_record(dest) is None


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-v"]))
