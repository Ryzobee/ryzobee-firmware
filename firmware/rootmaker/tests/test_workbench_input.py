"""Exercise the production owner-only input buffer with real BSP sample types."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "ryz_workbench"


class WorkbenchInputHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-workbench-input-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "workbench_input_test"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-I", str(ROOT / "tests" / "workbench_input_stubs"),
                "-I", str(ROOT / "components" / "ryz_board" / "include"),
                "-I", str(COMPONENT),
                str(COMPONENT / "workbench_input.c"),
                str(ROOT / "components" / "ryz_board" / "touch_protocol.c"),
                str(ROOT / "tests" / "workbench_input_test.c"),
                "-o", str(cls.binary),
            ],
            check=True,
        )

    def run_case(self, case):
        result = subprocess.run(
            [str(self.binary), case], capture_output=True, text=True, check=False
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_INPUT_PASS", result.stdout)

    def test_reset_requires_fresh_sample_and_release(self):
        self.run_case("reset")

    def test_order_move_coalescing_and_none_fallback(self):
        self.run_case("fifo")

    def test_ring_wrap_and_full_tail_move(self):
        self.run_case("wrap")

    def test_overflow_reports_once_and_quarantines_old_finger(self):
        self.run_case("overflow")

    def test_app_timeout_preserves_active_gesture_until_physical_recovery(self):
        self.run_case("error")

    def test_release_before_read_cannot_hide_overflow(self):
        self.run_case("overflow_release")

    def test_error_precedence_and_drop_counter_saturation(self):
        self.run_case("error_overflow")

    def test_normalized_physical_samples_across_apps(self):
        self.run_case("protocol")

    def test_current_sample_does_not_queue_missed_taps_or_overflow(self):
        self.run_case("current_no_queue")

    def test_current_events_follow_consumer_reads_not_physical_edges(self):
        self.run_case("current_events")

    def test_current_sample_errors_and_reset_clear_consumer_history(self):
        self.run_case("current_history")

    def test_mode_selection_clears_consumption_epoch_not_release_guard(self):
        self.run_case("mode_epoch")

    def test_suppressed_boot_release_is_valid_neutral_before_lua_starts(self):
        self.run_case("suppressed_autostart")

    def test_policy_cancels_delivered_contact_once_without_up_or_click(self):
        self.run_case("suppressed_contact")

    def test_cancel_survives_slow_reader_and_precedes_next_real_contact(self):
        self.run_case("suppressed_slow_reader")

    def test_policy_preserves_physical_errors_and_timeout_exhaustion(self):
        self.run_case("suppressed_errors")


if __name__ == "__main__":
    unittest.main()
