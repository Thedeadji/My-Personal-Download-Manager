cmake_minimum_required(VERSION 3.21)
project(mpdm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)

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

# ---- Linux packaging (install rules + CPack: .deb and .tar.gz) --------------
# Windows is packaged with Inno Setup (see packaging/mpdm.iss); these rules are
# Linux-only. Run packaging/linux/build-linux.sh, which builds then invokes cpack.
if(UNIX AND NOT APPLE)
    install(TARGETS mpdm mpdm_host RUNTIME DESTINATION bin)

    set(CPACK_PACKAGE_NAME "mpdm")
    set(CPACK_PACKAGE_VENDOR "MPDM")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "MyPersonalDownloadManager — multi-connection download manager")
    set(CPACK_PACKAGE_VERSION "1.0.0")
    set(CPACK_PACKAGE_CONTACT "MPDM")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_DEBIAN_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # System runtime deps for the .deb (GUI build pulls in Qt6).
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libcurl4, libssl3")
    if(MPDM_BUILD_GUI)
        set(CPACK_DEBIAN_PACKAGE_DEPENDS
            "${CPACK_DEBIAN_PACKAGE_DEPENDS}, libqt6widgets6, libqt6network6")
    endif()
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "ffmpeg")
    include(CPack)
endif()
