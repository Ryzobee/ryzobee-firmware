"""Real core + stream + RPC + IDF cJSON, with source/OS/external-gate Adapters.

The checking external gate is not the real ownership broker.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/ryz_monitor"
WORKBENCH = ROOT / "components/ryz_workbench"


class MonitorRpcIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise RuntimeError("Set IDF_PATH to the project's ESP-IDF 5.5.4 checkout")
        json = Path(idf) / "components/json/cJSON"
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-monitor-integration-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "monitor_integration_test"
        json_object = Path(cls.directory.name) / "cJSON.o"
        compiler = os.environ.get("CC", "cc")
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-fsanitize=address,undefined", "-g"]
        subprocess.run([compiler, *flags, "-Wno-deprecated-declarations",
                        "-I", str(json), "-c", str(json / "cJSON.c"),
                        "-o", str(json_object)], check=True, timeout=60)
        command = [compiler, *flags, "-DRYZ_TEST_TOOL=RYZ_TOOL_MONITOR"]
        for include in [ROOT / "tests/monitor_stubs", COMPONENT, COMPONENT / "include", WORKBENCH, json,
                        ROOT / "tests/workbench_tool_gate_stubs",
                        *[ROOT / "components" / name / "include" for name in ("ryz_tools", "ryz_i2c_scan", "ryz_rgb")]]:
            command.extend(["-I", str(include)])
        command.extend([str(COMPONENT / "ryz_monitor.c"), str(COMPONENT / "ryz_monitor_stream.c"),
                        str(ROOT / "tests/workbench_tool_gate_stubs/tool_gate_test.c"),
                        str(WORKBENCH / "workbench_monitor_rpc.c"),
                        str(ROOT / "tests/monitor_rpc_integration_test.c"),
                        str(json_object), "-lm", "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=60)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=20,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("MONITOR_RPC_INTEGRATION_PASS " + name, result.stdout)

    def test_first_configuration_and_start_without_view_do_not_fabricate_history(self):
        self.run_case("initial_configure")

    def test_admitted_start_before_stream_reset_is_not_a_zero_identity_history(self):
        self.run_case("pending_start_stop")

    def test_pause_clear_stop_inactive_configure_and_restart_preserve_history_identity(self):
        self.run_case("history")

    def test_running_source_switch_restore_and_success_publish_separate_configurations(self):
        self.run_case("switch_source")


if __name__ == "__main__":
    unittest.main()
