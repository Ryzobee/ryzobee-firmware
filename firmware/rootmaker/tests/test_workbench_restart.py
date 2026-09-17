"""Real Workbench restart, OTA core and Store; no device or network access."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OTA = ROOT / "components/ryz_ota"
STORE = ROOT / "components/ryz_script_store"
WORKBENCH = ROOT / "components/ryz_workbench"


class WorkbenchRestartTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-restart-build-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "restart"
        flags = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-g", "-pthread", "-fsanitize=address,undefined", "-DESP_ERR_INVALID_VERSION=0x10A"]
        for include in [ROOT / "tests/script_store_stubs", STORE, STORE / "include",
                        OTA, OTA / "include", OTA / "test_host", WORKBENCH]:
            flags += ["-I", str(include)]
        objects = []
        for name, source, definitions in [
            ("store", STORE / "ryz_script_store.c",
             ["-Dryz_script_store_status=restart_real_store_status"]),
            ("ota_platform", OTA / "test_host/fake_platform.c",
             ["-Dryz_ota_platform_reboot=restart_delegate_reboot"]),
        ]:
            output = Path(cls.directory.name) / (name + ".o")
            subprocess.run(flags + definitions + ["-c", str(source), "-o", str(output)],
                           check=True, timeout=30)
            objects.append(str(output))
        subprocess.run(flags + objects + [str(ROOT / "tests/script_store_stubs/script_store_host.c"),
                       str(OTA / "ryz_ota.c"), str(WORKBENCH / "workbench_restart.c"),
                       str(ROOT / "tests/workbench_restart_test.c"), "-o", str(cls.binary)],
                       check=True, timeout=30)

    def run_case(self, case):
        with tempfile.TemporaryDirectory(prefix="ryz-script-restart-") as root:
            result = subprocess.run([str(self.binary), case, root], text=True,
                                    capture_output=True, timeout=15,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_RESTART_PASS " + case, result.stdout)

    def test_active_job_rejects_restart_without_store_access(self):
        self.run_case("busy")

    def test_incomplete_guard_or_wrong_attempt_never_acquires_writer(self):
        self.run_case("arguments")

    def test_reboot_holds_writer_until_unexpected_return_then_releases(self):
        self.run_case("return")

    def test_status_error_and_each_dirty_flag_release_writer_exactly_once(self):
        self.run_case("status")

    def test_real_uninitialized_store_rejects_without_filesystem_io(self):
        self.run_case("uninitialized")

    def test_real_store_reader_lock_contention_rejects_then_explicit_retry_works(self):
        self.run_case("contention")

    def test_real_committed_but_unclean_journal_blocks_without_automatic_recovery(self):
        self.run_case("cleanup")

    def test_real_legacy_backup_blocks_restart_and_is_preserved(self):
        self.run_case("legacy")


if __name__ == "__main__":
    unittest.main()
