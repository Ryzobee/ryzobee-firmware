"""Production startup wrappers/getter; external SDK failure boundaries only."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/ryz_ble"


class BleInitDiagnosticsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="ryz-ble-init-diagnostics-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "diagnostics"
        command = [
            os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
            "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined",
            "-DRYZ_BLE_HOST_TEST", "-DRYZ_BLE_INIT_DIAGNOSTICS_HOST_TEST",
        ]
        for path in (ROOT / "tests/ble_init_diag_stubs", ROOT / "tests/ble_stubs",
                     COMPONENT / "include", COMPONENT):
            command += ["-I", str(path)]
        command += [str(COMPONENT / "ryz_ble.c"), str(COMPONENT / "ryz_ble_store.c"),
                    str(ROOT / "tests/ble_init_diagnostics_test.c"), "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=30)

    def run_case(self, stage):
        result = subprocess.run(
            [str(self.binary), str(stage)], capture_output=True, text=True, timeout=5,
            env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"BLE_INIT_DIAGNOSTICS_PASS stage={stage}", result.stdout)

    def test_success_is_silent_and_snapshot_finished(self):
        self.run_case(0)

    def test_buffer_no_mem_is_preserved_and_vhci_is_unentered(self):
        self.run_case(1)

    def test_vhci_callback_and_failure_are_transparently_forwarded(self):
        self.run_case(2)

    def test_later_port_failure_is_not_mislabelled_as_hci_failure(self):
        self.run_case(3)


if __name__ == "__main__":
    unittest.main()
