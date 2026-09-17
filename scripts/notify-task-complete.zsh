#!/bin/zsh
set -euo pipefail

/usr/bin/osascript -e 'display notification "可以回来查看结果了。" with title "Codex · Ryzobee 任务已完成"'
/usr/bin/afplay /System/Library/Sounds/Glass.aiff
