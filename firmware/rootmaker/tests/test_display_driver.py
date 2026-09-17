"""Run the actual display.c against a host-only asynchronous transport boundary."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class DisplayDriverTest(unittest.TestCase):
    def test_dirty_transfers_and_pixel_contract(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix='ryzobee-display-test-') as tmp:
            binary = str(Path(tmp) / 'display-test')
            command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                       '-fsanitize=address,undefined',
                       '-I' + str(root / 'tests/display_stubs'),
                       '-I' + str(root / 'components/ryz_board/include'),
                       str(root / 'tests/display_driver_test.c'),
                       str(root / 'components/ryz_board/display.c'),
                       '-o', binary]
            subprocess.run(command, check=True)
            subprocess.run([binary], check=True)
            for rotation in range(4):
                with self.subTest(boot_rotation=rotation):
                    subprocess.run([binary, 'boot-configuration', str(rotation)], check=True)
            for scenario in ('configuration', 'brightness-configuration', 'brightness-preview',
                             'brightness-set', 'brightness-update', 'brightness-rollback-set',
                             'brightness-rollback-update', 'brightness-guards',
                             'commits', 'block-drain', 'step-drain', 'privacy',
                             'privacy-pending', 'privacy-unready',
                             'privacy-disable-set', 'privacy-disable-update',
                             'privacy-drain', 'privacy-draw', 'privacy-final-drain',
                             'privacy-restore-set', 'privacy-restore-update',
                             'native-identical', 'native-one-pixel', 'native-repair',
                             'native-bounds', 'native-privacy-black'):
                with self.subTest(scenario=scenario):
                    subprocess.run([binary, scenario], check=True)


if __name__ == '__main__':
    unittest.main()
