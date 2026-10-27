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
