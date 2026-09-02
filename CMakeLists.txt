cmake_minimum_required(VERSION 3.20)
project(skyroads C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -O2")

# SKY_SDL2_FRAMEWORK=<path to SDL2.framework>: link the official (universal)
# SDL2.framework and reference it at @executable_path/../Frameworks — used by
# make_app.sh for the self-contained release bundle.  Default: Homebrew SDL2,
# best for development builds.
set(SKY_SDL2_FRAMEWORK "" CACHE PATH "Path to SDL2.framework for bundled release builds")

if(SKY_SDL2_FRAMEWORK)
    add_library(sky_sdl2 INTERFACE)
    target_include_directories(sky_sdl2 INTERFACE "${SKY_SDL2_FRAMEWORK}/Headers")
    get_filename_component(_sdl2_fw_dir "${SKY_SDL2_FRAMEWORK}" DIRECTORY)
    target_compile_options(sky_sdl2 INTERFACE "-F${_sdl2_fw_dir}")
    target_link_options(sky_sdl2 INTERFACE
        "-F${_sdl2_fw_dir}" "-framework" "SDL2"
        "-Wl,-rpath,@executable_path/../Frameworks")
    add_library(SDL2::SDL2 ALIAS sky_sdl2)
else()
    find_package(SDL2 REQUIRED CONFIG)
endif()

# GAME_SRC — the portable game logic.  Dependency-free: no SDL, no windowing,
# no audio device (PORTING.md §3.5).  Shared verbatim by the standalone build
# and the DXM core; only the platform.h implementation differs.
set(GAME_SRC
    src/compat.c src/assets.c src/menus.c src/audio.c src/opl3.c
    src/render.c src/game_play.c src/sky_run.c
)

set(CORE_SRC
    src/compat.c
    src/platform.c
)

# SKY_CORE=ON builds GAME_SRC as a static library for DOS ex Machina instead
# of the standalone executable.  No main.c, no platform.c, no SDL.
option(SKY_CORE "Build the DXM core static library instead of the game" OFF)
# src/dxm_core.h is VENDORED here, because the core has to build without DXM
# checked out anywhere - this repo is public and DXM is not, so CI cannot go
# and fetch it.  DXM's own build passes SKY_DXM_INCLUDE and its copy is put
# FIRST, so the authoritative header always wins when both are present; the
# vendored one is what everything else compiles against.  dxm_core.h carries
# DXM_ABI and the shell refuses a core whose value it does not recognise, so
# the two copies drifting is a refusal at startup, not a mystery later.
set(SKY_DXM_INCLUDE "" CACHE PATH "DXM's src/, if building against a checkout")

if(SKY_CORE)
    add_library(skyroads_core STATIC ${GAME_SRC} src/dxm_entry.c)
    target_include_directories(skyroads_core PUBLIC "${SKY_DXM_INCLUDE}" src)
    target_compile_definitions(skyroads_core PUBLIC SKY_CORE=1)
    target_link_libraries(skyroads_core PRIVATE m)
    return()
endif()

add_executable(asset_test ${CORE_SRC} src/main_test.c)
target_include_directories(asset_test PRIVATE src)
target_link_libraries(asset_test PRIVATE SDL2::SDL2)

add_executable(skyroads ${GAME_SRC} src/platform.c src/main.c)
target_include_directories(skyroads PRIVATE src)
target_link_libraries(skyroads PRIVATE SDL2::SDL2 m)
if(UNIX AND NOT APPLE)
    # Look for a bundled SDL2 in lib/ next to the binary first (the release
    # tarball ships one); fall back to the system SDL2 otherwise.
    target_link_options(skyroads PRIVATE "-Wl,-rpath,\$ORIGIN/lib")
endif()

add_executable(trek_test src/compat.c src/assets_headless.c src/render.c src/trek_test.c)
target_include_directories(trek_test PRIVATE src)

add_executable(game_sim src/compat.c src/render.c src/game_play.c src/game_sim.c)
target_include_directories(game_sim PRIVATE src)

add_executable(export_art src/compat.c src/assets_headless.c tools/export_art.c)
target_include_directories(export_art PRIVATE src)
target_link_libraries(export_art PRIVATE z)
add_executable(songs_dump src/compat.c src/assets_headless.c tools/songs_dump.c)
target_include_directories(songs_dump PRIVATE src)
