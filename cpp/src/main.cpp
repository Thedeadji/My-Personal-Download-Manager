// MyPersonalDownloadManager — C++ port (scaffold).
//
// This is a placeholder entry point to verify the toolchain (MSVC + CMake +
// Ninja + vcpkg) builds end-to-end. Real code is ported bottom-up per
// ../PORTING.md: models -> http (libcurl) -> engine -> resume -> manager -> CLI
// -> HLS/DASH/AES -> native host -> web -> GUI.

#include <iostream>
#include <nlohmann/json.hpp>

int main() {
    // Use nlohmann/json so a successful build also proves the vcpkg dependency
    // pipeline works, not just the compiler.
    nlohmann::json info = {
        {"app", "MyPersonalDownloadManager"},
        {"short", "MPDM"},
        {"language", "C++"},
        {"version", "0.1.0"},
        {"status", "scaffold"}
    };
    std::cout << "MPDM (C++) scaffold build OK\n";
    std::cout << info.dump(2) << "\n";
    return 0;
}
