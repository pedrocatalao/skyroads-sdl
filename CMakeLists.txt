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

set(CORE_SRC
    src/compat.c
    src/platform.c
)

add_executable(asset_test ${CORE_SRC} src/main_test.c)
target_include_directories(asset_test PRIVATE src)
target_link_libraries(asset_test PRIVATE SDL2::SDL2)

add_executable(skyroads ${CORE_SRC} src/assets.c src/menus.c src/audio.c src/opl3.c src/render.c src/game_play.c src/main.c)
target_include_directories(skyroads PRIVATE src)
target_link_libraries(skyroads PRIVATE SDL2::SDL2 m)

add_executable(trek_test src/compat.c src/assets_headless.c src/render.c src/trek_test.c)
target_include_directories(trek_test PRIVATE src)

add_executable(game_sim src/compat.c src/render.c src/game_play.c src/game_sim.c)
target_include_directories(game_sim PRIVATE src)

add_executable(export_art src/compat.c src/assets_headless.c tools/export_art.c)
target_include_directories(export_art PRIVATE src)
target_link_libraries(export_art PRIVATE z)
add_executable(songs_dump src/compat.c src/assets_headless.c tools/songs_dump.c)
target_include_directories(songs_dump PRIVATE src)
