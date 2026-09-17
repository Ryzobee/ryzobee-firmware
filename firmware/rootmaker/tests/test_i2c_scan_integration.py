"""Real JSON RPC -> real scanner worker -> JSON status, without a device.

Only OS/bus/pin seams are adapted. IDF cJSON and both production modules run;
this does not execute FreeRTOS, the UART transport, or physical I2C.
The external write gate is a checking Adapter, not the real ownership broker.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCANNER = ROOT / "components/ryz_i2c_scan"
WORKBENCH = ROOT / "components/ryz_workbench"


class I2cScanIntegrationHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise RuntimeError("Set IDF_PATH to the project's ESP-IDF 5.5.4 checkout")
        json = Path(idf) / "components/json/cJSON"
        if not (json / "cJSON.c").is_file():
            raise RuntimeError("IDF_PATH does not contain cJSON sources")
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-i2c-scan-integration-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "i2c_scan_integration_test"
        json_object = Path(cls.directory.name) / "cJSON.o"
        common = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                  "-Werror", "-fsanitize=address,undefined", "-g"]
        # Only third-party cJSON gets the macOS deprecated-sprintf exception.
        subprocess.run(common + ["-Wno-deprecated-declarations", "-I", str(json),
                                 "-c", str(json / "cJSON.c"), "-o", str(json_object)],
                       check=True, timeout=30)
        command = common + ["-pthread", "-DRYZ_TEST_TOOL=RYZ_TOOL_I2C"]
        for include in [ROOT / "tests/i2c_scan_integration_stubs", SCANNER,
                        SCANNER / "include", ROOT / "components/ryz_tool_pins/include",
                        WORKBENCH, json, ROOT / "tests/workbench_tool_gate_stubs",
                        *[ROOT / "components" / name / "include" for name in ("ryz_tools", "ryz_rgb", "ryz_monitor")]]:
            command.extend(["-I", str(include)])
        command.extend([str(SCANNER / "ryz_i2c_scan.c"),
                        str(ROOT / "tests/workbench_tool_gate_stubs/tool_gate_test.c"),
                        str(WORKBENCH / "workbench_i2c_rpc.c"),
                        str(ROOT / "tests/i2c_scan_integration_test.c"),
                        str(json_object), "-lm", "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=15,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("I2C_SCAN_INTEGRATION_PASS " + name, result.stdout)

    def test_json_start_reaches_real_worker_and_complete_ack_status(self):
        self.run_case("ack")

    def test_empty_requires_real_completed_traversal_of_112_nacks(self):
        self.run_case("empty")

    def test_inflight_cancel_ack_then_terminal_restart_rejects_old_boot_and_token(self):
        self.run_case("cancel_restart")

    def test_configure_cas_ack_is_software_only_and_does_not_relabel_completed_scan(self):
        self.run_case("configure_history")

    def test_failed_custom_teardown_is_not_empty_and_cancel_retries_release_only(self):
        self.run_case("cleanup_retry")


if __name__ == "__main__":
    unittest.main()
