"""Public app-host protocol contract used by the future Server Adapter."""
from pathlib import Path
import json
import select
import subprocess
import tempfile
import time
import unittest


class AppHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[1]
        subprocess.run(['sh', str(cls.root / 'tools/build_app_host.sh')],
                       cwd=cls.root, check=True)

    def test_batch_pointer_seed_frames_marks_and_replayable_ops(self):
        root = self.root
        commands = '\n'.join([
            'end 20',
            'pointer 2 down 10 20',
            'check 5',
            'pointer 8 move 30 40',
            'check 10',
            'pointer 12 up',
            'check 15',
            '',
        ])
        completed = subprocess.run(
            [str(root / 'build-host/app-host'), '--seed', '305419896',
             str(root / 'tests/app_host_fixture.lua')],
            input=commands, text=True, capture_output=True, timeout=10, check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        rows = [json.loads(line) for line in completed.stdout.splitlines() if line]
        frames = [row for row in rows if row.get('event') == 'frame']
        summary = rows[-1]
        self.assertGreaterEqual(len(frames), 4)
        self.assertTrue(summary['summary'] and summary['ok'])
        self.assertEqual(summary['seed'], 305419896)
        self.assertEqual([frame['seq'] for frame in frames], list(range(1, len(frames) + 1)))
        for frame in frames:
            self.assertEqual((frame['runtime'], frame['catalog']),
                             ('ryz-app/1', 'ryz-capabilities/1'))
            self.assertEqual((frame['width'], frame['height']), (240, 240))
            self.assertIn('ops', frame)
            self.assertIn('nodes', frame)
        first = frames[0]
        self.assertEqual(first['ops'], [
            {'op': 'clear', 'color': 0x0841},
            {'op': 'text', 'x': 8, 'y': 7, 'text': '2048', 'color': 0xffe0, 'scale': 3},
        ])
        self.assertTrue(first['nodes']['app.running'])
        self.assertEqual(first['nodes']['screen.presented'], 1)
        self.assertEqual(first['nodes']['screen.text_count'], 1)
        self.assertEqual(first['nodes']['screen.rect_count'], 0)
        self.assertEqual(first['nodes']['screen.text.0'], '2048')
        self.assertEqual(first['nodes']['mark.seed'], 305419896)
        self.assertNotIn('seed', first['nodes'])
        down = next(frame for frame in frames if frame['nodes'].get('mark.phase') == 'down')
        self.assertTrue(down['pressed'])
        self.assertEqual((down['x'], down['y']), (10, 20))
        self.assertEqual(down['ops'][0], {'op': 'rect', 'x': 10, 'y': 20,
                                          'width': 2, 'height': 2, 'color': 0x07e0})
        move = next(frame for frame in frames if frame['nodes'].get('mark.phase') == 'move')
        self.assertEqual((move['x'], move['y']), (30, 40))
        up = next(frame for frame in frames if frame['nodes'].get('mark.phase') == 'up')
        self.assertFalse(up['pressed'])
        self.assertFalse(up['has_position'])
        self.assertFalse(up['nodes']['mark.has_position'])
        self.assertEqual((up['x'], up['y']), (30, 40))
        self.assertEqual(up['ops'], [])
        self.assertEqual(up['nodes']['screen.text_count'], 1)
        self.assertEqual(up['nodes']['screen.rect_count'], 2)
        self.assertEqual(up['nodes']['screen.text.0'], '2048')

    def test_batch_accepts_move_first_sample_but_rejects_impossible_edges(self):
        root = self.root

        def run(*commands):
            completed = subprocess.run(
                [str(root / 'build-host/app-host'), '--seed', '2048',
                 str(root / 'tests/app_host_fixture.lua')],
                input='\n'.join([*commands, '']), text=True, capture_output=True,
                timeout=10, check=False,
            )
            rows = [json.loads(line) for line in completed.stdout.splitlines() if line]
            return completed, rows

        completed, rows = run(
            'end 20',
            'pointer 2 move 210 120',
            'pointer 8 move 30 120',
            'pointer 12 up',
            'check 15',
        )
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        frames = [row for row in rows if row.get('event') == 'frame']
        move = next(frame for frame in frames
                    if frame['nodes'].get('mark.phase') == 'move')
        self.assertEqual((move['pressed'], move['has_position'], move['x'], move['y']),
                         (True, True, 210, 120))
        up = next(frame for frame in frames if frame['nodes'].get('mark.phase') == 'up')
        self.assertEqual((up['pressed'], up['has_position'], up['x'], up['y']),
                         (False, False, 30, 120))
        self.assertTrue(rows[-1]['summary'] and rows[-1]['ok'])

        for invalid in [
            ('end 20', 'pointer 2 up'),
            ('end 20', 'pointer 2 down 10 20', 'pointer 8 down 30 40'),
        ]:
            rejected, rejected_rows = run(*invalid)
            self.assertEqual(rejected.returncode, 1, rejected.stderr + rejected.stdout)
            self.assertTrue(rejected_rows[-1]['summary'])
            self.assertFalse(rejected_rows[-1]['ok'])
            self.assertEqual(rejected_rows[-1]['phase'], 'input')
            self.assertEqual(rejected_rows[-1]['error'], 'pointer queue or phase invalid')

    def test_live_pointer_stream_and_stop_finish_cleanly(self):
        root = self.root
        process = subprocess.Popen(
            [str(root / 'build-host/app-host'), '--live', '--seed', '9',
             str(root / 'tests/app_host_fixture.lua')],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )

        def next_row(timeout=3):
            ready, _, _ = select.select([process.stdout], [], [], timeout)
            self.assertTrue(ready, 'timed out waiting for app-host NDJSON')
            line = process.stdout.readline()
            self.assertTrue(line, 'app-host stdout closed before expected row')
            return json.loads(line)

        first = next_row()
        self.assertEqual(first['event'], 'frame')
        self.assertEqual(first['nodes']['mark.seed'], 9)
        for command, phase, point in [
            ('pointer down 6 7\n', 'down', (6, 7)),
            ('pointer move 8 9\n', 'move', (8, 9)),
            ('pointer up\n', 'up', (8, 9)),
        ]:
            process.stdin.write(command)
            process.stdin.flush()
            deadline = time.monotonic() + 3
            while True:
                row = next_row(max(0.01, deadline - time.monotonic()))
                if row.get('nodes', {}).get('mark.phase') == phase:
                    self.assertEqual((row['x'], row['y']), point)
                    self.assertEqual(row['has_position'], phase != 'up')
                    self.assertEqual(row['nodes']['mark.has_position'], phase != 'up')
                    break
                self.assertLess(time.monotonic(), deadline)

        process.stdin.write('stop\n')
        process.stdin.flush()
        self.assertEqual(process.wait(timeout=3), 0)
        remaining = [json.loads(line) for line in process.stdout.read().splitlines() if line]
        self.assertTrue(remaining[-1]['summary'] and remaining[-1]['ok'])
        final_frame = next(row for row in reversed(remaining) if row.get('event') == 'frame')
        self.assertFalse(final_frame['nodes']['app.running'])
        process.stdin.close()
        process.stdout.close()
        process.stderr.close()

    def test_handled_hardware_unavailability_cannot_produce_a_passed_receipt(self):
        calls = [
            ("require('wifi').is_connected()", "false"),
            ("require('ble').is_connected()", "false"),
            ("require('imu').init()", "nil"),
            ("require('imu').read()", "nil"),
            ("require('imu').deinit()", "nil"),
        ]
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'hardware.lua'
            for call, expected in calls:
                with self.subTest(call=call):
                    source.write_text(
                        "-- ryz-app/1\nlocal value, reason = " + call + "\n"
                        "assert(value == " + expected + " and reason == 'unavailable')\n",
                        encoding='utf-8',
                    )
                    completed = subprocess.run(
                        [str(self.root / 'build-host/app-host'), str(source)],
                        input='end 100\n', text=True, capture_output=True,
                        timeout=5, check=False,
                    )
                    self.assertEqual(completed.returncode, 1,
                                     completed.stderr + completed.stdout)
                    summary = json.loads(completed.stdout.splitlines()[-1])
                    self.assertFalse(summary['ok'])
                    self.assertEqual(summary['phase'], 'unsupported')
                    self.assertIn('no hardware simulation or passed receipt', summary['error'])

    def test_sandbox_globals_and_require_allowlist_at_process_seam(self):
        root = self.root
        safe = """-- ryz-app/1
assert(io == nil and os == nil and debug == nil and package == nil)
assert(type(coroutine) == 'table' and load == nil and loadfile == nil and dofile == nil)
assert(pcall == nil and xpcall == nil)
"""
        forbidden = "-- ryz-app/1\nrequire('socket')\n"
        with tempfile.TemporaryDirectory() as directory:
            safe_path = Path(directory) / 'safe.lua'
            forbidden_path = Path(directory) / 'forbidden.lua'
            safe_path.write_text(safe, encoding='utf-8')
            forbidden_path.write_text(forbidden, encoding='utf-8')
            accepted = subprocess.run(
                [str(root / 'build-host/app-host'), str(safe_path)], input='end 1\n',
                text=True, capture_output=True, timeout=5, check=False,
            )
            rejected = subprocess.run(
                [str(root / 'build-host/app-host'), str(forbidden_path)], input='end 1\n',
                text=True, capture_output=True, timeout=5, check=False,
            )
        self.assertEqual(accepted.returncode, 0, accepted.stderr + accepted.stdout)
        self.assertTrue(json.loads(accepted.stdout.splitlines()[-1])['ok'])
        self.assertEqual(rejected.returncode, 1, rejected.stderr + rejected.stdout)
        summary = json.loads(rejected.stdout.splitlines()[-1])
        self.assertFalse(summary['ok'])
        self.assertIn('module not allowed: socket', summary['error'])


if __name__ == '__main__':
    unittest.main()
