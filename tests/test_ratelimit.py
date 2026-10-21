"""Tests for the token-bucket rate limiter and rate parsing (timing-based)."""
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mpdm.ratelimit import RateLimiter, parse_rate, format_rate


def test_parse_rate_units():
    assert parse_rate("500k") == 500 * 1024
    assert parse_rate("500kb") == 500 * 1024
    assert parse_rate("500KiB/s") == 500 * 1024
    assert parse_rate("1.5m") == int(1.5 * 1024 ** 2)
    assert parse_rate("2g") == 2 * 1024 ** 3
    assert parse_rate("1000000") == 1_000_000
    assert parse_rate("2mbps") == 2 * 1024 ** 2


def test_parse_rate_unlimited():
    for v in (None, "", "0", "unlimited", "none"):
        assert parse_rate(v) == 0


def test_parse_rate_invalid():
    try:
        parse_rate("fast")
    except ValueError:
        pass
    else:
        raise AssertionError("expected ValueError")


def test_format_rate():
    assert format_rate(0) == "unlimited"
    assert format_rate(1024) == "1.0 KB/s"
    assert format_rate(1024 ** 2) == "1.0 MB/s"


def test_unlimited_is_instant():
    rl = RateLimiter(0)
    t0 = time.monotonic()
    for _ in range(1000):
        rl.consume(1_000_000)
    assert time.monotonic() - t0 < 0.1   # no blocking when unlimited


def test_limited_throughput_matches_rate():
    rate = 100_000  # bytes/sec
    total = 30_000
    rl = RateLimiter(rate)
    t0 = time.monotonic()
    for _ in range(3):
        rl.consume(total // 3)
    elapsed = time.monotonic() - t0
    expected = total / rate  # 0.3s
    # Generous bounds to stay robust on slow/loaded CI and Windows timers.
    assert 0.8 * expected <= elapsed <= 3.0 * expected, elapsed


def test_shared_limiter_caps_aggregate_across_threads():
    rate = 200_000
    per_thread = 25_000
    nthreads = 4  # total 100_000 bytes -> ~0.5s at the shared cap
    rl = RateLimiter(rate)

    def worker():
        for _ in range(5):
            rl.consume(per_thread // 5)

    t0 = time.monotonic()
    threads = [threading.Thread(target=worker) for _ in range(nthreads)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t0
    expected = (per_thread * nthreads) / rate  # 0.5s
    # Must actually throttle (not run at full speed), within a loose ceiling.
    assert elapsed >= 0.6 * expected, f"not throttled: {elapsed}"
    assert elapsed <= 3.0 * expected, f"too slow: {elapsed}"


def test_set_rate_live():
    rl = RateLimiter(0)
    assert rl.rate == 0
    rl.set_rate(50_000)
    assert rl.rate == 50_000
    t0 = time.monotonic()
    rl.consume(25_000)  # ~0.5s at 50k/s
    assert time.monotonic() - t0 >= 0.3
    rl.set_rate(0)       # back to unlimited
    t0 = time.monotonic()
    rl.consume(10_000_000)
    assert time.monotonic() - t0 < 0.1


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
    print("all ratelimit tests passed")
