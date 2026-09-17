#!/usr/bin/env python3
"""Assert a running job survives repeated serial close/open, without reset."""
import argparse
import json
import time
import uuid
from pathlib import Path
from workbench_tests import WorkbenchBoard
from board_lua import expect_ok

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--cycles', type=int, default=5)
    parser.add_argument('--interval', type=float, default=1)
    args = parser.parse_args()
    if not 1 <= args.cycles <= 100 or not 0.1 <= args.interval <= 30:
        parser.error('cycles must be 1..100 and interval 0.1..30 seconds')
    name = 'wb_' + uuid.uuid4().hex[:8] + '.lua'
    board = None
    created = False
    report = {'ok': False, 'cycles': [], 'name': name}
    failure = None
    try:
        board = WorkbenchBoard(args.port)
        initial = board.call('info')
        expect_ok(initial)
        assert initial.get('job') is None, 'device is busy; refusing to stop an existing job'
        expect_ok(board.call('put', name=name, source="local b=require('ryzobee'); while true do b.sleep_ms(100) end"))
        created = True
        started = board.command('lua --run-async --path ' + name)
        expect_ok(started)
        report['job_id'] = started['job']['job_id']
        for index in range(args.cycles):
            before = board.call('info')
            expect_ok(before)
            board.close()
            board = None
            time.sleep(args.interval)
            board = WorkbenchBoard(args.port)
            after = board.call('info')
            expect_ok(after)
            job = after.get('job') or {}
            reset = before['boot_id'] != after['boot_id']
            continued = (not reset and job.get('job_id') == report['job_id']
                         and job.get('state') == 'running'
                         and job['elapsed_ms'] > before['job']['elapsed_ms'])
            report['cycles'].append({'cycle': index + 1, 'boot_changed': reset,
                                     'same_job_continued': continued,
                                     'before': before, 'after': after})
            assert continued, 'serial reconnect reset or lost the original running job'
            print('PASS reconnect', index + 1, report['job_id'], job['elapsed_ms'], 'ms', flush=True)
        report['ok'] = True
    except Exception as error:
        failure = error
        report['error'] = str(error)
    finally:
        try:
            if created:
                if board is None:
                    board = WorkbenchBoard(args.port)
                status = board.call('info')
                expect_ok(status)
                job = status.get('job')
                if job and job['name'] == name:
                    expect_ok(board.command('lua --stop ' + job['job_id']))
                    board.wait_job(job['job_id'])
                expect_ok(board.call('remove', name=name))
                report['cleanup_ok'] = True
        except Exception as error:
            report['ok'] = False
            report['cleanup_error'] = str(error)
            failure = failure or error
        finally:
            if board is not None:
                board.close()
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2))
    if failure:
        raise failure


if __name__ == '__main__':
    main()
