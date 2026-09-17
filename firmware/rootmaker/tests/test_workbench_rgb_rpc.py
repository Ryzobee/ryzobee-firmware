"""Real RGB JSON Adapter and locked cJSON; deterministic worker outcomes only."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKBENCH = ROOT / "components" / "ryz_workbench"


class WorkbenchRgbRpcHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise RuntimeError("Set IDF_PATH to the project's ESP-IDF 5.5.4 checkout")
        json = Path(idf) / "components" / "json" / "cJSON"
        if not (json / "cJSON.c").is_file():
            raise RuntimeError("IDF_PATH does not contain cJSON sources")
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-rgb-rpc-build-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "rgb_rpc_test"
        json_object = Path(cls.directory.name) / "cJSON.o"
        compiler = os.environ.get("CC", "cc")
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-g"]
        # The pinned dependency uses macOS-deprecated sprintf; only that
        # object's warning is relaxed. Project code retains strict warnings.
        subprocess.run([compiler, *flags, "-Wno-deprecated-declarations",
                        "-I", str(json), "-c", str(json / "cJSON.c"),
                        "-o", str(json_object)], check=True, timeout=60)
        command = [compiler, *flags, "-DRYZ_TEST_TOOL=RYZ_TOOL_RGB"]
        for include in [ROOT / "tests" / "workbench_rgb_rpc_stubs",
                        WORKBENCH, ROOT / "components" / "ryz_rgb" / "include", json,
                        ROOT / "tests/workbench_tool_gate_stubs",
                        *[ROOT / "components" / name / "include" for name in ("ryz_tools", "ryz_i2c_scan", "ryz_monitor")]]:
            command.extend(["-I", str(include)])
        command.extend([str(WORKBENCH / "workbench_rgb_rpc.c"),
                        str(ROOT / "tests/workbench_tool_gate_stubs/tool_gate_test.c"),
                        str(ROOT / "tests" / "workbench_rgb_rpc_test.c"),
                        str(json_object), "-lm", "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=60)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=30,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_RGB_RPC_PASS " + name, result.stdout)

    def test_strict_channels_fields_boot_and_types(self):
        self.run_case("validation")

    def test_raw_rgb_and_off_are_single_request_acknowledgements(self):
        self.run_case("submit")

    def test_cached_status_distinguishes_history_unknown_output_and_errors(self):
        self.run_case("status")

    def test_each_json_allocation_failure_is_safe_and_admission_is_last(self):
        self.run_case("oom")

    def test_cleanup_is_token_bound_without_a_color_send(self):
        self.run_case("cleanup")

    def test_external_gate_rejects_writes_not_reads_and_releases_on_failure(self):
        self.run_case("gate")


if __name__ == "__main__":
    unittest.main()
