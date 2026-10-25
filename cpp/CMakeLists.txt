cmake_minimum_required(VERSION 3.21)
project(mpdm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(mpdm
    src/main.cpp
)

# Dependencies (libcurl, openssl, nlohmann-json, pugixml, cpp-httplib) come via
# vcpkg and are enabled when the HTTP layer lands — see README "Enabling vcpkg".
# The scaffold builds with just MSVC + CMake so the toolchain can be verified now.
