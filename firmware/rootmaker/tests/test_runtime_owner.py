"""Real Lua runtimes through the owner channel; BSP/LVGL spies are not pixels."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RuntimeOwnerHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-runtime-owner-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "runtime_owner_test"
        runtime = ROOT / "components" / "ryz_runtime"
        workbench = ROOT / "components" / "ryz_workbench"
        lua = ROOT / "managed_components" / "georgik__lua"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-pthread", "-DMAKE_LIB",
                "-include", str(ROOT / "host" / "sdkconfig.h"),
                "-I", str(ROOT / "tests" / "runtime_owner_stubs"),
                "-I", str(runtime / "include"),
                "-I", str(workbench),
                "-I", str(ROOT / "components" / "ryz_board" / "include"),
                "-I", str(ROOT / "components" / "ryz_lvgl" / "include"),
                *[flag for name in ("ryz_tools", "ryz_i2c_scan", "ryz_rgb", "ryz_monitor")
                  for flag in ("-I", str(ROOT / "components" / name / "include"))],
                "-I", str(lua / "include"), "-I", str(lua / "lua"),
                str(ROOT / "tests" / "runtime_owner_test.c"),
                str(runtime / "lua_runtime.c"),
                str(runtime / "app_runtime.c"),
                str(runtime / "app_tools.c"),
                str(runtime / "nervous_runtime.c"),
                str(runtime / "ryz_runtime_io.c"),
                str(workbench / "workbench_io.c"),
                str(ROOT / "components" / "ryz_board" / "touch_protocol.c"),
                str(lua / "lua" / "onelua.c"),
                "-lm", "-o", str(cls.binary),
            ],
            check=True,
        )

    def run_case(self, case, timeout=10):
        arguments = [case] if isinstance(case, str) else case
        result = subprocess.run(
            [str(self.binary), *arguments], capture_output=True, text=True,
            check=False, timeout=timeout,
            env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"},
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RUNTIME_OWNER_PASS", result.stdout)

    def test_app_rejects_infinite_gc_registration_before_vm_close(self):
        self.run_case("finalizer_gc_app", timeout=2)

    def test_legacy_coroutine_cannot_swallow_timeout(self):
        self.run_case("legacy_coroutine_timeout", timeout=2)

    def test_legacy_coroutine_cannot_swallow_cancel(self):
        self.run_case("legacy_coroutine_cancel", timeout=2)

    def test_public_peripherals_in_both_dialects_cleanup_after_all_job_outcomes(self):
        self.run_case("peripheral_public_both")

    def test_legacy_rejects_infinite_gc_registration_before_vm_close(self):
        self.run_case("finalizer_gc_legacy", timeout=2)

    def finalizer_case(self, case):
        # Each dialect is an isolated process. A hung finalizer is a test
        # failure; subprocess.run kills and reaps it, not a claimed Lua exit.
        for dialect in ("app", "legacy"):
            with self.subTest(dialect=dialect):
                self.run_case(["finalizer", dialect, case], timeout=2)

    def test_gc_rejects_false_callable_and_shared_or_reinstalled_metatables(self):
        self.finalizer_case("registration")

    def test_delayed_gc_without_registration_and_raw_metatable_lookup_are_preserved(self):
        self.finalizer_case("delayed")

    def test_normal_metatable_identity_mutation_weakness_and_protection_are_preserved(self):
        self.finalizer_case("metatables")

    def test_close_runs_on_normal_return_and_error_in_reverse_order(self):
        self.finalizer_case("close_normal")

    def test_close_loops_are_hooked_after_normal_error_timeout_oom_and_nested_unwind(self):
        self.finalizer_case("close_timeout")

    def test_close_loops_and_string_close_remain_cancellable_during_unwind(self):
        self.finalizer_case("close_cancel")

    def test_legacy_display_touch_and_info_use_owner(self):
        self.run_case("legacy")

    def test_app_raw_display_and_input_use_owner(self):
        self.run_case("app_raw")

    def test_policy_cancel_is_visible_without_a_fake_up_in_both_lua_modes(self):
        self.run_case("app_touch_interrupted")
        self.run_case("legacy_touch_interrupted")

    def test_app_scene_pump_close_and_cleanup_use_owner(self):
        self.run_case("app_ui")

    def test_app_update_uses_owner_without_remount_or_changing_generation(self):
        self.run_case("app_update")

    def test_invalid_update_batch_never_reaches_owner_renderer(self):
        self.run_case("app_update_invalid")

    def test_cancel_queued_update_skips_renderer_but_still_acknowledges_and_closes(self):
        self.run_case("app_update_pending_cancel")

    def test_cancel_after_update_ack_closes_before_vm_teardown(self):
        self.run_case("app_update_post_cancel")

    def test_deadline_inside_update_keeps_candidate_alive_until_owner_ack(self):
        self.run_case("app_update_timeout")

    def test_update_driver_timeout_is_runtime_error_not_user_cancellation(self):
        self.run_case("app_update_driver_error")

    def test_neuro_sample_paint_and_stop_cleanup_use_owner(self):
        self.run_case("neuro")

    def test_owner_progresses_during_legacy_compute_and_stops(self):
        self.run_case("legacy_spin")

    def test_owner_progresses_during_app_compute_and_stops(self):
        self.run_case("app_spin")

    def test_mounted_scene_closes_before_vm_teardown_on_stop(self):
        self.run_case("ui_stop")

    def test_mounted_scene_closes_before_vm_teardown_on_timeout(self):
        self.run_case("ui_timeout")

    def test_legacy_timeout_inside_display_still_acks_and_cleans_up(self):
        self.run_case("show_timeout")

    def test_cancel_queued_neuro_paint_preserves_stop_phase(self):
        self.run_case("neuro_pending_stop")

    def test_timeout_queued_neuro_paint_preserves_timeout_phase(self):
        self.run_case("neuro_pending_timeout")

    def test_cancel_queued_neuro_sample_preserves_stop_phase(self):
        self.run_case("neuro_sample_stop")

    def test_cancel_queued_neuro_continuation_drains_and_preserves_stop(self):
        self.run_case("neuro_stripe_stop")

    def test_app_native_deadline_and_core_deadline_cannot_disagree(self):
        self.run_case("app_deadline_skew")

    def test_legacy_hardware_timeout_remains_native_error(self):
        self.run_case("legacy_touch_error")

    def test_app_hardware_timeout_remains_native_error(self):
        self.run_case("app_show_error")

    def test_neuro_hardware_failure_remains_native_error(self):
        self.run_case("neuro_paint_error")

    def test_app_tool_callback_forwards_on_worker_with_original_context_and_exact_identity(self):
        self.run_case("app_tools")

    def test_app_without_tool_callback_is_unavailable_without_bootstrap_or_hardware(self):
        self.run_case("app_tools_unavailable")

    def test_invalid_tool_callback_result_is_failed_and_reply_is_not_accepted(self):
        self.run_case("app_tools_bad_result")

    def test_cancel_after_tool_admission_preserves_stop_and_trusted_identity(self):
        self.run_case("app_tools_post_cancel")


if __name__ == "__main__":
    unittest.main()
