"""Real UART source + shared pin broker; only SDK/RTOS boundaries replaced."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MONITOR = ROOT / "components/ryz_monitor"
PINS = ROOT / "components/ryz_tool_pins"
SCAN = ROOT / "components/ryz_i2c_scan"


class MonitorUartTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-monitor-uart-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "uart"
        cls.includes = [ROOT / "tests/monitor_uart_stubs", ROOT / "tests/i2c_scan_bus_stubs",
                        MONITOR, MONITOR / "include", PINS / "include", SCAN,
                        SCAN / "include", ROOT / "components/ryz_board/include"]
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-g", "-fsanitize=address,undefined",
                        *["-I" + str(p) for p in cls.includes],
                        str(MONITOR / "ryz_monitor_uart.c"), str(PINS / "ryz_tool_pins.c"),
                        str(SCAN / "ryz_i2c_scan_bus.c"), str(SCAN / "ryz_i2c_scan_idf.c"),
                        str(ROOT / "tests/monitor_uart_test.c"), "-o", str(cls.binary)],
                       check=True, timeout=30)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True,
                                timeout=15, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("MONITOR_UART_PASS", result.stdout)

    def test_config_is_pure_strict_and_has_exact_baud_whitelist(self):
        self.run_case("validate")

    def test_rx_only_uart1_owns_pair_until_driver_and_rx_gpio_are_released(self):
        self.run_case("lifecycle")

    def test_binary_rx_does_not_require_data_events_and_error_observations_are_bounded(self):
        self.run_case("reception")

    def test_native_uart_pin_lease_conflicts_and_wrong_cpu_change_no_hardware(self):
        self.run_case("admission")

    def test_other_task_and_wrong_core_cannot_consume_active_owner_resources(self):
        self.run_case("wrong_owner")

    def test_install_errors_discard_early_dangling_sdk_queue_and_recover(self):
        self.run_case("install_errors")

    def test_each_configuration_failure_preserves_only_owned_cleanup_resources(self):
        self.run_case("configuration_errors")

    def test_actual_baud_readback_accepts_at_most_two_percent_divider_error(self):
        self.run_case("baud_accuracy")

    def test_negative_or_invalid_read_return_is_failure_with_cleared_sample(self):
        self.run_case("read_errors")

    def test_gpio_restore_failure_blocks_pair_and_next_begin_retries_cleanup_first(self):
        self.run_case("cleanup")

    def test_defensive_sdk_error_states_do_not_reuse_consumed_queues(self):
        self.run_case("defensive_delete")

    def test_real_i2c_bus_and_uart_share_pair_lease_including_failed_uart_cleanup(self):
        self.run_case("i2c_interoperability")

    def test_unaudited_sdk_configs_and_uart1_console_trace_fail_compilation(self):
        for define in ["CONFIG_PM_ENABLE=1", "CONFIG_FREERTOS_SMP=1", "CONFIG_FREERTOS_UNICORE=1",
                       "CONFIG_IDF_TARGET_ESP32S3=0", "ESP_IDF_VERSION=0",
                       "CONFIG_ESP_CONSOLE_UART_NUM=1", "CONFIG_APPTRACE_DEST_UART1=1"]:
            with self.subTest(define=define):
                result = subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D" + define,
                                         *["-I" + str(p) for p in self.includes], "-c",
                                         str(MONITOR / "ryz_monitor_uart.c"), "-o",
                                         str(Path(self.directory.name) / "unsupported.o")],
                                        capture_output=True, text=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Monitor UART", result.stderr)


if __name__ == "__main__":
    unittest.main()
