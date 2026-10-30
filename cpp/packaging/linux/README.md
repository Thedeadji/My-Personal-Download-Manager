# Building MPDM for Linux

MPDM's code is portable (Qt 6 + libcurl + `std::filesystem`; all Windows-specific
calls are `#ifdef _WIN32`-guarded). These files build it on Linux and package it as
a `.deb` and a `.tar.gz`.

> **Status:** the Linux build has not yet been CI-tested. Treat the first build as a
> port bring-up — expect to install a dependency or two. The core engine, CLI,
> Qt GUI, and web dashboard are expected to work; see *Platform differences* below.

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
in the browser to get its ID first.

## Platform differences (Linux vs Windows)

- **Autostart** ("Launch MPDM when the system starts") is written as
  `~/.config/autostart/mpdm.desktop` instead of a registry Run key.
- **System proxy** ("Use the system / browser proxy") reads the `http_proxy` /
  `HTTPS_PROXY` environment variables (Windows reads the Internet Settings registry).
- **Incognito** still keeps downloads out of history, but the folder isn't marked
  hidden (there is no Windows hidden attribute on Linux).
- The browser "Save as…" dialog from the confirm popup is Windows-only (it uses the
  Windows file dialog); on Linux the download uses the chosen folder/name directly.
- `ffmpeg` must be on `PATH` for HLS/DASH stream grabbing.
