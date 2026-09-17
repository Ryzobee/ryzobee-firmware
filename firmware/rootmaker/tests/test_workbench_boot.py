"""Real boot decision and Job admission; SDK cJSON and real source SHA, no board."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPONENTS = ROOT / "components"
WORKBENCH = COMPONENTS / "ryz_workbench"
STORE = COMPONENTS / "ryz_script_store"
STUBS = ROOT / "tests" / "script_store_stubs"


class WorkbenchBootHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise RuntimeError("Set IDF_PATH to the project's ESP-IDF 5.5.4 checkout")
        json = Path(idf) / "components" / "json" / "cJSON"
        if not (json / "cJSON.c").is_file():
            raise RuntimeError("IDF_PATH does not contain cJSON sources")
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-boot-test-build-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "workbench_boot_test"
        json_object = Path(cls.directory.name) / "cJSON.o"
        flags = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
                 "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined"]
        # Only the pinned third-party cJSON object suppresses macOS SDK's
        # deprecated sprintf warning. Project sources retain strict warnings.
        subprocess.run([*flags, "-Wno-deprecated-declarations", "-I", str(json),
                        "-c", str(json / "cJSON.c"), "-o", str(json_object)],
                       check=True, timeout=60)
        command = [*flags, "-pthread"]
        includes = [STUBS, STORE, STORE / "include", WORKBENCH, json]
        includes.extend(COMPONENTS / name / "include" for name in [
            "ryz_runtime", "ryz_tools", "ryz_i2c_scan", "ryz_rgb", "ryz_monitor"])
        for include in includes:
            command.extend(["-I", str(include)])
        command.extend([str(WORKBENCH / "workbench_boot.c"),
                        str(WORKBENCH / "workbench_job_start.c"),
                        str(STORE / "ryz_script_store.c"),
                        str(STUBS / "script_store_host.c"),
                        str(ROOT / "tests" / "workbench_boot_test.c"),
                        str(json_object), "-lm", "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=60)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=15,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_BOOT_PASS " + name, result.stdout)

    def test_failed_confirmation_cannot_enqueue_a_job(self):
        self.run_case("failed_confirmation")

    def test_each_required_initialization_gate_blocks_both_admission_modes(self):
        self.run_case("gates")

    def test_confirmation_retries_after_exactly_five_seconds_without_opening_runtime(self):
        self.run_case("retry")

    def test_confirmed_offline_ready_services_remain_ready_during_lua_display_lease(self):
        self.run_case("confirmed")


if __name__ == "__main__":
    unittest.main()
