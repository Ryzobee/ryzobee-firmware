"""Real versioned BLE record codec; bounded fault-injected NVS peer."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BleStoreCodecTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-ble-codec-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "codec-test"
        component = ROOT / "components/ryz_ble"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-DRYZ_BLE_HOST_TEST",
                   "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined"]
        command += [part for path in (ROOT / "tests/ble_stubs", component, component / "include")
                    for part in ("-I", str(path))]
        command += [str(ROOT / "tests/ble_store_codec_test.c"), str(component / "ryz_ble_store.c"),
                    "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=30)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=5,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("BLE_STORE_CODEC_PASS " + case, result.stdout)

    def test_whole_blob_roundtrip_keys_metadata_preference_and_peer_clear(self):
        self.run_case("roundtrip")

    def test_unsafe_key_records_are_rejected_before_nvs_io(self):
        self.run_case("invalid")

    def test_crc_version_length_and_security_metadata_corruption_fail_closed(self):
        self.run_case("corruption")

    def test_missing_records_default_on_but_real_read_errors_clear_output(self):
        self.run_case("load-errors")

    def test_all_save_stages_and_ambiguous_commit_fail_without_leaked_handles(self):
        self.run_case("save-errors")


if __name__ == "__main__":
    unittest.main()
