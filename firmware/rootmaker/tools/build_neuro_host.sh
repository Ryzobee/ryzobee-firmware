#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build-host
cc -std=c11 -O2 -Wall -Wextra -DMAKE_LIB -include host/sdkconfig.h -Ihost \
  -Icomponents/ryz_runtime/include \
  -Imanaged_components/georgik__lua/include -Imanaged_components/georgik__lua/lua \
  host/neuro_host.c components/ryz_runtime/nervous_runtime.c \
  managed_components/georgik__lua/lua/onelua.c \
  -lm -o build-host/neuro-host
