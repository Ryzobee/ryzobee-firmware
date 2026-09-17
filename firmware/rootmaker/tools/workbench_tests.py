#!/usr/bin/env python3
"""Hardware acceptance through the public serial interface; no flashing."""
import argparse
import base64
import hashlib
import json
import time
import uuid
from pathlib import Path
from board_lua import Board, expect_ok


class WorkbenchBoard(Board):
    def read_response(self):
        line = self.serial.readline().decode('utf-8', errors='replace').strip()
        for prefix in ('RYZOBEE_RPC ', 'RYZOBEE_EVENT '):
            if prefix not in line:
                continue
            try:
                response = json.loads(line.split(prefix, 1)[1])
            except json.JSONDecodeError:
                continue
            self.events.append(response)
            return response
        return None

    def command(self, command):
        return self.call('console', command=command)

    def wait_job(self, job_id, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            response = self.command('lua --job ' + job_id)
            expect_ok(response)
            if response['job']['state'] != 'running':
                return response['job']
        raise AssertionError('job did not finish: ' + job_id)

    def raw_command(self, command):
        self.serial.write((command + '\n').encode())
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            response = self.read_response()
            if response and response.get('id') == 'terminal':
                return response
        raise AssertionError('raw Console did not respond')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', required=True)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--soak-seconds', type=int, default=0)
    parser.add_argument('--only', default='')
    args = parser.parse_args()
    board = WorkbenchBoard(args.port)
    name = 'wb_' + uuid.uuid4().hex[:8] + '.lua'
    cases = []
    completed = False
    failure = None
    def case(label, fn):
        if args.only and args.only != label:
            return
        started = time.monotonic()
        value = fn()
        cases.append({'name': label, 'elapsed_ms': round((time.monotonic() - started) * 1000), 'detail': value})
        print('PASS', label, flush=True)

    def upload(source):
        sha = hashlib.sha256(source.encode()).hexdigest()
        response = board.call('put', name=name, source=source, sha256=sha)
        expect_ok(response)
        assert response['sha256'] == sha
        return sha

    def run(source):
        upload(source)
        response = board.command('lua --run-async --path ' + name)
        expect_ok(response)
        return response['job']['job_id']

    def stop(job_id):
        started = time.monotonic()
        ack = board.command('lua --stop ' + job_id)
        ack_ms = (time.monotonic() - started) * 1000
        expect_ok(ack)
        result = board.wait_job(job_id)
        elapsed = (time.monotonic() - started) * 1000
        print('STOP_TIMING', {'ack_ms': round(ack_ms, 1), 'total_ms': round(elapsed, 1),
                              'post_ack_ms': round(elapsed - ack_ms, 1),
                              'runtime_elapsed_ms': result.get('elapsed_ms'),
                              'ack_bytes': len(json.dumps(ack)), 'status_bytes': len(json.dumps(result))}, flush=True)
        assert result['state'] == 'stopped', result
        assert elapsed < 500, elapsed
        return round(elapsed, 1)
    try:
        case('console_help', lambda: expect_ok(board.command('help'), 'run-async'))
        def raw_equivalence():
            via_rpc = board.command('help')
            via_text = board.raw_command('help')
            assert via_rpc['output'] == via_text['output']
            expect_ok(board.raw_command('board info'))
        case('raw_console_matches_rpc', raw_equivalence)
        def invalid_commands():
            for command in ('print(1)', 'lua', 'lua --run-async', 'lua --run-async --path ../boot.lua',
                            'lua --jobs --path hello.lua', 'lua --job missing', 'lua --stop missing',
                            'board nope', 'help\nboard info'):
                assert board.command(command)['ok'] is False, command
        case('invalid_commands_are_rejected', invalid_commands)
        def files():
            source = '-- 中文测试\nprint("你好，Ryzobee")\n'
            sha = upload(source)
            expect_ok(board.call('get', name=name))
            assert board.call('get', name=name)['source'] == source
            assert any(f['name'] == name for f in board.call('list')['files'])
            assert not board.call('put', name=name, source='bad', sha256='0' * 64)['ok']
            assert board.call('get', name=name)['sha256'] == sha
            # boot.lua is an ordinary user file. Mutate only this run's
            # uniquely named temporary file; never probe user-file protection.
            for size in (8192, 16384):
                upload('--' + 'x' * (size - 2))
            assert not board.call('put', name=name, source='x' * 16385)['ok']
            assert not board.call('put', name=name, source='print(1)\0print(2)')['ok']
        case('files_checksum_unicode_limits', files)
        def errors():
            for source, phase in [('local =', 'syntax'), ("error('TEST')", 'runtime'),
                                  ("local t={}; while true do t[#t+1]=string.rep('x',8192) end", 'memory')]:
                result = board.wait_job(run(source))
                assert result['state'] == 'failed' and result['phase'] == phase, result
                expect_ok(board.command('board info'))
        case('syntax_runtime_and_memory_recovery', errors)
        def cancellation(source):
            job_id = run(source)
            assert not board.call('list')['ok']
            assert not board.command('lua --run-async --path ' + name)['ok']
            return stop(job_id)
        def stop_then_restart():
            stop_ms = cancellation('while true do end')
            restarted = board.wait_job(run("print('RESTART_AFTER_STOP_PASS')"))
            assert restarted['state'] == 'done' and restarted['ok'] is True, restarted
            return {'stop_ms': stop_ms, 'restart_job_id': restarted['job_id']}
        case('stop_infinite_lua_ms', stop_then_restart)
        case('stop_native_sleep_ms', lambda: cancellation("while true do require('ryzobee').sleep_ms(5000) end"))
        case('stop_native_display_touch_ms', lambda: cancellation("local d=require('display'); local t=require('touch'); while true do t.read(); d.clear(0); d.show() end"))
        # A clean canvas skips DMA waits. The Lua hook must still yield/check
        # cancellation, so no-op refresh loops cannot starve the Console task.
        case('stop_noop_display_ms', lambda: cancellation("local d=require('display'); d.clear(0); d.show(); while true do d.show() end"))
        def flood():
            job_id = run("while true do print(string.rep('x', 256)) end")
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline:
                board.read_response()
            stop_ms = stop(job_id)
            result = board.command('lua --job ' + job_id)['job']
            assert result['dropped_bytes'] > 0, result
            return {'stop_ms': stop_ms, 'dropped_bytes': result['dropped_bytes']}
        case('log_flood_does_not_starve_stop', flood)
        def unicode_stream():
            text = '触摸屏' * 100
            job_id = run('print(' + json.dumps(text, ensure_ascii=False) + ')')
            deadline = time.monotonic() + 2
            chunks = []
            while time.monotonic() < deadline:
                response = board.read_response()
                if response and response.get('event') == 'output' and response['job_id'] == job_id:
                    chunks.append((response['seq'], base64.b64decode(response['data_b64'])))
            # call() may consume initial events while waiting for the start reply.
            chunks = [(r['seq'], base64.b64decode(r['data_b64'])) for r in board.events
                      if r.get('event') == 'output' and r['job_id'] == job_id]
            data = b''.join(data for _, data in sorted(chunks))
            assert data.decode() == text + '\n', data
        case('utf8_output_across_frames', unicode_stream)
        def partial_upload():
            old = board.call('get', name=name)['source']
            payload = json.dumps({'id':'interrupted','op':'put','name':name,'source':'print(999)'})
            board.serial.write(payload[:20].encode())
            # A newline abandons the partial JSON without any filesystem mutation.
            board.serial.write(b'\n')
            assert board.call('get', name=name)['source'] == old
        case('partial_transfer_preserves_file', partial_upload)
        def repeated_jobs():
            before = board.call('info')
            for _ in range(20):
                board.wait_job(run('print(42)'))
            after = board.call('info')
            assert before['free_psram_bytes'] - after['free_psram_bytes'] < 4096
            assert before['free_internal_bytes'] - after['free_internal_bytes'] < 8192
        case('twenty_task_lifecycles', repeated_jobs)
        if args.soak_seconds:
            def soak():
                source = (Path(__file__).resolve().parents[1] / 'scripts/workbench_demo.lua').read_text()
                job_id = run(source)
                boot = board.call('info')['boot_id']
                started = time.monotonic()
                next_check = started
                samples = []
                while time.monotonic() - started < args.soak_seconds:
                    board.read_response()
                    if time.monotonic() >= next_check:
                        info = board.call('info')
                        assert info['boot_id'] == boot and info['job']['state'] == 'running', info
                        samples.append({k: info[k] for k in ('uptime_ms', 'free_internal_bytes', 'free_psram_bytes', 'touch_reads', 'touch_errors')})
                        print('SOAK', round(time.monotonic() - started), samples[-1], flush=True)
                        next_check = time.monotonic() + 30
                return {'seconds': time.monotonic() - started, 'stop_ms': stop(job_id), 'samples': samples}
            case('screen_touch_continuous_soak', soak)
        assert cases, 'no matching test case'
        completed = True
    except BaseException as error:
        failure = str(error)
        raise
    finally:
        try:
            info = board.call('info')
            job = info.get('job')
            if job and job.get('name') == name:
                expect_ok(board.command('lua --stop ' + job['job_id']))
                board.wait_job(job['job_id'])
            listing = board.call('list')
            if listing.get('ok') and any(item['name'] == name for item in listing['files']):
                expect_ok(board.call('remove', name=name))
        except Exception as error:
            completed = False
            failure = (failure or '') + '; cleanup: ' + str(error)
        finally:
            if args.report:
                args.report.parent.mkdir(parents=True, exist_ok=True)
                args.report.write_text(json.dumps({'ok': completed, 'error': failure,
                    'temporary_script': name, 'tests': cases, 'responses': board.events}, ensure_ascii=False, indent=2))
            board.close()
    if not completed:
        raise AssertionError(failure)
    print('WORKBENCH_TESTS_PASS', len(cases))


if __name__ == '__main__':
    main()
