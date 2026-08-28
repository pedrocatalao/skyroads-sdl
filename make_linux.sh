#!/bin/bash
# make_linux.sh — build SkyRoads on Linux.  Fetches the freeware game data
# via get_data.sh if it isn't present yet, then builds the `skyroads` binary.
#
# Requirements:  cmake, a C compiler, SDL2 dev headers, curl, unzip
#   Debian/Ubuntu:  sudo apt install build-essential cmake libsdl2-dev curl unzip
#   Fedora:         sudo dnf install gcc cmake SDL2-devel curl unzip
#   Arch:           sudo pacman -S base-devel cmake sdl2 curl unzip
#
# Usage: ./make_linux.sh [data-dir]     (default: ./data, fetched if missing)
set -euo pipefail

DATA_ARG="${1:-$(dirname "$0")/data}"

if ! find "$DATA_ARG" -maxdepth 1 -iname "roads.lzs" 2>/dev/null | grep -q . \
        && [ "$#" -ge 1 ]; then
    echo "ERROR: no SkyRoads data in '$DATA_ARG'" >&2
    echo "Point make_linux.sh at your game data, or run it with no argument" >&2
    echo "to download the freeware release into ./data automatically." >&2
    exit 1
fi
"$(dirname "$0")/get_data.sh" "$DATA_ARG"
DATA_DIR="$(cd "$DATA_ARG" && pwd)"

cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target skyroads -j"$(nproc 2>/dev/null || echo 4)"

echo
echo "built: build/skyroads"
echo "run:   ./build/skyroads \"$DATA_DIR\""
