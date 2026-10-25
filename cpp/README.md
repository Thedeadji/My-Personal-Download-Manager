# MPDM — C++ port (in progress)

The native C++ build of MyPersonalDownloadManager. The Python version (in this
repo) is the reference implementation; this port is validated against its
behavior and test vectors. See `../PORTING.md` for the feature → library map.

> Status: **scaffold** — toolchain check only. The real port is built bottom-up.

## Toolchain (already present on this machine)

- Visual Studio 2022 Community with the **Desktop development with C++** workload
  (MSVC `cl.exe`, CMake, Ninja, Windows SDK)
- **vcpkg** bundled with VS at
  `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg`

## Build (scaffold — no dependencies yet)

The scaffold uses the **Visual Studio generator**, which locates MSVC on its own
— so it builds from any shell (no Developer prompt, no Ninja, no vcpkg needed
yet).

### CLI (any PowerShell / cmd)
```powershell
cd cpp
cmake --preset vs
cmake --build build --config Debug
.\build\Debug\mpdm.exe
```

### Visual Studio
File → Open → **Folder…** → select this `cpp/` folder → build the `mpdm` target.

### VS Code + CMake Tools
Pick the **vs** preset → Configure → Build.

Expected output:
```
MPDM (C++) scaffold build OK
app=MyPersonalDownloadManager  short=MPDM  lang=C++  version=0.1.0  status=scaffold
```

## Enabling vcpkg (later — when the HTTP layer lands)

The VS-bundled vcpkg ships without git history, so manifest mode (baselines)
doesn't work with it. Use a proper git clone of vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
setx VCPKG_ROOT C:\vcpkg          # reopen the terminal afterwards

cd cpp
& "$env:VCPKG_ROOT\vcpkg.exe" x-update-baseline --add-initial-baseline   # fills builtin-baseline in vcpkg.json
```

Then re-add the toolchain to the `vs` preset (or add a `vcpkg` preset):
`"CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"`,
list the needed packages in `vcpkg.json`, and `find_package(...)` them in
`CMakeLists.txt`.

### Dependencies (added as the port advances)

| Layer | vcpkg package |
|---|---|
| HTTP (probe/ranges/proxy) | `curl` |
| AES-128 | `openssl` (or Windows CNG, no dep) |
| DASH XML | `pugixml` |
| Web dashboard | `cpp-httplib` |
| CLI parsing | `cli11` |
| JSON (sidecar/settings) | `nlohmann-json` |

## Layout (planned)
```
cpp/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── src/
│   ├── main.cpp         # entry (scaffold)
│   └── mpdm/            # ported modules: models, http, engine, …
└── tests/               # validate against ../shared test vectors
```
