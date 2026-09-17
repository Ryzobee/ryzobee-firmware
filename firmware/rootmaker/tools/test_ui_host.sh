#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

sh tools/build_app_host.sh
result=$(mktemp "${TMPDIR:-/tmp}/ryzobee-ui-host.XXXXXX")
text_bounds_result=$(mktemp "${TMPDIR:-/tmp}/ryzobee-ui-host-text-bounds.XXXXXX")
trap 'rm -f "$result" "$text_bounds_result"' EXIT HUP INT TERM

build-host/app-host scripts/ui_demo.lua >"$result" <<'EOF'
pointer 80 down 40 100
pointer 120 up
end 300
EOF

grep -Fq '"screen.text.0":"RYZOBEE"' "$result"
grep -Fq '"mark.action":"apps"' "$result"
grep -Fq '"summary":true,"ok":true' "$result"

build-host/app-host tests/ui_host_text_bounds.lua >"$text_bounds_result" <<'EOF'
end 1
EOF

grep -Fq '"op":"text","x":11,"y":10,"text":"TO","color":64320,"scale":1' "$text_bounds_result"
grep -Fq '"op":"text","x":30,"y":30,"text":"HI","color":65535,"scale":1' "$text_bounds_result"
if grep -Fq '"text":"HIDE"' "$text_bounds_result"; then
  printf '%s\n' 'Host rendered text outside its object height' >&2
  exit 1
fi
printf '%s\n' 'UI_HOST_PASS: managed scene rendered, text bounded and APPS activated'
