#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build-host
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DMAKE_LIB -include host/sdkconfig.h -Ihost \
  -Icomponents/ryz_runtime/include \
  -Icomponents/ryz_lvgl/include \
  -Icomponents/ryz_tools/include -Icomponents/ryz_i2c_scan/include \
  -Icomponents/ryz_rgb/include -Icomponents/ryz_monitor/include \
  -Imanaged_components/georgik__lua/include -Imanaged_components/georgik__lua/lua \
  tests/app_runtime_test.c components/ryz_runtime/app_runtime.c components/ryz_runtime/app_tools.c \
  managed_components/georgik__lua/lua/onelua.c \
  -lm -o build-host/app-runtime-test
UBSAN_OPTIONS=halt_on_error=1 build-host/app-runtime-test
