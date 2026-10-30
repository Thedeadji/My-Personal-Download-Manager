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

## GTK4 / libadwaita GUI (GNOME-native)

`gui-gtk/` is a second desktop front-end built with gtkmm 4 + libadwaita. It
uses the same `mpdm_core` backend, downloads list and `settings.json` as the Qt
GUI. Build either one, or both:

```bash
cd cpp
cmake -S . -B build-gtk -G Ninja -DMPDM_BUILD_GTK_GUI=ON
cmake --build build-gtk --target mpdm_gtk mpdm_host
./build-gtk/gui-gtk/mpdm_gtk
```

Needs gtkmm-4.0 ≥ 4.14 and libadwaita ≥ 1.6 (`libgtkmm-4.0-dev libadwaita-1-dev`
on Debian/Ubuntu, `gtkmm-4.0 libadwaita` on Arch). It installs `mpdm_gtk` plus
`com.mypersonaldownloadmanager.MPDM.desktop`, named after the application ID so
GNOME attributes its windows and notifications to it. Notes on behavior:

- **Single instance** comes from D-Bus (GApplication): launching it again, or
  `mpdm_gtk --background`, hands off to the running copy.
- **No tray icon.** GTK 4 has no tray API. Closing the window keeps MPDM running
  in the background, finished downloads raise desktop notifications, and
  *Quit* (Ctrl+Q / main menu) really exits.
- GUI-only preferences live in `~/.mypersonaldownloadmanager/gui-gtk.json`.
- Dev screenshots: `MPDM_STATE_DIR=/tmp/mpdm-shot mpdm_gtk --demo --shot out.png`
  (see `gui-gtk/main.cpp` for the other `--shot-*` targets).

## Browser integration

Several browsers can be connected at once, and each registration adds to the list.

- **Chrome / Chromium / Edge / Brave / Vivaldi / Opera:** load `extension/` unpacked
  (`chrome://extensions` → Developer mode → Load unpacked; the `.deb` installs it to
  `/usr/share/mpdm/extension`), copy its ID, then either use **Settings → Browser →
  Connected browsers → Add** in the app, or run
  `mpdm_host --register --extension-id <ID>`.
- **Firefox (121+):** click **Connect Firefox** in Settings, or run
  `mpdm_host --register --firefox` (the add-on ID is fixed). Then load the extension
  at `about:debugging` → This Firefox → Load Temporary Add-on → `manifest.json`.

`mpdm_host --list` shows what's registered; `mpdm_host --unregister --extension-id <ID>`
removes one entry, and a bare `mpdm_host --unregister` removes all of them.

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
