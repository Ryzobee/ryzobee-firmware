"""Production pure C pair leases, no GPIO/device operations."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PINS = ROOT / "components/ryz_tool_pins"


class ToolPinsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-tool-pins-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binaries = {}
        for variant, defines in {"quad": [], "octal_flash": ["-DCONFIG_ESPTOOLPY_OCT_FLASH=1"],
                                 "octal_psram": ["-DCONFIG_SPIRAM_MODE_OCT=1"]}.items():
            binary = Path(cls.directory.name) / variant
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                            "-Werror", "-pthread", "-g", "-fsanitize=address,undefined",
                            "-I" + str(ROOT / "tests/tool_pins_stubs"),
                            *defines, "-I" + str(PINS / "include"), str(PINS / "ryz_tool_pins.c"),
                            str(ROOT / "tests/tool_pins_test.c"), "-o", str(binary)],
                           check=True, timeout=30)
            cls.binaries[variant] = binary

    def run_case(self, case, variant="quad"):
        result = subprocess.run([str(self.binaries[variant]), case], capture_output=True,
                                text=True, timeout=15,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("TOOL_PINS_PASS", result.stdout)

    def test_pair_leases_are_atomic_and_stale_tokens_cannot_release_reuse(self):
        self.run_case("pairs")

    def test_actual_quad_build_has_only_the_approved_exposed_pins(self):
        self.run_case("eligibility")

    def test_octal_flash_reserves_33_through_37(self):
        self.run_case("eligibility", "octal_flash")

    def test_octal_psram_reserves_33_through_37(self):
        self.run_case("eligibility", "octal_psram")

    def test_invalid_inputs_clear_token_and_cannot_change_ownership(self):
        self.run_case("invalid")

    def test_real_threads_never_overlap_pair_ownership_or_reuse_tokens(self):
        self.run_case("concurrency")


if __name__ == "__main__":
    unittest.main()
