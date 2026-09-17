"""Real metrics ESP Adapter + actual IDF tsens public header; SDK calls replaced.

No real hardware allocator, FreeRTOS scheduler, ADC/calibration or device test.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/ryz_system_services"


class MetricsEspTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-system-metrics-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "metrics_esp"
        idf = Path(json.loads((ROOT / "build/project_description.json").read_text())["idf_path"])
        cls.command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-g"]
        for include in [ROOT / "tests/system_metrics_stubs", ROOT / "tests/system_services_stubs",
                        COMPONENT, idf / "components/esp_driver_tsens/include",
                        idf / "components/esp_system/include"]:
            cls.command.extend(["-I", str(include)])
        cls.command.extend([str(COMPONENT / "ryz_system_metrics_esp.c"),
                            str(ROOT / "tests/system_metrics_esp_test.c"), "-o", str(cls.binary)])
        subprocess.run(cls.command, check=True, timeout=30)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SYSTEM_METRICS_ESP_PASS " + case, result.stdout)

    def test_temperature_lifetime_error_output_range_and_recovery(self):
        self.run_case("temperature")

    def test_install_and_enable_failures_retry_without_duplicate_handle(self):
        self.run_case("retry")

    def test_two_actual_typed_idle_calls_keep_u64_values_and_fail_closed(self):
        self.run_case("idle")

    def test_ipc_failures_wrong_core_and_partial_samples_are_not_published(self):
        self.run_case("ipc_failure")

    def test_u32_target_configuration_is_rejected_at_compile_time(self):
        result = subprocess.run(self.command[:1] + ["-DCONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=0"] +
                                self.command[1:], text=True, capture_output=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("System LOAD requires ESP_TIMER runtime statistics with a 64-bit counter", result.stderr)


if __name__ == "__main__":
    unittest.main()
