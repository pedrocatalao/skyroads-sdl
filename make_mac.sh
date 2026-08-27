#!/bin/bash
# make_mac.sh — build SkyRoads.app (macOS).  If the game data isn't present
# yet, get_data.sh fetches it first.
#
# Usage: ./make_mac.sh [data-dir]     (default: ./data, fetched if missing)
set -euo pipefail

DATA_ARG="${1:-$(dirname "$0")/data}"

# ---- fetch the game data if it isn't there yet ----
have_data() { find "$1" -maxdepth 1 -iname "roads.lzs" 2>/dev/null | grep -q .; }

if ! have_data "$DATA_ARG" && [ "$#" -ge 1 ]; then
    echo "ERROR: no SkyRoads data in '$DATA_ARG'" >&2
    echo "Point make_mac.sh at your game data, or run it with no argument" >&2
    echo "to download the freeware release into ./data automatically." >&2
    exit 1
fi
"$(dirname "$0")/get_data.sh" "$DATA_ARG"

DATA_DIR="$(cd "$DATA_ARG" && pwd)"

cd "$(dirname "$0")"
OUT="build/SkyRoads.app"

# ---- official universal SDL2.framework (arm64 + x86_64), cached ----
SDL2_VER="2.32.10"
FW="build/vendor/SDL2.framework"
if [ ! -d "$FW" ]; then
    echo "Downloading SDL2 $SDL2_VER framework (universal, ~2 MB) ..."
    mkdir -p build/vendor
    curl -fL --progress-bar -o build/vendor/SDL2.dmg \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VER/SDL2-$SDL2_VER.dmg"
    MNT=$(mktemp -d)
    hdiutil attach build/vendor/SDL2.dmg -nobrowse -quiet -mountpoint "$MNT"
    cp -R "$MNT/SDL2.framework" build/vendor/
    hdiutil detach "$MNT" -quiet
    rm build/vendor/SDL2.dmg
fi

# universal (arm64 + x86_64) release build against the framework
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
      -DSKY_SDL2_FRAMEWORK="$PWD/$FW" \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" >/dev/null
cmake --build build/release --target skyroads -j >/dev/null

rm -rf "$OUT"
mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Resources"

cat > "$OUT/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>      <string>skyroads</string>
    <key>CFBundleIdentifier</key>      <string>local.skyroads-native-port</string>
    <key>CFBundleName</key>            <string>SkyRoads</string>
    <key>CFBundleDisplayName</key>     <string>SkyRoads</string>
    <key>CFBundlePackageType</key>     <string>APPL</string>
    <key>CFBundleShortVersionString</key> <string>1.0</string>
    <key>CFBundleIconFile</key>        <string>SkyRoads</string>
    <key>NSHighResolutionCapable</key> <true/>
</dict>
</plist>
PLIST

cp build/release/skyroads "$OUT/Contents/MacOS/skyroads"

# embed SDL2.framework (headers stripped) so the app is fully self-contained
mkdir -p "$OUT/Contents/Frameworks"
cp -R "$FW" "$OUT/Contents/Frameworks/"
rm -rf "$OUT/Contents/Frameworks/SDL2.framework/Headers" \
       "$OUT/Contents/Frameworks/SDL2.framework/Versions/A/Headers"

# Game data. Filenames in the freeware distribution may be UPPERCASE
# (DOS-style); copy case-insensitively and store as lowercase, which is
# what the engine opens.
copy_data() {  # $1 = filename (lowercase), $2 = "required" | "optional"
    local src
    src=$(find "$DATA_DIR" -maxdepth 1 -iname "$1" | head -1)
    if [ -z "$src" ]; then
        if [ "$2" = required ]; then
            echo "ERROR: required data file '$1' not found in $DATA_DIR" >&2
            echo "Point make_app.sh at your SkyRoads game data directory." >&2
            exit 1
        fi
        echo "note: optional '$1' not found, skipping"
        return
    fi
    cp "$src" "$OUT/Contents/Resources/$1"
}

for f in trekdat.lzs roads.lzs muzax.lzs cars.lzs dashbrd.lzs \
         mainmenu.lzs gomenu.lzs setmenu.lzs helpmenu.lzs intro.lzs \
         sfx.snd speed.dat oxy_disp.dat ful_disp.dat \
         world0.lzs world1.lzs world2.lzs world3.lzs world4.lzs \
         world5.lzs world6.lzs world7.lzs world8.lzs world9.lzs; do
    copy_data "$f" required
done
# not used by the port yet (demo mode)
for f in anim.lzs intro.snd demo.rec; do
    copy_data "$f" optional
done
# wavetable soundfont
copy_data "TimGM6mb.sf2" optional

# app icon
if [ -f icon.png ]; then
    python3 make_icon.py icon.png "$OUT/Contents/Resources/SkyRoads.icns" \
        && echo "icon: from icon.png" || echo "note: icon generation failed, skipping"
fi

codesign --force -s - "$OUT/Contents/Frameworks/SDL2.framework"
codesign --force -s - "$OUT"
# nudge Finder/LaunchServices so the fresh bundle's icon shows immediately
touch "$OUT"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$OUT" 2>/dev/null || true
echo "built: $OUT"
