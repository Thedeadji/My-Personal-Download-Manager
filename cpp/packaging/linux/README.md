# Building MPDM for Linux

MPDM's code is portable (Qt 6 + libcurl + `std::filesystem`; all Windows-specific
calls are `#ifdef _WIN32`-guarded). These files build it on Linux and package it as
a `.deb` and a `.tar.gz`.

> **Status:** built and tested on Ubuntu 24.04 (Qt 6.4, GCC 13): unit tests pass,
> the GUI runs, and browser downloads are handed from the native host to the app
> (queue + download popup), exactly like on Windows. Qt 6.4+ is supported.

## Quick start

```bash
cd cpp
packaging/linux/build-linux.sh        # system libraries
# or, with vcpkg:
packaging/linux/build-linux.sh -DCMAKE_TOOLCHAIN_FILE=/path/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Installers land in `cpp/build-linux/` (`mpdm-1.0.0-*.deb`, `mpdm-1.0.0-*.tar.gz`).

Install:

```bash
sudo apt install ./build-linux/mpdm-1.0.0-*.deb
```

This installs `mpdm` (CLI), `mpdm_gui` (desktop app), and `mpdm_host` (browser
native host) to `/usr/bin`, plus the `.desktop` entry and icon.

## Dependencies (Debian/Ubuntu example)

```bash
sudo apt install cmake ninja-build g++ qt6-base-dev \
     libcurl4-openssl-dev nlohmann-json3-dev libpugixml-dev libssl-dev ffmpeg
# cpp-httplib: libhttplib-dev if packaged, otherwise build via vcpkg.
```

## Browser integration

After installing, register the native messaging host for your browser:

```bash
mpdm_host --register --extension-id <ID from chrome://extensions>
```

This writes `~/.config/<browser>/NativeMessagingHosts/com.mypersonaldownloadmanager.host.json`
for Chrome / Chromium / Edge (whichever are installed). Load `extension/` unpacked
in the browser to get its ID first (the `.deb` installs it to
`/usr/share/mpdm/extension`). You can also register from the app: **Settings →
Browser → Browser host**.

## How browser downloads reach the app

The extension talks to `mpdm_host`. For each download the host writes the job to
`~/.mypersonaldownloadmanager/jobs/` and launches `mpdm_gui --background` fully
detached (it looks next to itself, then in `gui/` of a build tree, then on `PATH`).
A running copy adopts the job (single instance), adds it to the queue and opens the
download popup, so every setting applies the same way it does on Windows. If the
GUI can't be found, the host downloads headlessly so nothing is lost.

## Platform differences (Linux vs Windows)

- **Tray:** on desktops without a system tray (stock GNOME needs the AppIndicator
  extension), the window is shown instead of hiding to a tray icon that isn't there.
- **Autostart** is written as `~/.config/autostart/mpdm.desktop`.
- **System proxy** reads `https_proxy` / `http_proxy` / `all_proxy` (Windows reads
  the Internet Settings registry).
- **Default download folder** follows `XDG_DOWNLOAD_DIR` (localized folder names work).
- **Incognito** saves into the hidden `.InvisibleFile` folder.
- **"Save as..."** in the extension uses `zenity` or `kdialog`.
- `ffmpeg` must be on `PATH` for HLS/DASH stream grabbing.
