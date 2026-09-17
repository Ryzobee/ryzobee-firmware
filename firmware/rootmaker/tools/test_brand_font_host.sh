#!/bin/sh
set -eu
firmware_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    printf '%s\n' 'Usage: sh tools/test_brand_font_host.sh TARGET_IDF_BUILD [HOST_OUTPUT_DIR]' >&2
    exit 2
fi
target_build=$(CDPATH= cd -- "$1" && pwd)
if [ ! -f "$target_build/config/sdkconfig.h" ]; then
    printf '%s\n' 'Target build must contain generated config/sdkconfig.h' >&2
    exit 2
fi
if [ "$#" -eq 2 ]; then
    host_output=$2
else
    host_output=$(mktemp -d "${TMPDIR:-/tmp}/ryzobee-brand-font.XXXXXX")
fi
cmake -S "$firmware_root/host/pixels" -B "$host_output" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DRYZ_IDF_BUILD="$target_build"
cmake --build "$host_output" --parallel 8
ctest --test-dir "$host_output" --output-on-failure
# Optional viewing convenience on macOS; the test's authoritative pixel file
# is little-endian RGB565. Conversion never feeds back into the renderer.
if command -v sips >/dev/null 2>&1; then
    for fixture in brand-font-240 lua-update-before-240 lua-update-after-240 \
        lua-script-initial-240 lua-script-update-1-240 lua-script-update-2-240 \
        lua-script-update-3-240; do
        sips -s format png "$host_output/$fixture.bmp" \
            --out "$host_output/$fixture.png" >/dev/null
    done
fi
printf 'BRAND_FONT_HOST_ARTIFACTS: %s\n' "$host_output"
