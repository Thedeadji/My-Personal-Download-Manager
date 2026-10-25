cmake_minimum_required(VERSION 3.21)
project(mpdm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# Dependency-free backend layers ported from Python (more land per ../PORTING.md:
# http via libcurl, AES via openssl, DASH via pugixml, web via cpp-httplib —
# enabled once a git-cloned vcpkg is set up; see README "Enabling vcpkg").
add_library(mpdm_core
    src/ratelimit.cpp
    src/categories.cpp
    src/batch.cpp
)
target_include_directories(mpdm_core PUBLIC include)

add_executable(mpdm src/main.cpp)
target_link_libraries(mpdm PRIVATE mpdm_core)

add_executable(mpdm_tests tests/tests.cpp)
target_link_libraries(mpdm_tests PRIVATE mpdm_core)
add_test(NAME mpdm_tests COMMAND mpdm_tests)
