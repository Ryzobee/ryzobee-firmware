"""Real compatibility wrapper and simulated SDK callers, not hardware allocation.

Host calls the wrapper symbol directly. Final GNU --wrap wiring is a separate
target-link check, not a claim made by these tests.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components/ryz_board/i2c_idf_compat.c"


class I2cIdfCompatHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-i2c-idf-compat-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "i2c_idf_compat_test"
        cls.command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-g",
                       "-I", str(ROOT / "tests/i2c_idf_compat_stubs")]
        subprocess.run(cls.command + [str(SOURCE), str(ROOT / "tests/i2c_idf_compat_test.c"),
                                     "-o", str(cls.binary)], check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("I2C_IDF_COMPAT_PASS " + name, result.stdout)

    def test_master_null_success_becomes_no_mem_before_sdk_caller_dereference(self):
        self.run_case("master_null_success")

    def test_slave_null_success_becomes_no_mem_before_sdk_caller_dereference(self):
        self.run_case("slave_null_success")

    def test_valid_success_and_all_port_mode_arguments_pass_through_unchanged(self):
        self.run_case("success")

    def test_original_errors_and_returned_handles_are_not_rewritten_or_released(self):
        self.run_case("errors")

    def test_sdk_version_change_requires_explicit_compatibility_review(self):
        result = subprocess.run(self.command + ["-DTEST_IDF_PATCH=5", "-c", str(SOURCE),
                                "-o", str(Path(self.directory.name) / "wrong_version.o")],
                                capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Re-audit the I2C base-allocation compatibility wrapper", result.stderr)


if __name__ == "__main__":
    unittest.main()
