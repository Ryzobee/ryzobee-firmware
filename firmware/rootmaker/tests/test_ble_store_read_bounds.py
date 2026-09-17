"""Actual SDK member-sized output ABI through the production store callback."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/ryz_ble"


class BleStoreReadBoundsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="ryz-ble-store-read-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "store-read"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
                   "-Wall", "-Wextra", "-Werror", "-pthread",
                   "-fsanitize=address,undefined", "-DRYZ_BLE_HOST_TEST"]
        for path in (ROOT / "tests/ble_stubs", COMPONENT / "include", COMPONENT):
            command += ["-I", str(path)]
        command += [str(COMPONENT / "ryz_ble.c"), str(COMPONENT / "ryz_ble_store.c"),
                    str(ROOT / "tests/ble_store_read_bounds_test.c"), "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=5,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("BLE_STORE_READ_BOUNDS_PASS " + name, result.stdout)

    def test_local_irk_member_sized_sdk_output_is_not_overwritten(self):
        self.run_case("local-irk")

    def test_first_boot_missing_irk_uses_real_member_sized_caller_storage(self):
        self.run_case("first-boot")

    def test_cccd_member_success_and_missing_do_not_overwrite_caller(self):
        self.run_case("cccd")

    def test_csfc_member_success_and_missing_do_not_overwrite_caller(self):
        self.run_case("csfc")

    def test_our_security_member_success_and_missing_preserve_bounds(self):
        self.run_case("our-sec")

    def test_peer_security_member_success_and_missing_preserve_bounds(self):
        self.run_case("peer-sec")

    def test_rpa_member_success_and_missing_do_not_overwrite_caller(self):
        self.run_case("rpa")

    def test_unbonded_unknown_and_invalid_reads_do_not_touch_caller(self):
        self.run_case("missing")


if __name__ == "__main__":
    unittest.main()
