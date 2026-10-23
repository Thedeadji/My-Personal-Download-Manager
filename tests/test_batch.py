"""Tests for batch / wildcard URL expansion."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm.batch import expand_pattern, expand_urls, MAX_EXPANSION


def test_no_pattern():
    assert expand_pattern("https://h/file.zip") == ["https://h/file.zip"]


def test_numeric_range():
    assert expand_pattern("https://h/img[1-3].jpg") == [
        "https://h/img1.jpg", "https://h/img2.jpg", "https://h/img3.jpg"]


def test_zero_padded_range():
    assert expand_pattern("https://h/f[001-003].zip") == [
        "https://h/f001.zip", "https://h/f002.zip", "https://h/f003.zip"]


def test_descending_range():
    assert expand_pattern("https://h/[3-1].ts") == [
        "https://h/3.ts", "https://h/2.ts", "https://h/1.ts"]


def test_alpha_range():
    assert expand_pattern("https://h/ch[a-c].ts") == [
        "https://h/cha.ts", "https://h/chb.ts", "https://h/chc.ts"]


def test_cartesian_multiple_ranges():
    out = expand_pattern("https://h/[a-b]/[1-2].ts")
    assert out == [
        "https://h/a/1.ts", "https://h/a/2.ts",
        "https://h/b/1.ts", "https://h/b/2.ts"]


def test_expand_urls_list_and_dedupe():
    text = "https://h/a.zip\nhttps://h/img[1-2].png  https://h/a.zip"
    assert expand_urls(text) == [
        "https://h/a.zip", "https://h/img1.png", "https://h/img2.png"]


def test_expand_cap():
    try:
        expand_pattern(f"https://h/[1-{MAX_EXPANSION + 5}].bin")
    except ValueError:
        pass
    else:
        raise AssertionError("expected ValueError for oversized expansion")


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("all batch tests passed")
