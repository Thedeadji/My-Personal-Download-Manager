cmake_minimum_required(VERSION 3.21)
project(mpdm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies come from vcpkg (manifest mode: see vcpkg.json). More are added
# as the port progresses (libcurl, openssl, pugixml, cpp-httplib) — see
# ../PORTING.md for the feature -> library map.
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(mpdm
    src/main.cpp
)
target_link_libraries(mpdm PRIVATE nlohmann_json::nlohmann_json)
