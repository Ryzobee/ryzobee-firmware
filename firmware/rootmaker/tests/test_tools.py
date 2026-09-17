"""Actual per-job tool broker; native cores are typed state/admission Adapters."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ToolsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-tools-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "tools"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-pthread", "-fsanitize=address,undefined", "-g",
                   "-I" + str(ROOT / "tests/monitor_stubs")]
        for module in ("ryz_tools", "ryz_i2c_scan", "ryz_rgb", "ryz_monitor"):
            command += ["-I" + str(ROOT / "components" / module / "include")]
        command += [str(ROOT / "components/ryz_tools/ryz_tools.c"),
                    str(ROOT / "tests/tools_test.c"), "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=30)

    def case(self, name):
        result = subprocess.run([str(self.binary), name], text=True, capture_output=True,
                                timeout=15, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("TOOLS_PASS", result.stdout)

    def test_open_and_invalid_requests_do_not_start_or_claim_hardware(self):
        self.case("lazy")

    def test_active_external_tool_cannot_be_adopted_or_cancelled_by_lua(self):
        self.case("external_active")

    def test_lua_lease_covers_configure_to_start_and_blocks_only_owned_tool(self):
        self.case("lease")

    def test_native_tokens_are_recorded_before_reply_and_old_cancel_is_rejected(self):
        self.case("tokens")

    def test_async_close_requires_acknowledged_release_and_revokes_calls(self):
        self.case("close")

    def test_failed_cleanup_isolated_from_other_tools_and_explicit_retry(self):
        self.case("failed")

    def test_scan_completing_between_snapshot_and_cancel_does_not_quarantine(self):
        self.case("completion_race")

    def test_rejected_cancel_recheck_never_releases_retained_resources(self):
        self.case("race_held")

    def test_rejected_cancel_recheck_never_adopts_replacement_identity(self):
        self.case("race_replaced")

    def test_rejected_cancel_recheck_contention_remains_pending_without_resubmit(self):
        self.case("race_busy")

    def test_unknown_terminal_phase_is_not_proof_of_resource_release(self):
        self.case("race_unknown")

    def test_old_session_close_cannot_touch_reused_slots_or_new_current_session(self):
        self.case("stale")

    def test_monitor_config_only_never_adopts_or_stops_historical_session(self):
        self.case("monitor_config")

    def test_monitor_control_requires_own_receive_id_not_configuration_operation_id(self):
        self.case("monitor_tokens")

    def test_healthy_parked_rgb_can_be_reused_but_status_only_does_not_cleanup_it(self):
        self.case("rgb_parked")

    def test_unexpected_native_identity_is_quarantined_without_cancelling_it(self):
        self.case("identity_fault")

    def test_core_error_and_gate_contention_clear_outputs_without_implicit_retry(self):
        self.case("errors")

    def test_external_check_and_mutation_remain_exclusive_across_real_threads(self):
        self.case("concurrency")


if __name__ == "__main__":
    unittest.main()
