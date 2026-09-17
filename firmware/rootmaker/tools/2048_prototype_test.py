#!/usr/bin/env python3
"""PROTOTYPE: verify the raw-Lua 2048 path on a real Ryzobee board."""
import argparse
import base64
import hashlib
import json
import time
from pathlib import Path

from board_lua import expect_ok
from workbench_tests import WorkbenchBoard


DEVICE_NAME = 'ryz2048_prototype.lua'


def job_output(events, job_id):
    chunks = {}
    for event in events:
        if event.get('event') == 'output' and event.get('job_id') == job_id:
            chunks[event['seq']] = base64.b64decode(event['data_b64'])
    return b''.join(chunks[key] for key in sorted(chunks)).decode('utf-8')


def wait_for_output(board, job_id, text, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if text in job_output(board.events, job_id):
            return job_output(board.events, job_id)
        board.read_response()
    raise AssertionError(f'missing output {text!r}: {job_output(board.events, job_id)!r}')


def stop(board, job_id):
    started = time.monotonic()
    expect_ok(board.command('lua --stop ' + job_id))
    result = board.wait_job(job_id)
    elapsed_ms = round((time.monotonic() - started) * 1000, 1)
    assert result['state'] == 'stopped', result
    assert elapsed_ms < 500, elapsed_ms
    return {'elapsed_ms': elapsed_ms, 'job': result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--leave-running', action='store_true')
    parser.add_argument('--replace-prototype', action='store_true')
    args = parser.parse_args()

    source_path = Path(__file__).resolve().parents[1] / 'scripts/2048_prototype.lua'
    source = source_path.read_text(encoding='utf-8')
    source_bytes = source.encode('utf-8')
    source_sha = hashlib.sha256(source_bytes).hexdigest()
    assert 0 < len(source_bytes) <= 8192

    board = WorkbenchBoard(args.port)
    report = {'ok': False, 'source_bytes': len(source_bytes), 'source_sha256': source_sha}
    active_job = None
    try:
        before = board.call('info')
        expect_ok(before)
        assert before['display_driver_ready'] and before['touch_driver_ready'], before
        if before['job']:
            assert args.replace_prototype and before['job']['name'] == DEVICE_NAME, before['job']
            stop(board, before['job']['job_id'])
            before = board.call('info')

        listing = board.call('list')
        expect_ok(listing)
        existing = next((item for item in listing['files'] if item['name'] == DEVICE_NAME), None)
        if existing:
            existing_file = board.call('get', name=DEVICE_NAME)
            expect_ok(existing_file)
            assert existing_file['sha256'] == source_sha or args.replace_prototype, \
                'refusing to overwrite a different prototype'

        uploaded = board.call('put', name=DEVICE_NAME, source=source, sha256=source_sha)
        expect_ok(uploaded)
        assert uploaded['sha256'] == source_sha
        fetched = board.call('get', name=DEVICE_NAME)
        expect_ok(fetched)
        assert fetched['source'] == source and fetched['sha256'] == source_sha

        first = board.command('lua --run-async --path ' + DEVICE_NAME)
        expect_ok(first)
        active_job = first['job']['job_id']
        first_output = wait_for_output(board, active_job, 'GAME START')
        assert 'RYZ_2048_SELFTEST_PASS' in first_output, first_output
        time.sleep(1)
        running = board.command('lua --job ' + active_job)['job']
        assert running['state'] == 'running' and running['sha256'] == source_sha, running
        during = board.call('info')
        assert during['boot_id'] == before['boot_id'], during
        assert during['touch_reads'] > before['touch_reads'], (before, during)
        stopped = stop(board, active_job)
        active_job = None

        second = board.command('lua --run-async --path ' + DEVICE_NAME)
        expect_ok(second)
        active_job = second['job']['job_id']
        second_output = wait_for_output(board, active_job, 'GAME START')
        assert 'RYZ_2048_SELFTEST_PASS' in second_output, second_output
        final = board.call('info')
        assert final['boot_id'] == before['boot_id'], final
        assert final['job']['state'] == 'running' and final['job']['job_id'] == active_job, final

        report.update({
            'ok': True,
            'device_file': DEVICE_NAME,
            'boot_id': before['boot_id'],
            'firmware': before['firmware'],
            'lua': before['lua'],
            'first_job': running,
            'first_output': first_output,
            'stop': stopped,
            'touch_reads_delta': during['touch_reads'] - before['touch_reads'],
            'running_job': final['job'],
            'running_output': second_output,
        })
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if not args.leave_running:
            stop(board, active_job)
            active_job = None
    finally:
        if active_job and not report['ok']:
            try:
                stop(board, active_job)
            except Exception:
                pass
        board.close()
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
