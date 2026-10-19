"""Configuration and defaults for MPDM."""
from __future__ import annotations

import os
from dataclasses import dataclass, field, asdict
from pathlib import Path

APP_DIRNAME = ".mypersonaldownloadmanager"


def default_download_dir() -> Path:
    """Pick a sensible default output directory cross-platform."""
    home = Path.home()
    dl = home / "Downloads"
    return dl if dl.is_dir() else home


def state_dir() -> Path:
    """Where MPDM keeps its own state (history, logs)."""
    d = Path.home() / APP_DIRNAME
    d.mkdir(parents=True, exist_ok=True)
    return d


@dataclass
class Settings:
    """User-tunable settings. Mirrors the knobs a real DM exposes."""

    # Number of parallel connections per download (IDM-style segmentation).
    connections: int = 8
    # Minimum bytes a segment must be worth splitting into; tiny files use 1.
    min_segment_size: int = 256 * 1024  # 256 KiB
    # Socket/connection timeouts.
    connect_timeout: float = 15.0
    read_timeout: float = 60.0
    # How many times to retry a failed segment chunk before giving up.
    max_retries: int = 5
    retry_backoff: float = 1.5  # exponential base, seconds
    # Chunk size for streaming reads/writes.
    chunk_size: int = 1024 * 64  # 64 KiB
    # Identify ourselves honestly rather than impersonating a browser.
    user_agent: str = "MyPersonalDownloadManager/0.1 (+research)"
    # Default output directory.
    output_dir: str = field(default_factory=lambda: str(default_download_dir()))
    # Verify TLS certificates (leave True; exposed for testing only).
    verify_tls: bool = True

    def as_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_env(cls) -> "Settings":
        s = cls()
        if os.environ.get("MDM_CONNECTIONS"):
            try:
                s.connections = max(1, int(os.environ["MDM_CONNECTIONS"]))
            except ValueError:
                pass
        if os.environ.get("MDM_OUTPUT_DIR"):
            s.output_dir = os.environ["MDM_OUTPUT_DIR"]
        return s
