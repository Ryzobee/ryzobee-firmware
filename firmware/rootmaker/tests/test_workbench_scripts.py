"""Real Scripts controller; explicit reader/Store/write-guard service substitutes.

No LVGL rendering, filesystem, FreeRTOS or device behavior is claimed here.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WorkbenchScriptsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-wb-scripts-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "scripts-test"
        includes = [ROOT / "tests/workbench_scripts_stubs"]
        for name in ("ryz_workbench", "ryz_system_ui", "ryz_apps",
                     "ryz_script_store", "ryz_script_metadata"):
            includes += [ROOT / "components" / name, ROOT / "components" / name / "include"]
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
                   "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined"]
        command += [part for path in includes for part in ("-I", str(path))]
        command += [str(ROOT / "tests/workbench_scripts_test.c"),
                    str(ROOT / "components/ryz_workbench/workbench_scripts.c"),
                    "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=60)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_SCRIPTS_PASS " + name, result.stdout)

    def test_catalog_window_and_selected_source_survive_pixel_scroll(self):
        for name in ("catalog", "selection"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_boot_identity_missing_stale_and_bounded_read_retry(self):
        for name in ("boot-none", "boot-file", "boot-stale", "boot-retry", "boot-retry-bound"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_only_clear_and_copy_are_admitted_with_exact_identity_and_write_guard(self):
        for name in ("clear", "copy", "strict-fields", "invalid-source", "publication-busy"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_missing_or_denied_guard_and_revision_change_never_reach_store_writer(self):
        for operation in ("clear", "copy"):
            for reason in ("no-guard", "no-begin", "no-end", "busy", "revision"):
                with self.subTest(operation=operation, reason=reason):
                    self.run_case(operation + "-" + reason)

    def test_unaccepted_request_is_revoked_on_close_or_new_view_even_if_publication_busy(self):
        for operation in ("clear", "copy"):
            for reason in ("close", "new-page", "close-publication"):
                with self.subTest(operation=operation, reason=reason):
                    self.run_case(operation + "-" + reason)

    def test_known_failure_unknown_cleanup_and_absence_never_retry_automatically(self):
        for operation in ("clear", "copy"):
            for reason in ("failed", "unknown", "cleanup", "missing"):
                with self.subTest(operation=operation, reason=reason):
                    self.run_case(operation + "-" + reason)

    def test_accepted_write_survives_close_without_late_navigation_or_reexecution(self):
        for operation in ("clear", "copy"):
            with self.subTest(operation=operation):
                self.run_case(operation + "-inflight")


if __name__ == "__main__":
    unittest.main()
