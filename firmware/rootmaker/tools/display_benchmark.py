#!/usr/bin/env python3
"""Measure real Lua drawing/flush latency; no flash, reset or script-file writes.

Requires an idle device. The display is changed during measurement; optical
tearing and physical panel response are not measured by these software timings.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import sys

from board_lua import Board, expect_ok, require


CASES = {
    "full_frame": "d.clear(0x0010 + i % 2)",
    "small_rect": "d.rect(80, 100, 16, 16, 0x07e0 + i % 2)",
    "text_region": "d.rect(20, 30, 100, 16, 0); d.text(20, 30, i % 2 == 0 and 'ON' or 'OFF', 0xffff, 2)",
    "unchanged": "-- no drawing since the previous show",
}


def source_for(name, samples):
    return """local b = require('ryzobee')
local d = require('display')
d.clear(0)
d.show()
for i = 1, %d do
  local begin = b.millis()
  %s
  local drawn = b.millis()
  assert(d.show())
  local shown = b.millis()
  print('DISPLAY_BENCH', '%s', drawn - begin, shown - drawn)
end
""" % (samples, CASES[name], name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--samples', type=int, default=12)
    parser.add_argument('--max-full-ms', type=float, default=40)
    parser.add_argument('--max-small-ms', type=float, default=10)
    parser.add_argument('--max-unchanged-ms', type=float, default=2)
    args = parser.parse_args()
    require(2 <= args.samples <= 24, 'samples must be 2..24')
    report = {'ok': False, 'checked_at': datetime.now(timezone.utc).isoformat(),
              'scope': 'RAM drawing and synchronous SPI completion, not optical tearing',
              'cases': {}, 'failures': []}
    board = None
    try:
        board = Board(args.port)
        report['board'] = board.call('info')
        expect_ok(report['board'])
        require(not report['board'].get('job'), 'Stop the current job explicitly before benchmarking')
        for name in CASES:
            response = board.call('eval', source=source_for(name, args.samples), timeout_ms=5000)
            expect_ok(response)
            rows = []
            for line in response.get('output', '').splitlines():
                fields = line.split('\t')
                if len(fields) == 4 and fields[:2] == ['DISPLAY_BENCH', name]:
                    rows.append({'draw_ms': int(fields[2]), 'show_ms': int(fields[3])})
            require(len(rows) == args.samples, 'Missing samples: ' + repr(response))
            result = {'samples': rows,
                      'draw_mean_ms': sum(r['draw_ms'] for r in rows) / len(rows),
                      'show_mean_ms': sum(r['show_ms'] for r in rows) / len(rows),
                      'show_max_ms': max(r['show_ms'] for r in rows)}
            report['cases'][name] = result
            limit = (args.max_full_ms if name == 'full_frame' else
                     args.max_unchanged_ms if name == 'unchanged' else args.max_small_ms)
            if result['show_max_ms'] > limit:
                report['failures'].append('%s show max %s ms > %s ms' % (name, result['show_max_ms'], limit))
            print(name, json.dumps({k: v for k, v in result.items() if k != 'samples'}), flush=True)
        report['after'] = board.call('info')
        expect_ok(report['after'])
        require(report['after']['boot_id'] == report['board']['boot_id'], 'Device reset during benchmark')
        report['ok'] = not report['failures']
        for failure in report['failures']:
            print('FAIL', failure, flush=True)
        return 0 if report['ok'] else 1
    except (AssertionError, OSError, TimeoutError, ValueError) as error:
        report['error'] = str(error)
        print('FAIL', error, file=sys.stderr)
        return 1
    finally:
        if board:
            board.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    sys.exit(main())
