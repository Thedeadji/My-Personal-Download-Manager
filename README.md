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
- **Integrity check** — final file size is verified against `Content-Length`.
- **HLS/DASH grabber** — parses `.m3u8` / `.mpd` manifests, downloads the
  segments concurrently, and muxes them to MP4 via `ffmpeg`.
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
python -m mpdm.ui.app
```

A desktop download-manager window: a toolbar (Add URL, Resume, Pause, Remove,
Clear finished, Open folder, Settings), a category sidebar (All, Downloading,
Finished, Video, Audio, Documents, Archives, Programs, Other), and a live table
with per-row name / size / % / status / speed / time-left. Multiple downloads run
at once (set the limit in Settings → "Max simultaneous downloads"); each can be
paused, resumed, or removed, and the list persists across restarts. Add URL
pre-fills from your clipboard if it holds a link. No license or registration —
it's free and open.

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

Then right-click any link/media → “Download with MyPersonalDownloadManager”. Full steps
are in `extension/README.md`.

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

- **Encrypted HLS**: fetch the `#EXT-X-KEY` key and AES-128-CBC decrypt each
  segment before muxing (marked extension point in `hls.py` / `grabber.py`).
- **DASH SegmentList/SegmentBase**: handle the non-template manifest shapes
  (marked in `dash.py`).
- **Per-download speed limiting** and a global scheduler/queue.
- **Browser capture**: a browser extension + a native-messaging host that hands
  URLs to the engine — the clean, documented integration path (no kernel
  drivers needed).

## Responsible use

Download only content you have the right to download, and respect each site's
terms of service and applicable copyright law.
