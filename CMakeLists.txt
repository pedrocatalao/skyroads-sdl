cmake_minimum_required(VERSION 3.20)
project(skyroads C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -O2")

# SKY_CORE=ON builds GAME_SRC as a static library for DOS ex Machina instead
# of the standalone executable.  No main.c, no platform.c, no SDL.
option(SKY_CORE "Build the DXM core static library instead of the game" OFF)

# SKY_SDL2_FRAMEWORK=<path to SDL2.framework>: link the official (universal)
# SDL2.framework and reference it at @executable_path/../Frameworks — used by
# make_app.sh for the self-contained release bundle.  Default: Homebrew SDL2,
# best for development builds.
set(SKY_SDL2_FRAMEWORK "" CACHE PATH "Path to SDL2.framework for bundled release builds")

# The core links no SDL at all (PORTING.md 3.5), so a core build must not
# REQUIRE it to configure either - demanding it here is what broke the CI
# check on a runner that has the framework but no SDL2 cmake config.
if(SKY_CORE)
    # nothing to find
elseif(SKY_SDL2_FRAMEWORK)
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

# src/dxm_core.h is VENDORED here, because the core has to build without DXM
# checked out anywhere - this repo is public and DXM is not, so CI cannot go
# and fetch it.  DXM's own build passes SKY_DXM_INCLUDE and its copy is put
# FIRST, so the authoritative header always wins when both are present; the
# vendored one is what everything else compiles against.  dxm_core.h carries
# DXM_ABI and the shell refuses a core whose value it does not recognise, so
# the two copies drifting is a refusal at startup, not a mystery later.
set(SKY_DXM_INCLUDE "" CACHE PATH "DXM's src/, if building against a checkout")

# CORE_DXM_SRC — the game, plus the two files that make it a DXM core.
# src/dxm_platform.c is the shared adapter, vendored here for the same reason
# dxm_core.h is: a module has to build with no DXM checked out.  It belongs
# INSIDE the core rather than in the shell, because a loadable module cannot
# reach back into the host executable for the fourteen plat_* symbols it
# needs - that back-linking is fragile on Unix and impossible on Windows.
set(CORE_DXM_SRC ${GAME_SRC} src/dxm_entry.c src/dxm_platform.c)

if(SKY_CORE)
    add_library(skyroads_core STATIC ${CORE_DXM_SRC})
    target_include_directories(skyroads_core PUBLIC "${SKY_DXM_INCLUDE}" src)
    target_compile_definitions(skyroads_core PUBLIC SKY_CORE=1)
    target_link_libraries(skyroads_core PRIVATE m)

    # SKY_MODULE=ON also builds the loadable module DXM downloads and dlopens.
    # One extension on every platform, because the catalogue names the file.
    if(SKY_MODULE)
        add_library(skyroads_module MODULE ${CORE_DXM_SRC})
        set_target_properties(skyroads_module PROPERTIES
            OUTPUT_NAME "skyroads" PREFIX "" SUFFIX ".dxm"
            C_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)
        target_include_directories(skyroads_module PUBLIC "${SKY_DXM_INCLUDE}" src)
        target_compile_definitions(skyroads_module PUBLIC SKY_CORE=1)
        target_link_libraries(skyroads_module PRIVATE m)
        # Fail the BUILD on an unresolved symbol rather than the dlopen: a
        # module that quietly expects the host to supply something is exactly
        # the failure this design exists to prevent.
        if(APPLE)
            # erroring on an undefined symbol is already the default here,
            # and asking for it explicitly is deprecated
        elseif(WIN32)
            target_link_options(skyroads_module PRIVATE -Wl,--exclude-all-symbols)
        else()
            target_link_options(skyroads_module PRIVATE -Wl,--no-undefined)
        endif()
    endif()
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
