"""Data models and shared types for MPDM."""
from __future__ import annotations

import enum
import time
from dataclasses import dataclass, field, asdict
from typing import Optional


class Status(str, enum.Enum):
    PENDING = "pending"
    PROBING = "probing"
    DOWNLOADING = "downloading"
    PAUSED = "paused"
    MERGING = "merging"
    COMPLETED = "completed"
    FAILED = "failed"


@dataclass
class Segment:
    """One byte-range of the target file, downloaded over its own connection."""

    index: int
    start: int          # inclusive byte offset
    end: int            # inclusive byte offset
    downloaded: int = 0  # bytes written for this segment so far

    @property
    def size(self) -> int:
        return self.end - self.start + 1

    @property
    def remaining(self) -> int:
        return max(0, self.size - self.downloaded)

    @property
    def complete(self) -> bool:
        return self.downloaded >= self.size

    @property
    def current_offset(self) -> int:
        """Absolute file offset to resume writing this segment at."""
        return self.start + self.downloaded


@dataclass
class RemoteInfo:
    """What we learned about the URL before downloading."""

    url: str
    final_url: str
    size: Optional[int]       # Content-Length, or None if unknown
    accept_ranges: bool       # server supports Range requests
    filename: str
    etag: Optional[str] = None
    last_modified: Optional[str] = None
    content_type: Optional[str] = None


@dataclass
class Progress:
    """Snapshot of download progress, passed to UI callbacks."""

    total: Optional[int]
    downloaded: int
    speed_bps: float
    status: Status
    segments: int
    active_segments: int
    eta_seconds: Optional[float] = None
    filename: str = ""

    @property
    def percent(self) -> Optional[float]:
        if not self.total:
            return None
        return min(100.0, self.downloaded * 100.0 / self.total)


@dataclass
class TaskRecord:
    """Serializable record of a download task (used for resume + history)."""

    url: str
    dest_path: str
    size: Optional[int]
    accept_ranges: bool
    segments: list = field(default_factory=list)  # list[Segment-as-dict]
    status: str = Status.PENDING.value
    etag: Optional[str] = None
    last_modified: Optional[str] = None
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)

    def to_dict(self) -> dict:
        return asdict(self)
