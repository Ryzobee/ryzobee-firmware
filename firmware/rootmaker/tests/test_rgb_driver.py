"""Real RMT driver + controlled IDF calls, never board/waveform evidence."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RGB = ROOT / "components/ryz_rgb"


class RgbDriverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="ryz-rgb-driver-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "driver"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-g",
                        "-I", str(ROOT / "tests/rgb_driver_stubs"),
                        "-I", str(RGB), "-I", str(RGB / "include"),
                        str(RGB / "ryz_rgb_driver.c"), str(ROOT / "tests/rgb_driver_test.c"),
                        "-o", str(cls.binary)], check=True, timeout=30)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True,
                                text=True, timeout=15,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RGB_DRIVER_PASS " + case, result.stdout)

    def test_1024_colors_have_grb_msb_symbols_and_both_reset_segments(self):
        self.run_case("waveform")

    def test_failed_channel_allocation_retries_without_using_a_handle(self):
        self.run_case("partial_channel")

    def test_failed_encoder_allocation_keeps_only_one_channel(self):
        self.run_case("partial_encoder")

    def test_mismatched_or_unreadable_clock_never_transmits(self):
        self.run_case("wrong_clock")

    def test_enable_or_queue_error_does_not_claim_a_completed_frame(self):
        self.run_case("admission")

    def test_stopped_timed_out_frame_is_not_success_and_next_black_is_a_real_frame(self):
        self.run_case("timeout_stopped")

    def test_failed_stop_retains_live_payload_until_retry_reclaims(self):
        self.run_case("timeout_disable_failure")

    def test_failed_reclaim_retains_payload_without_reenable_or_overwrite(self):
        self.run_case("timeout_reclaim_failure")

    def test_frame_completion_without_disable_success_still_returns_failure(self):
        self.run_case("completed_disable_failure")

    def test_permanent_disable_failure_never_reenables_or_claims_black_success(self):
        self.run_case("permanent_disable_failure")

    def test_cleanup_releases_without_sending_or_allocating_and_can_reopen(self):
        self.run_case("cleanup_complete")

    def test_cleanup_handles_no_allocation_and_partial_encoder_allocation(self):
        self.run_case("cleanup_empty_partial")

    def test_cleanup_drains_failed_write_without_resending(self):
        self.run_case("cleanup_timeout")

    def test_cleanup_drain_failure_retains_payload_then_explicit_retry_releases(self):
        self.run_case("cleanup_drain_retry")

    def test_encoder_delete_failure_is_retryable_and_blocks_unsafe_reuse(self):
        self.run_case("cleanup_encoder_retry")

    def test_channel_delete_failure_keeps_remaining_handle_without_double_deleting_encoder(self):
        self.run_case("cleanup_channel_retry")

    def test_cleanup_permanent_stop_failure_never_deletes_or_sends_black(self):
        self.run_case("cleanup_permanent_stop_failure")

    def test_only_original_cpu1_pinned_worker_can_write_or_cleanup(self):
        self.run_case("owner_guard")


if __name__ == "__main__":
    unittest.main()
