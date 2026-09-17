#!/bin/sh
# Run from the repository root. Read-only; no dependency downloads or edits.
set -eu
test -f firmware/rootmaker/dependencies.lock
# Approved V5 image masks, ESP_TIMER/U64 CPU runtime accounting and UTF-8
# decoding for Teko degree and default-disabled optional FreeType are part of
# the current config baseline. Lua source,
# its ASCII text policy, partitions and manifests stay frozen.
printf '%s\n' \
  '7abc385d2827f60923ccdfca4635d5a07fb38d912c2709d475a2097fdc781666  firmware/rootmaker/dependencies.lock' \
  'f8cee631c082dd080c38b6e8a6c139fe4cd4bb857960f045ef4732a679511d0b  firmware/rootmaker/components/ryz_runtime/idf_component.yml' \
  '2142af350732163d0115c931952c704e1bf612de18ff0d2c9598312b4e806ae3  firmware/rootmaker/components/ryz_lvgl/idf_component.yml' \
  '0aec5006f1fde09e543f0c8ad7422d18906afaee0669b7900e34aaccdb744282  firmware/rootmaker/sdkconfig.defaults' \
  'ee2dbe707ca251c2c1fa1de6fee8a101218f39ffe81967edefb606a3089e2d09  firmware/rootmaker/partitions.csv' | shasum -a 256 -c -
lua_source_digest=$(
  cd firmware/rootmaker/managed_components/georgik__lua
  rg --files . | LC_ALL=C sort | xargs shasum -a 256 | shasum -a 256
)
case "$lua_source_digest" in
  607f4f94ed22e38f745d107837832dff52d98f90f838255863c74930594f089c*) printf '%s\n' 'LUA_COMPONENT_SOURCE_UNCHANGED' ;;
  *) printf '%s\n' 'Lua component source differs from the pre-Workbench baseline' >&2; exit 1 ;;
esac
