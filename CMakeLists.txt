cmake_minimum_required(VERSION 3.26)
project(twinstick_aiming CXX)

if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "e916b61ca00c391a1dd73ce4f80ed87ae3a12227")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

add_mod(twinstick_aiming
        FEATURES game
        SOURCES src/mod.cpp
        MOD_JSON mod.json
)
