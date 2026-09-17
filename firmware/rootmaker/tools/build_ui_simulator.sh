#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
firmware_dir=$(pwd)
simulator_build="$firmware_dir/build-host/simulator"
idf_build=${1:-${RYZ_IDF_BUILD:-}}
if [ "$#" -gt 1 ]; then
    echo "Usage: sh tools/build_ui_simulator.sh [current-idf-build-directory]" >&2
    exit 2
fi
if [ -n "$idf_build" ]; then
    cmake -S host/pixels -B "$simulator_build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug -DRYZ_BUILD_SDL_SIMULATOR=ON -DRYZ_IDF_BUILD="$idf_build"
else
    # Reuse only the already selected build, never guess a target configuration.
    if [ ! -f "$simulator_build/CMakeCache.txt" ]; then
        echo "First build needs an ESP-IDF build directory containing config/sdkconfig.h." >&2
        exit 2
    fi
    cmake -S host/pixels -B "$simulator_build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug -DRYZ_BUILD_SDL_SIMULATOR=ON
fi
cmake --build "$simulator_build" --parallel 8
ctest --test-dir "$simulator_build" --output-on-failure
echo "Built and checked: $simulator_build/v5_simulator.app"
echo "macOS: open the app bundle above. Linux: run $simulator_build/v5_simulator"
echo "Quit any old simulator window before reopening; rebuilding does not hot-reload a running process."
echo "M / I / T or APPS: real factory Monitor / I2C / DeviceTest Lua; SIMULATED, no device access."
echo "Esc or hold B for 5 seconds in a factory app: return Home."
