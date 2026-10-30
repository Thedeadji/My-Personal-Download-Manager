# Packaging the MPDM desktop app (Windows)

> Releases are built by CI: pushing a `v*` tag runs
> `.github/workflows/release.yml`, which does the steps below on `windows-latest`
> (Qt 6.8, vcpkg, app-local MSVC runtime) and attaches `MPDM-Setup-<ver>.exe` to the
> GitHub Release. The manual steps are for local builds.

Produces a self-contained install of the Qt GUI (`MPDM.exe` + all Qt/vcpkg DLLs)
and a Windows installer (`MPDM-Setup-<ver>.exe`).

## Prerequisites
- Qt 6 **MSVC 2022 64-bit** kit (e.g. `C:\Qt\6.12.0\msvc2022_64`)
- vcpkg (`VCPKG_ROOT`), CMake, Visual Studio 2022 build tools
- Inno Setup 6 (`ISCC.exe`) — `winget install JRSoftware.InnoSetup`

## 1. Configure + build the GUI (Release)
```sh
cmake -S cpp -B cpp/build-gui -G "Visual Studio 17 2022" -A x64 \
  -DMPDM_BUILD_GUI=ON \
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64
cmake --build cpp/build-gui --config Release --target mpdm_gui
```

## 2. Assemble the standalone folder
```sh
DIST=cpp/build-gui/dist
rm -rf "$DIST" && mkdir -p "$DIST"
cp cpp/build-gui/gui/Release/mpdm_gui.exe "$DIST/MPDM.exe"
cp cpp/build-gui/Release/mpdm_host.exe "$DIST/"
cp cpp/build-gui/vcpkg_installed/x64-windows/bin/*.dll "$DIST/"
"C:/Qt/6.12.0/msvc2022_64/bin/windeployqt.exe" --release --no-translations --compiler-runtime "$DIST/MPDM.exe"
```
`dist/` is now self-contained (verified to run with Qt not on PATH).

## 3. Build the installer
```sh
"$LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe" \
  /DDistDir="<abs>\cpp\build-gui\dist" \
  /DOutDir="<abs>\cpp\build-gui" /DAppVersion=1.2.0 \
  cpp/packaging/mpdm.iss
```
Output: `cpp/build-gui/MPDM-Setup-1.0.0.exe` — installs to Program Files, adds a
Start-Menu entry (+ optional desktop shortcut) and an uninstaller.

## Portable alternative
Zip the `dist/` folder; it runs anywhere without installation.
