"""Production scan bus + diagnostic Adapter + pin leases; SDK/RTOS alone replaced."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUS = ROOT / "components/ryz_i2c_scan"
PINS = ROOT / "components/ryz_tool_pins"


class I2cScanBusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-i2c-scan-bus-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = binary = Path(cls.directory.name) / "bus"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-g", "-fsanitize=address,undefined",
                        "-I" + str(ROOT / "tests/i2c_scan_bus_stubs"),
                        "-I" + str(ROOT / "components/ryz_board/include"),
                        "-I" + str(BUS), "-I" + str(BUS / "include"),
                        "-I" + str(PINS / "include"), str(PINS / "ryz_tool_pins.c"),
                        str(BUS / "ryz_i2c_scan_bus.c"), str(BUS / "ryz_i2c_scan_idf.c"),
                        str(ROOT / "tests/i2c_scan_bus_test.c"), "-o", str(binary)],
                       check=True, timeout=30)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("I2C_SCAN_BUS_PASS", result.stdout)

    def test_custom_pair_is_claimed_before_hardware_and_released_after_teardown(self):
        self.run_case("lifecycle")

    def test_400khz_nack_timeout_and_old_error_cannot_pollute_new_early_failure(self):
        self.run_case("errors")

    def test_partial_allocations_and_restore_failures_keep_lease_until_retry(self):
        self.run_case("allocation")

    def test_teardown_consumes_only_confirmed_resources_and_no_dangling_retries(self):
        self.run_case("cleanup")

    def test_native_reserved_software_leased_and_wrong_core_never_reconfigure_pins(self):
        self.run_case("admission")

    def test_default_borrows_board_bus_and_invalid_configs_never_create_hardware(self):
        self.run_case("defaults")

    def test_second_gpio_restore_failure_keeps_both_pin_leases_without_reusing_freed_bus(self):
        self.run_case("second_restore")

    def test_other_native_i2c1_owner_on_different_pins_is_not_deinitialized(self):
        self.run_case("occupied_controller")

    def test_unaudited_idf_chip_pm_smp_and_unicore_configs_fail_compilation(self):
        for define in ["CONFIG_PM_ENABLE=1", "CONFIG_FREERTOS_SMP=1", "CONFIG_FREERTOS_UNICORE=1",
                       "CONFIG_IDF_TARGET_ESP32S3=0", "ESP_IDF_VERSION=0"]:
            with self.subTest(define=define):
                result = subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D" + define,
                                         "-I" + str(ROOT / "tests/i2c_scan_bus_stubs"),
                                         "-I" + str(BUS), "-c", str(BUS / "ryz_i2c_scan_idf.c"),
                                         "-o", str(Path(self.directory.name) / "unsupported.o")],
                                        capture_output=True, text=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("requires the audited" if "VERSION" not in define and "TARGET" not in define
                              else "require the audited", result.stderr)


if __name__ == "__main__":
    unittest.main()
