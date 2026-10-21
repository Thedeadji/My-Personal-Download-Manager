"""Bandwidth throttling — a thread-safe token-bucket rate limiter.

A download's segment workers all share one limiter, so their *combined* rate is
capped at the per-download limit; the manager can also hand every download a
second, shared limiter to cap the *global* rate across all downloads at once.

The bucket refills at `rate` bytes/second (capped to ~1s of burst). `consume(n)`
deducts n bytes and, if that drives the balance negative, sleeps just long
enough to pay it back — letting the balance go negative so concurrent callers
serialize naturally instead of all racing through. A rate of 0 means unlimited
(consume returns immediately), so throttling adds no overhead when it's off.
"""
from __future__ import annotations

import threading
import time


class RateLimiter:
    def __init__(self, rate_bps: float = 0):
        self._rate = max(0.0, float(rate_bps))
        self._tokens = 0.0
        self._last = time.monotonic()
        self._lock = threading.Lock()

    @property
    def rate(self) -> float:
        """Current limit in bytes/second (0 == unlimited)."""
        return self._rate

    def set_rate(self, rate_bps: float) -> None:
        """Change the limit on the fly; resets the bucket cleanly."""
        with self._lock:
            self._rate = max(0.0, float(rate_bps))
            self._last = time.monotonic()
            self._tokens = 0.0

    def consume(self, nbytes: int) -> None:
        """Account for nbytes of transfer, blocking as needed to honor the rate."""
        if nbytes <= 0:
            return
        with self._lock:
            if self._rate <= 0:
                return
            now = time.monotonic()
            # Refill for the elapsed time, but never bank more than ~1s of burst.
            self._tokens = min(self._rate, self._tokens + (now - self._last) * self._rate)
            self._last = now
            self._tokens -= nbytes
            wait = 0.0 if self._tokens >= 0 else -self._tokens / self._rate
        if wait > 0:
            time.sleep(wait)


_UNITS = {"k": 1024, "m": 1024 ** 2, "g": 1024 ** 3, "t": 1024 ** 4}


def parse_rate(text) -> int:
    """Parse a human rate like "500k", "1.5m", "2g", "1000000" into bytes/sec.

    Suffixes are binary (k = 1024). A trailing "b", "ib", "/s" or "ps" is ignored
    ("500kb", "500KiB/s" both work). Empty / "0" / "unlimited" / None -> 0.
    """
    if text is None:
        return 0
    s = str(text).strip().lower().replace("/s", "").replace("ps", "")
    if not s or s in ("0", "none", "unlimited", "inf"):
        return 0
    if s.endswith("b"):
        s = s[:-1]
    if s.endswith("i"):
        s = s[:-1]
    mult = 1
    if s and s[-1] in _UNITS:
        mult = _UNITS[s[-1]]
        s = s[:-1]
    try:
        value = int(float(s) * mult)
    except ValueError:
        raise ValueError(f"invalid rate: {text!r}")
    return max(0, value)


def format_rate(bps: float) -> str:
    """Render a bytes/sec rate for display (e.g. "1.5 MB/s", "unlimited")."""
    if bps <= 0:
        return "unlimited"
    n = float(bps)
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.1f} {unit}/s"
        n /= 1024
    return f"{n:.1f} TB/s"
