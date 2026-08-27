#!/bin/bash
# get_data.sh — fetch the freeware SkyRoads game data (and the wavetable
# soundfont) into a directory.  Idempotent: only downloads what's missing.
# Used by make_mac.sh (and a future make_linux.sh), or run standalone:
#
#   ./get_data.sh [dir]     (default: ./data)
#
# The game is Bluemoon's freeware; this repo never redistributes it — the
# download happens on your machine, from their official site.
set -euo pipefail

DEST="${1:-$(dirname "$0")/data}"
mkdir -p "$DEST"

if ! find "$DEST" -maxdepth 1 -iname "roads.lzs" | grep -q .; then
    URL="http://www.bluemoon.ee/history/skyroads/skyroads.zip"
    echo "Downloading SkyRoads (freeware) from $URL ..."
    curl -fL --progress-bar -o "$DEST/skyroads.zip" "$URL"
    unzip -o -q "$DEST/skyroads.zip" -d "$DEST"
    rm "$DEST/skyroads.zip"
    find "$DEST" -maxdepth 1 -iname "roads.lzs" | grep -q . \
        || { echo "ERROR: download did not contain the expected game data" >&2; exit 1; }
fi

# TimGM6mb SoundFont (GPL, Tim Brechbill / MuseScore) for the wavetable
# music mode; the game falls back to AdLib FM without it.
if ! find "$DEST" -maxdepth 1 -iname "TimGM6mb.sf2" | grep -q .; then
    SF_URL="https://sourceforge.net/p/mscore/code/HEAD/tree/trunk/mscore/share/sound/TimGM6mb.sf2?format=raw"
    echo "Downloading TimGM6mb.sf2 (wavetable instruments, ~6 MB) ..."
    curl -fL --progress-bar -o "$DEST/TimGM6mb.sf2" "$SF_URL" \
        || echo "warning: soundfont fetch failed; music will use AdLib FM"
fi

echo "OK: game data in $DEST"
