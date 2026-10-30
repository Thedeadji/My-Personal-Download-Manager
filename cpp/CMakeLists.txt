cmake_minimum_required(VERSION 3.21)

# Release version; CI passes -DMPDM_VERSION=<tag without the leading "v">.
set(MPDM_VERSION "1.0.0" CACHE STRING "MPDM release version")
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" MPDM_VERSION_NUMERIC "${MPDM_VERSION}")
project(mpdm VERSION ${MPDM_VERSION_NUMERIC} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# curl: vcpkg ships a CMake config; distro packages (Debian/Ubuntu) often don't,
# so fall back to CMake's FindCURL module. Both provide CURL::libcurl.
find_package(CURL CONFIG QUIET)
if(NOT CURL_FOUND)
    find_package(CURL REQUIRED)
endif()
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(pugixml CONFIG REQUIRED)
# cpp-httplib: vcpkg ships a CMake config; Debian/Ubuntu ship only pkg-config;
# otherwise use the single header. All paths end in the httplib::httplib target.
find_package(httplib CONFIG QUIET)
if(NOT TARGET httplib::httplib)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(CPPHTTPLIB QUIET IMPORTED_TARGET cpp-httplib)
    endif()
    if(TARGET PkgConfig::CPPHTTPLIB)
        add_library(httplib::httplib ALIAS PkgConfig::CPPHTTPLIB)
    else()
        find_path(HTTPLIB_INCLUDE_DIR httplib.h REQUIRED)
        add_library(mpdm_httplib INTERFACE)
        target_include_directories(mpdm_httplib INTERFACE ${HTTPLIB_INCLUDE_DIR})
        add_library(httplib::httplib ALIAS mpdm_httplib)
    endif()
endif()

# Ported backend layers (grows per ../PORTING.md).
add_library(mpdm_core
    src/config.cpp
    src/scheduler.cpp
    src/ratelimit.cpp
    src/categories.cpp
    src/batch.cpp
    src/http_client.cpp
    src/storage.cpp
    src/engine.cpp
    src/manager.cpp
    src/aes128.cpp
    src/hls.cpp
    src/dash.cpp
    src/muxer.cpp
    src/grabber.cpp
    src/gdrive.cpp
    src/cli.cpp
    src/native_host.cpp
    src/web.cpp
)
target_include_directories(mpdm_core PUBLIC include)
target_link_libraries(mpdm_core PUBLIC
    CURL::libcurl
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    pugixml::pugixml
    httplib::httplib
)

add_executable(mpdm src/main.cpp)
target_link_libraries(mpdm PRIVATE mpdm_core)

add_executable(mpdm_host src/host_main.cpp)
target_link_libraries(mpdm_host PRIVATE mpdm_core)

add_executable(mpdm_tests tests/tests.cpp)
target_link_libraries(mpdm_tests PRIVATE mpdm_core)
add_test(NAME mpdm_tests COMMAND mpdm_tests)

# Desktop GUI (Qt6). Off by default so the core build needs no Qt; enable with
# -DMPDM_BUILD_GUI=ON once Qt6 is installed (sets CMAKE_PREFIX_PATH to the Qt dir).
option(MPDM_BUILD_GUI "Build the Qt6 desktop GUI" OFF)
if(MPDM_BUILD_GUI)
    add_subdirectory(gui)
endif()

# Desktop GUI (GTK4 + libadwaita via gtkmm) — the GNOME-native front-end. Builds
# independently of the Qt GUI; enable with -DMPDM_BUILD_GTK_GUI=ON.
option(MPDM_BUILD_GTK_GUI "Build the GTK4/libadwaita desktop GUI" OFF)
if(MPDM_BUILD_GTK_GUI)
    add_subdirectory(gui-gtk)
endif()

# ---- Linux packaging (install rules + CPack: .deb and .tar.gz) --------------
# Windows is packaged with Inno Setup (see packaging/mpdm.iss); these rules are
# Linux-only. Run packaging/linux/build-linux.sh, which builds then invokes cpack.
if(UNIX AND NOT APPLE)
    install(TARGETS mpdm mpdm_host RUNTIME DESTINATION bin)
    # The browser extension (load unpacked from /usr/share/mpdm/extension).
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/../extension/ DESTINATION share/mpdm/extension)

    # One .deb per desktop front-end. They install the same files (CLI, host,
    # extension), so they replace each other via the virtual "mpdm-desktop".
    if(MPDM_BUILD_GTK_GUI AND NOT MPDM_BUILD_GUI)
        set(CPACK_PACKAGE_NAME "mpdm-gtk")
    elseif(MPDM_BUILD_GUI AND NOT MPDM_BUILD_GTK_GUI)
        set(CPACK_PACKAGE_NAME "mpdm-qt")
    else()
        set(CPACK_PACKAGE_NAME "mpdm")
    endif()
    set(CPACK_PACKAGE_VENDOR "MPDM")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "MyPersonalDownloadManager — multi-connection download manager")
    set(CPACK_PACKAGE_VERSION "${MPDM_VERSION}")
    set(CPACK_PACKAGE_CONTACT "MPDM")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)  # <name>_<version>_<arch>.deb
    set(CPACK_DEBIAN_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_PROVIDES "mpdm-desktop")
    set(CPACK_DEBIAN_PACKAGE_CONFLICTS "mpdm-desktop")
    set(CPACK_DEBIAN_PACKAGE_REPLACES "mpdm-desktop")
    # Runtime deps: computed from the binaries where dpkg-shlibdeps exists (the
    # Debian/Ubuntu build hosts; library package names differ per release), else
    # a best-effort list.
    find_program(DPKG_SHLIBDEPS dpkg-shlibdeps)
    if(DPKG_SHLIBDEPS)
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    else()
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcurl4, libssl3")
        if(MPDM_BUILD_GUI)
            set(CPACK_DEBIAN_PACKAGE_DEPENDS
                "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libqt6widgets6, libqt6network6")
        endif()
        if(MPDM_BUILD_GTK_GUI)
            set(CPACK_DEBIAN_PACKAGE_DEPENDS
                "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libgtkmm-4.0-0, libadwaita-1-0")
        endif()
    endif()
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "ffmpeg")
    include(CPack)
endif()
