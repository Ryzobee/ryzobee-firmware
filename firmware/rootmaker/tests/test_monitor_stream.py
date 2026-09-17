"""Real fixed-storage Monitor stream; no SDK or device operations."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "components/ryz_monitor"


class MonitorStreamTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-monitor-stream-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "stream"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-pthread", "-g", "-fsanitize=address,undefined",
                        "-I" + str(ROOT / "tests/monitor_stream_stubs"),
                        "-I" + str(MODULE), "-I" + str(MODULE / "include"),
                        str(MODULE / "ryz_monitor_stream.c"),
                        str(ROOT / "tests/monitor_stream_test.c"), "-o", str(cls.binary)],
                       check=True, timeout=30)

    def check_case(self, case):
        result = subprocess.run([str(self.binary), case], text=True, capture_output=True,
                                timeout=20, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("MONITOR_STREAM_PASS", result.stdout)

    def test_no_implicit_start_invalid_values_and_failure_outputs_are_cleared(self):
        self.check_case("invalid")

    def test_binary_chunks_bounded_retention_and_non_consuming_cursor_gap(self):
        self.check_case("retention")

    def test_pause_is_a_frozen_copy_while_live_reception_keeps_overwriting(self):
        self.check_case("pause")

    def test_clear_expires_inflight_tokens_both_views_and_old_cursors(self):
        self.check_case("clear")

    def test_source_switch_and_new_session_reject_old_logger_and_uart_tokens(self):
        self.check_case("sources")

    def test_filtered_and_truncated_bytes_are_not_invented_physical_loss_counts(self):
        self.check_case("counts")

    def test_empty_paused_view_rejects_future_live_cursor(self):
        self.check_case("empty_pause")

    def test_concurrent_producers_and_cached_readers_never_tear_binary_records(self):
        self.check_case("concurrency")


if __name__ == "__main__":
    unittest.main()
