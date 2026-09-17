"""Real tool broker -> Monitor worker -> fixed stream; never device evidence."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ToolsMonitorIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-tools-monitor-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "tools_monitor"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-pthread", "-fsanitize=address,undefined", "-g",
                   "-I" + str(ROOT / "tests/monitor_stubs"),
                   "-I" + str(ROOT / "components/ryz_monitor")]
        for component in ("ryz_tools", "ryz_monitor", "ryz_i2c_scan", "ryz_rgb"):
            command += ["-I" + str(ROOT / "components" / component / "include")]
        command += [str(ROOT / "components/ryz_tools/ryz_tools.c"),
                    str(ROOT / "components/ryz_monitor/ryz_monitor.c"),
                    str(ROOT / "components/ryz_monitor/ryz_monitor_stream.c"),
                    str(ROOT / "tests/tools_monitor_integration_test.c"),
                    "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=30)

    def case(self, name):
        result = subprocess.run([str(self.binary), name], text=True, capture_output=True,
                                timeout=15, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("TOOLS_MONITOR_PASS " + name, result.stdout)

    def test_app_claim_start_and_binary_pause_clear_read_use_real_stream(self):
        self.case("claim_start_stream")

    def test_close_supersedes_inflight_candidate_and_late_begin_cannot_reopen(self):
        self.case("close_candidate")

    def test_close_supersedes_inflight_restore_and_old_source_stays_closed(self):
        self.case("close_restore")

    def test_config_only_job_never_stops_or_adopts_historical_receive_session(self):
        self.case("config_only_history")

    def test_failed_cleanup_retains_old_owner_until_explicit_retry_and_isolates_new_job(self):
        self.case("cleanup_failure_isolation")


if __name__ == "__main__":
    unittest.main()
