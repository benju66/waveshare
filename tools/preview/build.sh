#!/bin/sh
# Build and run the host preview, writing out/frame.png.
#
# render.c is copied in next to the stub headers on purpose: a quoted #include
# resolves against the directory of the file doing the including, so this is
# what makes render.c pick up our display.h and esp_log.h instead of the
# ESP-IDF ones. Nothing in fluidbox/ is modified.
#
# Usage: ./build.sh [particles]

set -e

here=$(cd "$(dirname "$0")" && pwd)
main="$here/../../fluidbox/main"
out="$here/out"

mkdir -p "$out"
cp "$main/render.c" "$here/render_host.c"
trap 'rm -f "$here/render_host.c"' EXIT

cc -O1 -std=c11 -Wall -I "$here" -I "$main" \
    "$here/preview.c" "$here/render_host.c" -lm -o "$out/preview"

"$out/preview" "${1:-0}" > "$out/frame.ppm"

if command -v sips >/dev/null 2>&1; then
    sips -s format png "$out/frame.ppm" --out "$out/frame.png" >/dev/null
    echo "$out/frame.png"
else
    echo "$out/frame.ppm"
fi
