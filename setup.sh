#!/bin/sh
# One-time setup for render.py (needs network for the git fetches, about 230 MB).
#
#   sh setup.sh
#
# 1. PebbleOS at the tag the SDK's emery emulator runs (v4.33.2 for SDK 4.33.1),
#    plus the two submodules the graphics code needs (FreeRTOS headers, TinyMT for rand()).
# 2. hostgfx/setup_fw.py: system resources, builtin fallback font, font table and
#    resource ids extracted from the SDK's QEMU images, SDK->firmware name map.
# Everything stays inside this directory (git-ignored); nothing is installed system-wide.
# The firmware data comes from your own Pebble SDK and is never redistributed.
set -eu
R=$(cd "$(dirname "$0")" && pwd)
TAG=${PEBBLEOS_TAG:-v4.33.2}
mkdir -p "$R/src"

if [ ! -d "$R/src/pebbleos/.git" ]; then
  git clone --depth 1 --branch "$TAG" https://github.com/coredevices/pebbleos.git "$R/src/pebbleos"
fi

# Host-only fixes: C undefined behaviour that ARM and the host resolve differently
# (e.g. a 32-bit shift in the glyph renderer). Each patch makes the host match ARM.
for patch in "$R"/hostgfx/patches/*.patch; do
  if git -C "$R/src/pebbleos" apply --check "$patch" 2>/dev/null; then
    git -C "$R/src/pebbleos" apply "$patch"
  fi
done

fetch_pinned() {  # dir url submodule-path
  if [ -d "$1/.git" ]; then return 0; fi
  sha=$(git -C "$R/src/pebbleos" ls-tree HEAD "$3" | awk '{print $3}')
  mkdir -p "$1"
  git -C "$1" init -q
  git -C "$1" remote add origin "$2"
  git -C "$1" fetch -q --depth 1 origin "$sha"
  git -C "$1" checkout -q FETCH_HEAD
}
fetch_pinned "$R/src/FreeRTOS-Kernel" https://github.com/coredevices/FreeRTOS-Kernel \
  third_party/freertos/FreeRTOS-Kernel
fetch_pinned "$R/src/TinyMT" https://github.com/MersenneTwister-Lab/TinyMT \
  third_party/tinymt/TinyMT

python3 "$R/hostgfx/setup_fw.py"
echo "ready: python3 render.py PROJECT_DIR OUT.png"
