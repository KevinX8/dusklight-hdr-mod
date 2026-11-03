cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type")
endif ()

project(hdr_mod VERSION 1.0.0 LANGUAGES CXX)

# Needs the HDR engine changes (GfxService 1.5 HDR frame, side buffers). Until they are upstream,
# build against the fork; -DDUSKLIGHT_DIR=/path/to/dusklight uses a local checkout instead.
set(DUSKLIGHT_REPOSITORY "https://github.com/KevinX8/dusklight.git"
        CACHE STRING "Dusklight git repository to fetch from")
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "f88ce94047f40d915d3abb248971801f2bac2c38")  # KevinX8/dusklight hdr-fork
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

configure_file(mod.json.in "${CMAKE_CURRENT_BINARY_DIR}/mod.json" @ONLY NEWLINE_STYLE LF)

add_mod(hdr_mod
        FEATURES webgpu fmt      # GfxService 1.5 + fmt logging; no game-code access needed
        SOURCES src/mod.cpp src/settings.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
        RES_DIR res              # res/hdr.wgsl
)
