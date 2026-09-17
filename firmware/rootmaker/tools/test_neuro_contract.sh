#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build-host
cc -std=c11 -O1 -g -Wall -Wextra -fsanitize=undefined -DMAKE_LIB \
  -include host/sdkconfig.h -Ihost -Icomponents/ryz_runtime/include \
  -Imanaged_components/georgik__lua/include \
  -Imanaged_components/georgik__lua/lua tests/neuro_contract_test.c \
  components/ryz_runtime/nervous_runtime.c managed_components/georgik__lua/lua/onelua.c \
  -lm -o build-host/neuro-contract-test
build-host/neuro-contract-test
