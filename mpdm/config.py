"""Configuration and defaults for MPDM."""
from __future__ import annotations

import json
import os
from dataclasses import dataclass, field, asdict
from pathlib import Path

APP_DIRNAME = ".mypersonaldownloadmanager"

# Incognito downloads go into this hidden subfolder of the download dir and are
# never written to the app's history.
INCOGNITO_DIRNAME = "InvisibleFile"

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
    # Dynamic segmentation: when a connection finishes, split the segment with the
    # most bytes left and steal half, so no connection goes idle before the end.
    dynamic_segmentation: bool = True
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
    # Per-category destination overrides, e.g. {"Video": "D:/Movies"}. Absolute
    # paths used as-is; relative joined to output_dir. Empty = default subfolders.
    category_dirs: dict = field(default_factory=dict)

    def as_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_env(cls) -> "Settings":
        return apply_env(cls())


def apply_env(s: "Settings") -> "Settings":
    """Overlay MDM_* environment variables onto a Settings (env wins)."""
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
    if os.environ.get("MDM_DYNAMIC"):
        s.dynamic_segmentation = os.environ["MDM_DYNAMIC"].strip().lower() not in ("0", "false", "no", "")
    return s


# ---- persisted preferences (settings.json) -------------------------------
def _prefs_path() -> Path:
    return state_dir() / "settings.json"


def load_prefs() -> dict:
    """Read the persisted preferences file, or {} if absent/invalid."""
    try:
        return json.loads(_prefs_path().read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def save_prefs(prefs: dict) -> None:
    try:
        p = _prefs_path()
        tmp = str(p) + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(prefs, f, indent=2)
        os.replace(tmp, p)
    except OSError:
        pass


def settings_from_prefs(prefs: dict) -> "Settings":
    """Build Settings from defaults, overlaid by the prefs file, then env."""
    s = Settings()
    for k, v in (prefs.get("settings") or {}).items():
        if k in Settings.__dataclass_fields__:
            setattr(s, k, v)
    return apply_env(s)
