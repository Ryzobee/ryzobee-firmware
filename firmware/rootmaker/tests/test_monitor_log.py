"""Real Monitor tap + stream against the pinned SDK's actual Log V1 format."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "components/ryz_monitor"
LOGGER = ROOT / "components/ryz_log"
IDF = Path(os.environ.get("IDF_PATH", ""))


class MonitorLogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.environ.get("IDF_PATH"):
            raise unittest.SkipTest("Activate ESP-IDF v5.5.4 to set IDF_PATH")
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-monitor-log-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "log"
        cls.flags = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                     "-Werror", "-g", "-fsanitize=address,undefined",
                     "-Dvprintf=monitor_test_vprintf",
                     "-I" + str(ROOT / "tests/monitor_log_stubs"),
                     "-I" + str(ROOT / "tests/monitor_stream_stubs"),
                     "-I" + str(IDF / "components/log/include"),
                     "-I" + str(IDF / "components/esp_common/include"),
                     "-I" + str(MODULE), "-I" + str(MODULE / "include"),
                     "-I" + str(LOGGER / "include")]
        subprocess.run(cls.flags + [str(MODULE / "ryz_monitor_log.c"),
                                   str(LOGGER / "ryz_log.c"),
                                   str(MODULE / "ryz_monitor_stream.c"),
                                   str(ROOT / "tests/monitor_log_test.c"),
                                   "-o", str(cls.binary)], check=True, timeout=30)

    def check_case(self, case):
        result = subprocess.run([str(self.binary), case], text=True, capture_output=True,
                                timeout=20, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("MONITOR_LOG_PASS", result.stdout)

    def test_install_once_and_inactive_original_sink_return_and_arguments(self):
        self.check_case("install")

    def test_unknown_sink_is_not_replaced_or_chained(self):
        self.check_case("ownership")

    def test_all_audited_templates_reconstruct_fixed_labels_and_numeric_values(self):
        self.check_case("allowed")

    def test_unknown_tag_payload_stage_and_format_are_not_copied_into_monitor(self):
        self.check_case("privacy")

    def test_late_clear_stop_and_new_source_do_not_admit_previous_capture(self):
        self.check_case("stale")

    def test_pause_freezes_view_not_capture_and_disabled_tap_still_forwards(self):
        self.check_case("pause")

    def test_reentrant_capture_is_dropped_without_blocking_original_sink(self):
        self.check_case("reentrant")

    def test_unaudited_logger_abi_is_rejected_at_compile_time(self):
        for define in ("CONFIG_LOG_VERSION=2", "CONFIG_LOG_TIMESTAMP_SOURCE_RTOS=0",
                       "CONFIG_LOG_MODE_TEXT=0"):
            with self.subTest(define=define):
                result = subprocess.run(self.flags + ["-D" + define, "-fsyntax-only",
                                                       str(LOGGER / "ryz_log.c")],
                                        text=True, capture_output=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("requires audited IDF", result.stderr)


if __name__ == "__main__":
    unittest.main()
