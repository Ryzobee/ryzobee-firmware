"""Production BOOT recognizer/FIFO and independent 5-second exit, no GPIO."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
WORKBENCH = ROOT / 'components/ryz_workbench'


class BootEventsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='ryz-boot-events-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'boot_events_test'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g',
                        '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-I', str(WORKBENCH / 'include'),
                        '-I', str(ROOT / 'components/ryz_runtime/include'),
                        str(WORKBENCH / 'workbench_boot_events.c'),
                        str(WORKBENCH / 'workbench_boot_key.c'),
                        str(ROOT / 'tests/workbench_boot_events_test.c'),
                        '-o', str(cls.binary)], check=True, timeout=60)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True,
                                timeout=15, env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('BOOT_EVENTS_PASS ' + name, result.stdout)


CASES = ('single', 'double_boundary', 'double_debounce_window', 'bounce', 'long',
         'long_release_boundary', 'delayed_sampling', 'second_long', 'triple', 'wrap', 'fifo',
         'overflow', 'overflow_delayed_poll', 'error', 'reset_held',
         'release_guard', 'unavailable', 'system_exit')
for case in CASES:
    def run(self, name=case):
        self.run_case(name)
    setattr(BootEventsTest, 'test_' + case, run)


if __name__ == '__main__':
    unittest.main()
