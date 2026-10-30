#!/usr/bin/env bash
# Build MPDM on Linux and produce installers (.deb + .tar.gz) via CPack.
#
# Requirements (install via your distro or vcpkg):
#   - CMake >= 3.21, a C++17 compiler (g++/clang++), Ninja or Make
#   - Qt6 (Widgets + Network), libcurl, OpenSSL, nlohmann-json, pugixml, cpp-httplib
#   - ffmpeg at runtime (for HLS/DASH grabbing)
#
# Debian/Ubuntu deps, for example:
#   sudo apt install cmake ninja-build g++ pkg-config qt6-base-dev libgl1-mesa-dev \
#        libcurl4-openssl-dev nlohmann-json3-dev libpugixml-dev libssl-dev \
#        libcpp-httplib-dev ffmpeg zenity
#
# Usage:
#   ./build-linux.sh                         # system libraries
#   ./build-linux.sh -DCMAKE_TOOLCHAIN_FILE=/path/vcpkg/scripts/buildsystems/vcpkg.cmake
#
set -euo pipefail

# cd to the cpp/ project root (two levels up from packaging/linux/).
cd "$(dirname "$0")/../.."

BUILD="${MPDM_BUILD_DIR:-build-linux}"

cmake -S . -B "$BUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      -DMPDM_BUILD_GUI=ON \
      "$@"

cmake --build "$BUILD" -j"$(nproc)"

# Run the backend tests (non-fatal — packaging continues even if a test is skipped).
ctest --test-dir "$BUILD" --output-on-failure || true

# Produce the installers.
( cd "$BUILD" && cpack )

echo
echo "Installers:"
ls -1 "$BUILD"/*.deb "$BUILD"/*.tar.gz 2>/dev/null || echo "  (none produced — check the CPack output above)"
echo
echo "Install the .deb with:  sudo apt install $(realpath "$BUILD")/mpdm-1.0.0-*.deb"
echo "Browser extension (load unpacked):  /usr/share/mpdm/extension"
echo "Register the browser host after install:  mpdm_host --register --extension-id <ID>"
