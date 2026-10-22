# MyPersonalDownloadManager (MPDM)

A small, readable **download manager built from scratch in Python**, for
learning and research. It implements the core ideas behind modern download
accelerators — multi-connection *segmented* HTTP downloading with resume — plus
a basic HLS/DASH stream grabber that assembles video segments into an MP4.

This is **100% original code**. It is not derived from, and contains no part of,
any commercial download manager. It was written as a study of how such tools
work internally.

## Features

- **Segmented downloading** — splits a file into N byte-ranges (HTTP `Range`
  requests, RFC 7233) and fetches them over parallel connections, writing each
  piece directly to its offset in a pre-allocated file (no temp-file merge).
- **Resume** — per-segment progress is checkpointed to a `<file>.mpdm` sidecar,
  so an interrupted download continues instead of restarting. Resume is only
  reused when the server's size + `ETag` still match.
- **Graceful fallback** — servers without range support get a single resumable
  stream.
- **Speed limiting** — cap the rate per download or globally across all
  downloads, adjustable while a download runs (token-bucket throttle).
- **Scheduler** — start/pause the queue or change the global speed limit at set
  times (daily windows or a one-shot), e.g. download only during off-peak hours.
- **Integrity check** — final file size is verified against `Content-Length`.
- **Auto-sort by type** — finished downloads are filed into IDM-style subfolders
  (Video, Music, Documents, Compressed, Programs); toggle in Settings.
- **HLS/DASH grabber** — parses `.m3u8` / `.mpd` manifests, downloads the
  segments concurrently, and muxes them to MP4 via `ffmpeg`. **AES-128**
  encrypted HLS is supported (keys/IVs tracked per segment, with key rotation),
  using a built-in pure-Python AES — no extra dependency required.
- **Three ways to use it** — a library API, a CLI, and a minimal Tkinter GUI.

## Install

```bash
cd MyPersonalDownloadManager
python -m pip install -r requirements.txt      # just: requests
# ffmpeg is only needed for the `grab` (HLS/DASH) feature:
#   Windows: https://ffmpeg.org/download.html  (add ffmpeg.exe to PATH)
```

## Use it

### CLI

```bash
# Multi-connection file download into your Downloads folder
python -m mpdm get "https://example.com/big.zip" -n 8

# Choose output path and directory
python -m mpdm get "https://example.com/big.zip" -o "C:\Users\me\Downloads\big.zip"

# Cap the download speed (per download). Accepts 500k, 1.5m, 2g, or raw bytes.
python -m mpdm get "https://example.com/big.zip" -l 500k

# Some servers reject non-browser clients (HTTP 403). Opt into a browser
# User-Agent (or pass any custom string). Default stays an honest MPDM UA.
python -m mpdm get "https://example.com/big.zip" --ua browser

# Grab an HLS stream to MP4 (needs ffmpeg)
python -m mpdm grab "https://example.com/playlist.m3u8" -o movie.mp4 --quality 720

# Clean up orphaned resume sidecars (.mpdm) and temp files left by an
# interrupted/killed download; add --purge to also delete the incomplete files
python -m mpdm clean --purge
```

If a download is interrupted, just run the same `get` command again — MPDM finds
the matching `.mpdm` sidecar and **resumes** it instead of making a new copy.

### GUI (download manager)

```bash
python -m mpdm.ui          # the home window (same as python -m mpdm.ui.app)
```

A desktop download-manager window: a toolbar (Add URL, Resume, Stop/Pause,
Remove, Clear finished, Open folder, **Scheduler**, Settings, plus Pause-all /
Resume-all and a **global speed-limit** box), a category sidebar (All,
Downloading, Finished, Video, Audio, Documents, Archives, Programs, Other), and a
live table with per-row name / size / % / status / speed / time-left. Multiple
downloads run at once (set the limit in Settings → "Max simultaneous downloads");
each can be paused, resumed, removed, or given its **own speed limit** (right-click
→ "Set speed limit…"). **Settings** also holds the global limit and the
**browser User-Agent** toggle; **Scheduler** opens an editor for daily/one-shot
rules (pause/resume the queue or change the global limit at set times). The list
persists across restarts, and Add URL pre-fills from your clipboard. No license or
registration — it's free and open.

### Web UI (browser dashboard)

```bash
python -m mpdm.web                 # serve on http://127.0.0.1:8765 and open a browser
python -m mpdm.web --port 9000 --no-open
```

