"""Configuration and defaults for MPDM."""
from __future__ import annotations

import os
from dataclasses import dataclass, field, asdict
from pathlib import Path

APP_DIRNAME = ".mypersonaldownloadmanager"

# Our honest default identifier.
DEFAULT_USER_AGENT = "MyPersonalDownloadManager/0.1 (+research)"
# A realistic desktop-Chrome UA, used only when the user opts in because a server
# rejects non-browser clients (some hosts 403 an honest UA). Off by default.
BROWSER_USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
)


def resolve_user_agent(value) -> str:
    """Map a UA choice to a header string.

    None/"" -> honest default; "browser"/"chrome" -> the browser preset;
    anything else is used verbatim as a custom User-Agent.
    """
    if value is None:
        return DEFAULT_USER_AGENT
    v = str(value).strip()
    if not v or v.lower() in ("default", "honest"):
        return DEFAULT_USER_AGENT
    if v.lower() in ("browser", "chrome"):
        return BROWSER_USER_AGENT
    return v


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
    # Per-download speed limit in bytes/sec (0 = unlimited). Shared across this
    # download's segment workers, so it caps their combined rate.
    max_rate_bps: int = 0
    # Sort finished downloads into IDM-style subfolders by type (Video, Music,
    # Documents, Compressed, Programs); "Other" types stay in output_dir.
    categorize: bool = True
    # Identify ourselves honestly by default; can be overridden (e.g. a browser
    # UA) for servers that reject non-browser clients. See resolve_user_agent().
    user_agent: str = DEFAULT_USER_AGENT
    # Default output directory.
    output_dir: str = field(default_factory=lambda: str(default_download_dir()))
    # Verify TLS certificates (leave True; exposed for testing only).
    verify_tls: bool = True
    # Optional proxy for all requests, e.g. "http://host:8080" or
    # "socks5://host:1080" (empty = direct / honor system env proxies).
    proxy: str = ""

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
        if os.environ.get("MDM_MAX_RATE"):
            from .ratelimit import parse_rate
            s.max_rate_bps = parse_rate(os.environ["MDM_MAX_RATE"])
        if os.environ.get("MDM_USER_AGENT"):
            s.user_agent = resolve_user_agent(os.environ["MDM_USER_AGENT"])
        if os.environ.get("MDM_CATEGORIZE"):
            s.categorize = os.environ["MDM_CATEGORIZE"].strip().lower() not in ("0", "false", "no", "")
        if os.environ.get("MDM_PROXY"):
            s.proxy = os.environ["MDM_PROXY"].strip()
        return s
