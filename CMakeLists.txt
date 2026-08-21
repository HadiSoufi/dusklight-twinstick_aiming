cmake_minimum_required(VERSION 3.26)
project(twinstick_aiming CXX)

if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "95608438c107fc321b82c5878f70a811e7c5913c")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

add_mod(twinstick_aiming
        FEATURES game
        SOURCES src/mod.cpp
        MOD_JSON mod.json
)
