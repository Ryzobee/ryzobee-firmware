"""Host checks for the production borrowed-call channel, not ESP task evidence."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "ryz_workbench"


class WorkbenchIoHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-workbench-io-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "workbench_io_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                   "-Werror", "-pedantic", "-pthread",
                   "-fsanitize=address,undefined", "-g", "-I", str(COMPONENT),
                   str(COMPONENT / "workbench_io.c"),
                   str(ROOT / "tests/workbench_io_test.c"),
                   "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_IO_PASS " + name, result.stdout)

    def test_invalid_calls_and_empty_dispatch_are_safe(self):
        self.run_case("invalid")

    def test_concurrent_producers_have_exactly_one_owner(self):
        self.run_case("producers")

    def test_cancel_cannot_release_executing_borrowed_storage(self):
        self.run_case("borrowed_cancel")

    def test_done_remains_busy_until_matching_acknowledgement(self):
        self.run_case("done_ack")

    def test_stale_ticket_cannot_acknowledge_a_later_operation(self):
        self.run_case("stale_ticket")

    def test_cleanup_is_dispatched_even_after_cancellation(self):
        self.run_case("cleanup")

    def test_multiple_rounds_publish_payload_and_never_replay_old_calls(self):
        self.run_case("visibility")

    def test_ticket_wrap_skips_reserved_zero(self):
        self.run_case("wrap")


if __name__ == "__main__":
    unittest.main()
