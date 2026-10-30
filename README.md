# MyPersonalDownloadManager (MPDM)

**MPDM is a free, open-source download manager for Windows and Linux**, written from
scratch. It speeds up downloads by fetching each file over several connections at
once, resumes interrupted downloads, grabs HLS/DASH video streams into MP4, and takes
over downloads from your browser through an extension.

This is 100% original code. It is not derived from, and contains no part of, any
commercial download manager. No license key, no registration: it's free by design
(MIT).

## 1. What it does

**Downloading**
- **Multi-connection downloads.** A file is split into byte ranges (HTTP `Range`
  requests) fetched in parallel and written straight into one pre-allocated file.
- **Dynamic segmentation.** A connection that finishes early takes over half of the
  slowest remaining range, so no connection sits idle near the end.
- **Resume.** Progress is checkpointed to a `<file>.mpdm` sidecar; an interrupted
  download continues where it stopped (when the server's size and ETag still match).
- **Video streams.** HLS (`.m3u8`, including AES-128 encrypted) and DASH (`.mpd`)
  are downloaded segment by segment and joined into an MP4 with `ffmpeg`.
- **Google Drive links** are resolved past the "can't scan for viruses" page to the
  real file and its real name.
- **Batch / wildcard URLs** such as `img[1-100].jpg` or `ep[01-12].mp4`.
- **Speed limits** per download and globally, adjustable while downloading.
- **Retries** on connection errors, with a configurable wait between tries.

**Organizing**
- **Categories.** Finished files are sorted into Video, Music, Documents,
  Compressed and Programs folders. Each category can have its own folder, extra file
  types, and you can create your own categories.
- **When a file already exists:** rename, overwrite, skip, or ask each time.
- **Temporary folder** for parts in progress, optional **server file dates**,
  and **incognito** downloads that never appear in the history.
- **Scheduler** (pause/resume or change the speed limit at set times) and a
  **"when all finish"** action (run a program, sleep, shut down).

**Using it**
- **Desktop app** with a queue, throughput graph and per-connection view; a live
  **download popup** for each download; a **tray icon** that shows progress and
  status, with a quick panel on left-click.
- **Browser extension** for Chrome, Edge, Brave, Vivaldi, Opera and Firefox:
  right-click any link → *Download with MyPersonalDownloadManager*, optionally capture
  every browser download, and a floating panel that lists the videos on a page.
  Several browsers can be connected at once.
- **Command line** (`mpdm get <url>`) and a **web dashboard** (`mpdm web`), included in
  the Linux packages (AppImage: `./MPDM-*.AppImage --cli get <url>`). On Windows, use the
  Python version (section 4) for the command line.

## 2. How it works

```
 Browser extension ──native messaging──► mpdm_host ──job file──► Desktop app (queue)
                                                                       │
 Command line / web dashboard ─────────────────────────────────────────┤
                                                                       ▼
                                                              Download manager
                                                       (queue, limits, categories,
                                                        scheduler, settings)
                                                                       │
                                                                       ▼
                                                              Download engine
                                       probe size + range support ─► split into ranges
                                       ─► N worker connections ─► write at offsets
                                       ─► checkpoint to .mpdm ─► verify ─► move/sort
```

1. **Probe.** The engine asks the server for the file size, whether it supports
   ranges, its name and its ETag.
2. **Split and download.** The file is pre-allocated and divided into ranges; a pool
   of connections downloads them, stealing work from the slowest range when idle.
   Servers without range support get a single resumable stream.
3. **Checkpoint.** Every second the per-range progress is saved to `<file>.mpdm`, so
   a crash, pause or reboot resumes instead of restarting.
4. **Finish.** The final size is checked against the server's, the file is moved
   to its category folder, and the sidecar is removed.

**Browser downloads.** The extension sends the URL, cookies and referrer (so
logged-in downloads work) to the native host `mpdm_host`. The host drops the job into
`~/.mypersonaldownloadmanager/jobs/` and starts the desktop app if it isn't running.
The app (single instance) picks the job up, adds it to the queue and opens its
download popup, so every setting applies to browser downloads too.

**Where things are kept.** Settings, the download list and the job queue live in
`~/.mypersonaldownloadmanager/` (`C:\Users\<you>\.mypersonaldownloadmanager` on
Windows).

### Project layout

```
MyPersonalDownloadManager/
├── cpp/                         The app (C++17)
│   ├── src/, include/mpdm/      Core: engine, manager, HTTP (libcurl), HLS/DASH,
│   │                            AES-128, categories, scheduler, native host, CLI, web
│   ├── gui/                     Desktop app, Qt 6 ("Industrial" design) — Windows + Linux
│   ├── gui-gtk/                 Desktop app, GTK 4 + libadwaita (GNOME-native) — Linux
│   ├── tests/                   Unit tests (CTest)
│   ├── packaging/
│   │   ├── mpdm.iss             Windows installer (Inno Setup)
│   │   └── linux/               .deb (CPack), AppImage, .desktop files, build notes
│   ├── CMakeLists.txt           Build (targets: mpdm, mpdm_host, mpdm_gui, mpdm_gtk)
│   └── vcpkg.json               Dependencies on Windows
├── extension/                   Browser extension (Manifest V3, Chrome + Firefox)
├── mpdm/                        The original Python version (engine, Tkinter UI, web)
├── tests/                       Tests for the Python version
├── install_host.py              Connects a browser to the Python version
├── .github/workflows/release.yml  Builds every package and publishes a Release on a v* tag
└── LICENSE                      MIT
```

## 3. Install

Download the latest files from the
**[Releases page](https://github.com/Thedeadji/My-Personal-Download-Manager/releases/latest)**.

### Windows

1. Download **`MPDM-Setup-<version>.exe`** and run it. Windows SmartScreen may warn
   that the app is unrecognized (the installer isn't code-signed); choose
   **More info → Run anyway**.
2. The installer adds MPDM to the Start menu (optional desktop shortcut), installs
   the browser extension to `C:\Program Files\MPDM\extension`, and connects Firefox
   automatically. No Visual C++ redistributable is needed.
3. Start **MPDM**. Closing the window keeps it running in the tray; quit from the
   tray icon's menu.

MPDM checks GitHub for new versions once a day (toggle in **Settings → General**). When one
is out, a banner offers **Update now**: it downloads the installer, verifies its checksum,
waits for your downloads to finish, then installs and restarts MPDM.

Uninstall from *Settings → Apps*; it also disconnects the browsers.

### Linux

Pick one:

| File | Works on | Notes |
|---|---|---|
| `MPDM-Qt-<version>-x86_64.AppImage` | any distro with glibc 2.39+ (Ubuntu 24.04+) | one file, nothing to install |
| `MPDM-GTK-<version>-x86_64.AppImage` | glibc 2.43+ (Ubuntu 26.04, Fedora 44, Arch) | GNOME-native look, one file |
| `mpdm-qt_<version>_amd64.deb` | Ubuntu 24.04+ / Debian 13+ | installs the Qt app |
| `mpdm-gtk_<version>_amd64.deb` | Ubuntu 26.04+ | installs the GTK app (replaces `mpdm-qt`) |

```bash
# AppImage
chmod +x MPDM-Qt-*.AppImage && ./MPDM-Qt-*.AppImage

# or .deb
sudo apt install ./mpdm-qt_*_amd64.deb
```

Install `ffmpeg` for video-stream grabbing, and `zenity` or `kdialog` for the
extension's *Save as…* dialog. The `.deb` puts the extension in
`/usr/share/mpdm/extension`; with an AppImage, download `mpdm-extension-<version>.zip`
from the release and unzip it. To build from source instead, see
[`cpp/packaging/linux/README.md`](cpp/packaging/linux/README.md).

### Connect your browser

**Chrome, Edge, Brave, Vivaldi, Opera**
1. Open `chrome://extensions` (or `edge://extensions`, …) and turn on **Developer mode**.
2. **Load unpacked** → choose the `extension` folder (Windows:
   `C:\Program Files\MPDM\extension`; Linux: `/usr/share/mpdm/extension` or the unzipped
   release zip). Copy the **ID** shown on the extension's card.
3. In MPDM: **Settings → Browser → Connected browsers**, paste the ID, click **Add**.
4. Reload the extension. Its popup → **Test connection to host** should say *Host connected ✓*.

Repeat for each browser; they're all kept in the list.

**Firefox (121+)**
1. In MPDM: **Settings → Browser → Connect Firefox** (the Windows installer already did it).
2. In Firefox open `about:debugging` → **This Firefox** → **Load Temporary Add-on…** and
   pick the extension's `manifest.json`.

Regular Firefox keeps unsigned add-ons only until it restarts; for a permanent install,
sign the extension (free, unlisted) on addons.mozilla.org or use Firefox Developer
Edition / Nightly with `xpinstall.signatures.required` set to `false`.

The command-line equivalent of these steps is `mpdm_host --register --extension-id <ID>`
or `mpdm_host --register --firefox` (`--list` and `--unregister` also exist).

## 4. The Python version (try it without installing)

The repository also contains MPDM's original **Python** implementation in `mpdm/`.
It runs straight from the cloned folder: no installer, no admin rights, nothing
copied into Program Files. It's handy for trying MPDM out or reading how a download
manager works.

```bash
git clone https://github.com/Thedeadji/My-Personal-Download-Manager.git
cd My-Personal-Download-Manager
python -m pip install -r requirements.txt      # Python 3.9+, only `requests`

python -m mpdm.ui                               # desktop window (Tkinter)
python -m mpdm get "<url>" -n 8                  # download with 8 connections
python -m mpdm grab "<playlist.m3u8>" -o out.mp4 # video stream to MP4 (needs ffmpeg)
python -m mpdm clean --purge                     # remove leftovers of interrupted downloads
python -m mpdm.web                               # web dashboard on http://127.0.0.1:8765
```

Useful `get` options: `-l 500k` speed limit, `--ua browser` browser User-Agent,
`--proxy http://host:port`, `--incognito`, and `[1-10]` ranges in the URL for batches.
Tkinter comes with the python.org installers on Windows and macOS; on Linux install
`python3-tk`.

**Browser extension with the Python version:** load `extension/` unpacked as above,
then run `python install_host.py --extension-id <ID> --browser chrome` (or `edge`,
`firefox`, `all`).

**If the installed app is on the same computer:**
- Both use the same browser-host name, so `install_host.py` takes that browser away
  from the installed app (and connecting it in the app takes it back).
  `python install_host.py --uninstall` disconnects both.
- Both use the same `~/.mypersonaldownloadmanager` folder: don't run them at the same time.
- The Python version is the original prototype. It doesn't have the tray icon and quick
  panel, "ask" for existing files, the temporary folder, server file dates, several
  connected browsers at once, or the extension's *Save as…* button.

Tests: `python tests/test_batch.py` (any `tests/test_*.py`; `test_engine.py` also needs
`pip install pytest`).

## 5. Responsible use

MPDM is a tool for downloading files you are allowed to download. Please:

- Download only content you have the right to, and respect copyright law and each
  site's terms of service.
- Don't use it to get around paywalls, DRM or other access controls. MPDM doesn't
  break DRM; the only encrypted streams it handles are standard HLS AES-128 streams
  whose keys the site itself serves to your browser.
- Be gentle with servers: many parallel connections put more load on a site. Lower
  the connection count or set a speed limit for small sites, and respect rate limits.
- MPDM identifies itself honestly by default; switching to a browser User-Agent is
  opt-in and only meant for servers that wrongly block download tools.

Released under the [MIT License](LICENSE).
