"""Real scan JSON Adapter + locked IDF cJSON; scanner outcomes, no I2C."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKBENCH = ROOT / "components" / "ryz_workbench"
SCAN = ROOT / "components" / "ryz_i2c_scan"


class WorkbenchI2cRpcHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise RuntimeError("Set IDF_PATH to the project's ESP-IDF 5.5.4 checkout")
        json = Path(idf) / "components" / "json" / "cJSON"
        if not (json / "cJSON.c").is_file():
            raise RuntimeError("IDF_PATH does not contain cJSON sources")
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-i2c-rpc-build-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "i2c_rpc_test"
        json_object = Path(cls.directory.name) / "cJSON.o"
        compiler = os.environ.get("CC", "cc")
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-g"]
        # This pinned dependency uses macOS-deprecated sprintf. Only its object
        # relaxes that warning; the production Adapter/test keep strict flags.
        subprocess.run([compiler, *flags, "-Wno-deprecated-declarations",
                        "-I", str(json), "-c", str(json / "cJSON.c"),
                        "-o", str(json_object)], check=True, timeout=60)
        command = [compiler, *flags, "-DRYZ_TEST_TOOL=RYZ_TOOL_I2C"]
        for include in [ROOT / "tests" / "workbench_i2c_rpc_stubs",
                        WORKBENCH, SCAN / "include", json,
                        ROOT / "tests/workbench_tool_gate_stubs",
                        *[ROOT / "components" / name / "include" for name in ("ryz_tools", "ryz_rgb", "ryz_monitor")]]:
            command.extend(["-I", str(include)])
        command.extend([str(WORKBENCH / "workbench_i2c_rpc.c"),
                        str(ROOT / "tests/workbench_tool_gate_stubs/tool_gate_test.c"),
                        str(ROOT / "tests" / "workbench_i2c_rpc_test.c"),
                        str(json_object), "-lm", "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=60)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=30,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_I2C_RPC_PASS " + name, result.stdout)

    def test_strict_fields_types_boot_and_cancel_token(self):
        self.run_case("validation")

    def test_start_and_cancel_are_versioned_request_acknowledgements(self):
        self.run_case("mutations")

    def test_cached_status_preserves_result_classes_and_exact_times(self):
        self.run_case("status")

    def test_only_complete_conclusive_nacks_mean_empty(self):
        self.run_case("empty")

    def test_every_mutation_ack_allocation_precedes_side_effect(self):
        self.run_case("mutation_oom")

    def test_every_status_and_failure_reply_allocation_is_cleaned(self):
        self.run_case("status_oom")

    def test_v2_configuration_cas_validation_and_historical_scan_identity(self):
        self.run_case("configuration")

    def test_each_configuration_ack_allocation_precedes_software_commit(self):
        self.run_case("configure_oom")

    def test_external_gate_rejects_writes_not_reads_and_releases_on_failure(self):
        self.run_case("gate")


if __name__ == "__main__":
    unittest.main()
