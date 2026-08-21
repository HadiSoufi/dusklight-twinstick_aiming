cmake_minimum_required(VERSION 3.26)
project(modern_aiming CXX)

if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "95608438c107fc321b82c5878f70a811e7c5913c")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

add_mod(modern_aiming
        FEATURES game
        SOURCES src/mod.cpp
        MOD_JSON mod.json
)
target_include_directories(modern_aiming PRIVATE include)

# Development-only harness driven by test/run_tests.ps1. Off by default so a
# normal build never ships it alongside the mod.
option(MODERN_AIMING_BUILD_TESTS "Build the unattended aiming test harness" OFF)

if (MODERN_AIMING_BUILD_TESTS)
    add_mod(aim_test
            FEATURES game
            SOURCES test/aim_test.cpp
            MOD_JSON test/mod.json
    )
    target_include_directories(aim_test PRIVATE include)
endif ()
