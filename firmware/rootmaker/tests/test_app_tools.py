"""Real shared Lua facade and locked 32-bit Lua; controlled trusted tool Adapter."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "components/ryz_runtime"


class AppToolsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-app-tools-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "app_tools_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-DMAKE_LIB", "-include", str(ROOT / "host/sdkconfig.h")]
        for include in [ROOT / "host", RUNTIME, RUNTIME / "include", ROOT / "components/ryz_lvgl/include",
                        *[ROOT / "components" / name / "include" for name in
                          ("ryz_tools", "ryz_i2c_scan", "ryz_rgb", "ryz_monitor")],
                        ROOT / "managed_components/georgik__lua/include", ROOT / "managed_components/georgik__lua/lua"]:
            command.extend(["-I", str(include)])
        command.extend([str(ROOT / "tests/app_tools_test.c"), str(RUNTIME / "app_runtime.c"),
                        str(RUNTIME / "app_tools.c"), str(ROOT / "managed_components/georgik__lua/lua/onelua.c"),
                        "-lm", "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=60)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=30,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("APP_TOOLS_PASS " + name, result.stdout)

    def test_module_allowlist_and_null_adapter_are_safe(self):
        self.run_case("module")

    def test_all_thirteen_actions_exact_request_mapping_and_u32_strings(self):
        self.run_case("actions")

    def test_strict_tokens_tables_types_and_no_metatable_execution(self):
        self.run_case("validation")

    def test_structured_callback_codes_keep_expected_failure_recoverable(self):
        self.run_case("errors")

    def test_full_bounded_status_preserves_history_exact_counters_and_unknowns(self):
        self.run_case("status")

    def test_monitor_binary_nul_bytes_pagination_and_malformed_reply_bounds(self):
        self.run_case("read")

    def test_cancel_and_budget_checks_before_and_after_admission(self):
        self.run_case("cancel")

    def test_finalizer_registration_rejected_without_losing_c_owned_cleanup(self):
        self.run_case("finalizer")

    def test_ordinary_metatables_and_hook_controlled_close_keep_tool_calls(self):
        self.run_case("metatable")

    def test_explicit_cancel_and_stop_keep_original_tokens_and_c_cleanup(self):
        self.run_case("explicit_cleanup")

    def test_vm_oom_before_and_after_admission_records_ownership_for_cleanup(self):
        self.run_case("oom")


if __name__ == "__main__":
    unittest.main()
