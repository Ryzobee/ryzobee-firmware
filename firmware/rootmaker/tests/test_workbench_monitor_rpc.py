"""Real Monitor JSON Adapter + locked IDF cJSON; controlled public service outcomes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKBENCH = ROOT / "components" / "ryz_workbench"


class WorkbenchMonitorRpcTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise RuntimeError("Set IDF_PATH to the project's ESP-IDF 5.5.4 checkout")
        json = Path(idf) / "components/json/cJSON"
        if not (json / "cJSON.c").is_file():
            raise RuntimeError("IDF_PATH does not contain cJSON sources")
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-monitor-rpc-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "monitor_rpc_test"
        json_object = Path(cls.directory.name) / "cJSON.o"
        compiler = os.environ.get("CC", "cc")
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-g"]
        # Only the locked third-party object's deprecated sprintf is exempt.
        subprocess.run([compiler, *flags, "-Wno-deprecated-declarations",
                        "-I", str(json), "-c", str(json / "cJSON.c"),
                        "-o", str(json_object)], check=True, timeout=60)
        command = [compiler, *flags, "-DRYZ_TEST_TOOL=RYZ_TOOL_MONITOR"]
        for include in [ROOT / "tests/workbench_monitor_rpc_stubs", WORKBENCH,
                        ROOT / "components/ryz_monitor/include", json,
                        ROOT / "tests/workbench_tool_gate_stubs",
                        *[ROOT / "components" / name / "include" for name in ("ryz_tools", "ryz_i2c_scan", "ryz_rgb")]]:
            command.extend(["-I", str(include)])
        command.extend([str(WORKBENCH / "workbench_monitor_rpc.c"),
                        str(ROOT / "tests/workbench_tool_gate_stubs/tool_gate_test.c"),
                        str(ROOT / "tests/workbench_monitor_rpc_test.c"),
                        str(json_object), "-lm", "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=60)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=30,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_MONITOR_RPC_PASS " + name, result.stdout)

    def test_strict_fields_boot_tokens_sources_and_types(self):
        self.run_case("validation")

    def test_async_operations_and_view_cas_acknowledgements(self):
        self.run_case("mutations")

    def test_cached_status_current_capture_requested_configs_and_loss_boundaries(self):
        self.run_case("status")

    def test_bounded_binary_read_base64_exact_stats_and_stale_errors(self):
        self.run_case("read")

    def test_every_json_allocation_is_safe_and_precedes_successful_mutation(self):
        self.run_case("oom")

    def test_external_gate_rejects_writes_not_reads_and_releases_on_failure(self):
        self.run_case("gate")


if __name__ == "__main__":
    unittest.main()
