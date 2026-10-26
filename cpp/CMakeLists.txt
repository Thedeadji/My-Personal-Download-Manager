cmake_minimum_required(VERSION 3.21)
project(mpdm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Ported backend layers (grows per ../PORTING.md).
add_library(mpdm_core
    src/ratelimit.cpp
    src/categories.cpp
    src/batch.cpp
    src/http_client.cpp
    src/storage.cpp
    src/engine.cpp
    src/manager.cpp
)
target_include_directories(mpdm_core PUBLIC include)
target_link_libraries(mpdm_core PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)

add_executable(mpdm src/main.cpp)
target_link_libraries(mpdm PRIVATE mpdm_core)

add_executable(mpdm_tests tests/tests.cpp)
target_link_libraries(mpdm_tests PRIVATE mpdm_core)
add_test(NAME mpdm_tests COMMAND mpdm_tests)
