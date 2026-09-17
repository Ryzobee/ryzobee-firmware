"""Real network RPC + locked IDF cJSON; only system-service outcomes are faked."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class WorkbenchNetworkRpcTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise RuntimeError("Set IDF_PATH to the project's ESP-IDF 5.5.4 checkout")
        json = Path(idf) / "components/json/cJSON"
        if not (json / "cJSON.c").is_file():
            raise RuntimeError("IDF_PATH does not contain cJSON sources")
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-network-rpc-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "network_rpc_test"
        json_object = Path(cls.directory.name) / "cJSON.o"
        compiler = os.environ.get("CC", "cc")
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-g"]
        # Only the pinned third-party object gets the deprecated sprintf waiver.
        subprocess.run([compiler, *flags, "-Wno-deprecated-declarations", "-I", str(json),
                        "-c", str(json / "cJSON.c"), "-o", str(json_object)], check=True, timeout=60)
        workbench = ROOT / "components/ryz_workbench"
        command = [compiler, *flags]
        for include in [ROOT / "tests/workbench_network_rpc_stubs", json, workbench,
                        *[ROOT / "components" / name / "include" for name in
                          ("ryz_system_services", "ryz_provisioning", "ryz_time")]]:
            command.extend(["-I", str(include)])
        command.extend([str(workbench / "workbench_network_rpc.c"),
                        str(ROOT / "tests/workbench_network_rpc_test.c"),
                        str(json_object), "-lm", "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=60)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=30,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_NETWORK_RPC_PASS " + case, result.stdout)

    def test_strict_fields_types_boot_and_confirmation(self):
        self.run_case("validation")

    def test_mutations_acknowledge_only_one_admitted_intent(self):
        self.run_case("mutation")

    def test_status_preserves_operation_history_and_never_serializes_ap_secrets(self):
        self.run_case("status")

    def test_service_errors_and_unknown_enums_never_look_healthy(self):
        self.run_case("errors")

    def test_every_json_allocation_failure_is_safe_and_success_admission_is_last(self):
        self.run_case("oom")


if __name__ == "__main__":
    unittest.main()
