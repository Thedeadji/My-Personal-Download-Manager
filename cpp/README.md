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

## Build

### Option A — Visual Studio (simplest)
File → Open → **Folder…** → select this `cpp/` folder. VS detects
`CMakeLists.txt` + `vcpkg.json`, uses its bundled vcpkg automatically, configures,
and builds. Run the `mpdm` target.

### Option B — Developer PowerShell for VS 2022 (CLI)
```powershell
# one-time: point vcpkg at the VS-bundled copy (or set it as a user env var)
$env:VCPKG_ROOT = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg"

cd cpp
cmake --preset vcpkg      # first run also installs deps from vcpkg.json
cmake --build build
.\build\mpdm.exe
```

### Option C — VS Code + CMake Tools
Set `VCPKG_ROOT` (as above) so the `vcpkg` preset's toolchain resolves, then use
CMake Tools: select the **vcpkg** preset → Configure → Build.

Expected output:
```
MPDM (C++) scaffold build OK
{ "app": "MyPersonalDownloadManager", ... }
```

## Dependencies (grow as the port advances)

`vcpkg.json` currently pulls only `nlohmann-json` (header-only, fast first build).
As each layer lands we add:

| Layer | vcpkg package |
|---|---|
| HTTP (probe/ranges/proxy) | `curl` |
| AES-128 | `openssl` (or Windows CNG, no dep) |
| DASH XML | `pugixml` |
| Web dashboard | `cpp-httplib` |
| CLI parsing | `cli11` |

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