A dependency-free local dashboard (standard-library `http.server` + one HTML
page) over the same engine: add a URL, watch per-row size / progress / status /
speed / time-left, Stop/Resume/Remove each, set a per-download or global speed
limit, Pause-all / Resume-all / Clear finished. Binds to localhost only. Handy
when you want a browser UI or a headless box you drive from another machine
(use `--host 0.0.0.0` at your own risk on trusted networks).

### Browser integration (extension + native host)

Send downloads straight from Chrome/Edge to the MPDM engine — the standards-based
way, with no kernel driver or memory hooks. The extension captures the URL plus
cookies and referrer (so logged-in downloads work) and hands them to a small
native-messaging host that drives the engine.

```bash
# 1) Load the unpacked extension from the `extension/` folder at chrome://extensions
#    (Developer mode -> Load unpacked), then copy its extension ID.
# 2) Register the native host:
python install_host.py --extension-id <THE_ID> --browser chrome
```

Then right-click any link/media → “Download with MyPersonalDownloadManager”. A
small **progress popup** opens for each browser download (filename, speed,
downloaded / total, and Stop/Resume) — like IDM's download dialog. Set the
environment variable `MDM_NO_POPUP=1` to download silently instead.

On pages with video or audio, a **floating “⬇ Download media” panel** appears
(like IDM's video panel): it lists the page's HTML5 media **and** any HLS/DASH
streams sniffed from the network, and one click sends the chosen item to MPDM
(HLS/DASH are grabbed and muxed to MP4). Toggle it with **“Show video download
panel”** in the extension popup. Full steps are in `extension/README.md`.

### Library

```python
from mpdm.engine import download
from mpdm.config import Settings

path = download(
    "https://example.com/big.zip",
    settings=Settings(connections=8),
    progress_cb=lambda p: print(p.percent, p.speed_bps),
)
print("saved to", path)
```

## How it works

```
                 ┌──────────────┐
   URL  ───────► │  http_client │  probe(): ranged GET of byte 0 →
                 │   (probe)    │  learns size + Range support + filename
                 └──────┬───────┘
                        ▼
                 ┌──────────────┐   plan_segments(): divide [0,size)
                 │    engine    │   into N contiguous segments
                 │ (segmented)  │
                 └──────┬───────┘
          ┌────────────┼────────────┐      each worker:
          ▼            ▼            ▼       Range: bytes=start-end
      worker 0     worker 1     worker N →  seek(start); write chunks
          └────────────┼────────────┘       (progress checkpointed to .mpdm)
                        ▼
                pre-allocated output file  ──►  size verified ──► done
```

The stream grabber (`grabber.py`) reuses the same HTTP layer: it resolves a
manifest (`hls.py` / `dash.py`) to a list of segment URLs, downloads them
concurrently, then concatenates + remuxes to MP4 (`muxer.py`).

## Project layout

```
MyPersonalDownloadManager/
├── README.md
├── LICENSE
├── requirements.txt
└── mpdm/
    ├── __init__.py
    ├── __main__.py        # python -m mpdm
    ├── config.py          # Settings / defaults
    ├── models.py          # Segment, Progress, TaskRecord, Status
    ├── http_client.py     # probe() + ranged GET
    ├── engine.py          # the segmented download core
    ├── storage.py         # .mpdm resume sidecar
    ├── hls.py             # m3u8 parser
    ├── dash.py            # mpd parser
    ├── muxer.py           # ffmpeg segment assembly
    ├── grabber.py         # manifest → MP4 orchestration
    ├── cli.py             # argparse CLI
    └── ui/app.py          # Tkinter GUI stub
└── tests/
    └── test_engine.py     # offline unit tests
```

## Run the tests

```bash
python -m pytest tests/ -v      # or: python tests/test_engine.py
```

## Extending it (good next steps)

- **SAMPLE-AES HLS**: the partial-encryption mode (AES-128 full-segment is
  already supported); needs container-aware decryption — marked in `hls.py`.
- **Standalone installer**: bundle into a single `.exe` with PyInstaller and an
  Inno Setup / NSIS installer (detect/bundle ffmpeg; optionally register the
  native host).
- **Web UI**: an optional FastAPI + HTML dashboard over the same engine, as a
  lighter-footprint alternative to the Tkinter window.

## Responsible use

Download only content you have the right to download, and respect each site's
terms of service and applicable copyright law.
