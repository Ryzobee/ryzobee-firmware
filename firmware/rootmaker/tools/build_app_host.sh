#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build-host
app_core_sha=$(sh tools/app_core_sha.sh)
cc -std=c11 -O2 -Wall -Wextra -Werror -DMAKE_LIB -include host/sdkconfig.h -Ihost \
  -Icomponents/ryz_runtime/include \
  -Icomponents/ryz_lvgl/include \
  -Icomponents/ryz_tools/include -Icomponents/ryz_i2c_scan/include \
  -Icomponents/ryz_rgb/include -Icomponents/ryz_monitor/include \
  -DRYZ_APP_CORE_SHA="\"${app_core_sha}\"" \
  -Imanaged_components/georgik__lua/include -Imanaged_components/georgik__lua/lua \
  host/app_host.c components/ryz_runtime/app_runtime.c components/ryz_runtime/app_tools.c \
  managed_components/georgik__lua/lua/onelua.c \
  -lm -o build-host/app-host
