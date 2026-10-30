# MyPersonalDownloadManager (MPDM)

A small, readable **download manager built from scratch in Python**, for
learning and research. It implements the core ideas behind modern download
accelerators — multi-connection *segmented* HTTP downloading with resume — plus
a basic HLS/DASH stream grabber that assembles video segments into an MP4.

This is **100% original code**. It is not derived from, and contains no part of,
any commercial download manager. It was written as a study of how such tools
work internally.

## Two ways to run it

MPDM comes in two forms that share the same design and the same browser extension:

| | **Installed app** (C++ / Qt) | **Python version** |
|---|---|---|
| Get it | Windows installer (`MPDM-Setup-*.exe`), or build the Linux `.deb` (see `cpp/packaging/linux/README.md`) | Clone the repo and run it — **nothing to install** |
| Needs | nothing extra on Windows | Python 3.9+ and `requests` |
| Best for | everyday use | trying MPDM out, studying the code, running on a machine where you can't install software |
| Desktop UI | full "Industrial" window, tray icon + quick panel, live download popup | simpler Tkinter window and download popup |

### Try it without installing (Python)

If you just want to test MPDM, you don't need the installer. The Python version
runs straight from the cloned folder — no admin rights, nothing copied into
Program Files, no registry changes (unless you connect a browser, below).

```bash
git clone https://github.com/Thedeadji/My-Personal-Download-Manager.git
cd My-Personal-Download-Manager
python -m pip install -r requirements.txt   # only `requests`

python -m mpdm.ui                            # desktop window
python -m mpdm get "<url>"                   # or download from the command line
python -m mpdm.web                           # or a dashboard at http://127.0.0.1:8765
```

- **Tkinter** (for the window) ships with the python.org installers on Windows and
  macOS; on Linux install it with `sudo apt install python3-tk`.
- **ffmpeg** is only needed for grabbing HLS/DASH video streams.
- To stop, just close it. To remove it, delete the folder (and, if you like,
  `~/.mypersonaldownloadmanager`).

**Testing the browser extension with the Python version** — load `extension/`
unpacked (see *Browser integration* below), copy its ID, then:

```bash
python install_host.py --extension-id <THE_ID> --browser chrome   # or edge / firefox / all
```

Things to know before mixing the two versions on one computer:

- **They share one browser connection.** Both register the same native-messaging
  host name, so running `install_host.py` points that browser at the Python version
  and the installed app stops receiving its downloads (and vice versa). To switch
  back, open the installed app → **Settings → Browser → Connected browsers** and
  add the extension ID again. `python install_host.py --uninstall` removes the
  registration for *both* versions.
- **They share one settings/queue folder** (`~/.mypersonaldownloadmanager`), so
  don't run both at the same time.
- **The Python version is the original prototype** and lacks the installed app's
  newer features: the tray icon and quick panel, "ask me" for duplicate files, the
  temporary download folder, server file dates, several browsers connected at
  once, and the extension's **Save as…** button (it does nothing with the Python
  host).

Tests: run any file directly, e.g. `python tests/test_batch.py`
(`tests/test_engine.py` additionally needs `pip install pytest`).

## Features

- **Segmented downloading** — splits a file into N byte-ranges (HTTP `Range`
  requests, RFC 7233) and fetches them over parallel connections, writing each
  piece directly to its offset in a pre-allocated file (no temp-file merge).
- **Dynamic segmentation** — when a connection finishes early, it splits the
  segment with the most bytes left and steals half, so no connection sits idle
  before the download ends (toggle in Settings).
- **Resume** — per-segment progress is checkpointed to a `<file>.mpdm` sidecar,
  so an interrupted download continues instead of restarting. Resume is only
  reused when the server's size + `ETag` still match.
- **Graceful fallback** — servers without range support get a single resumable
  stream.
- **Speed limiting** — cap the rate per download or globally across all
  downloads, adjustable while a download runs (token-bucket throttle).
- **Batch / wildcard URLs** — expand patterns like `img[1-100].jpg`,
  `f[001-010].zip`, `ch[a-e]/[1-3].ts`, or a pasted list, into many downloads.
- **Proxy support** — route requests through an HTTP/SOCKS proxy.
- **Google Drive links** — share/`uc` URLs are resolved to the real file
  (handles the "can't scan for viruses" confirmation page) and named correctly.
- **Incognito downloads** — kept out of the app's history and saved into a hidden
  `InvisibleFile` folder; still pausable/resumable while the app is open.
- **Finish actions** — run a program, sleep, or shut down when the queue is done.
- **Clipboard monitoring** — the desktop app can offer to grab copied links.
- **Scheduler** — start/pause the queue or change the global speed limit at set
  times (daily windows or a one-shot), e.g. download only during off-peak hours.
- **Integrity check** — final file size is verified against `Content-Length`.
- **Auto-sort by type** — finished downloads are filed into IDM-style subfolders
  (Video, Music, Documents, Compressed, Programs); toggle in Settings, and point
  any category at its own folder/drive (Settings → Category folders…).
- **Persistent settings** — your preferences are saved to
  `~/.mypersonaldownloadmanager/settings.json` and restored on the next launch.
- **HLS/DASH grabber** — parses `.m3u8` / `.mpd` manifests, downloads the
  segments concurrently, and muxes them to MP4 via `ffmpeg`. **AES-128**
  encrypted HLS is supported (keys/IVs tracked per segment, with key rotation),
  using a built-in pure-Python AES — no extra dependency required.
- **Three ways to use it** — a library API, a CLI, and a minimal Tkinter GUI.

## Install (Python version)

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

# Batch/wildcard: download many at once (expands the [a-b] ranges)
python -m mpdm get "https://example.com/img[1-50].jpg"

# Through a proxy
python -m mpdm get "https://example.com/big.zip" --proxy http://127.0.0.1:8080

# Incognito: save into a hidden InvisibleFile folder, no history record
python -m mpdm get "https://example.com/secret.zip" --incognito

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
