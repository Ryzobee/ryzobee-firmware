"""Actual Lua/Job/broker/scanner; controlled OS/bus, no device or fake PASS."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AppToolsI2cIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-lua-tools-i2c-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "app_tools_i2c"
        json = Path(os.environ["IDF_PATH"]) / "components/json/cJSON"
        flags = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                 "-pthread", "-fsanitize=address,undefined", "-g"]
        json_object = Path(cls.directory.name) / "json.o"
        subprocess.run(flags + ["-Wno-deprecated-declarations", "-I", str(json), "-c",
                       str(json / "cJSON.c"), "-o", str(json_object)], check=True, timeout=30)
        lua = ROOT / "managed_components/georgik__lua"
        command = flags + ["-DMAKE_LIB", "-include", str(ROOT / "host/sdkconfig.h")]
        for directory in [ROOT / "tests/i2c_scan_stubs", ROOT / "components/ryz_i2c_scan",
                          ROOT / "components/ryz_workbench", lua / "include", lua / "lua", json]:
            command += ["-I", str(directory)]
        for component in ("ryz_runtime", "ryz_lvgl", "ryz_tools", "ryz_i2c_scan", "ryz_rgb",
                          "ryz_monitor", "ryz_tool_pins"):
            command += ["-I", str(ROOT / "components" / component / "include")]
        command += [str(ROOT / path) for path in (
            "components/ryz_runtime/app_runtime.c", "components/ryz_runtime/app_tools.c",
            "components/ryz_workbench/workbench_tools.c", "components/ryz_tools/ryz_tools.c",
            "components/ryz_i2c_scan/ryz_i2c_scan.c", "tests/app_tools_i2c_integration_test.c")]
        command += [str(lua / "lua/onelua.c"), str(json_object), "-lm", "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=60)

    def case(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=15,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("APP_TOOLS_I2C_PASS", result.stdout)

    def test_normal_return_retires_actual_inflight_scan_after_vm_is_freed(self):
        self.case("done")

    def test_lua_error_does_not_drop_accepted_native_ownership(self):
        self.case("runtime")

    def test_lua_deadline_closes_actual_worker_even_without_script_cleanup(self):
        self.case("timeout")

    def test_cancellation_after_admission_cannot_escape_c_cleanup(self):
        self.case("stopped")

    def test_post_admission_lua_allocation_failure_preserves_c_cleanup_identity(self):
        self.case("memory")

    def test_failed_cleanup_survives_destroyed_job_until_explicit_original_session_retry(self):
        self.case("failed_cleanup")

    def test_ordinary_application_creates_no_tool_session_or_worker(self):
        self.case("unused")


if __name__ == "__main__":
    unittest.main()
