"""Real Apps task/reader, Store and metadata, with controlled pthread scheduling.

POSIX temporary files/CommonCrypto are Host evidence, not SPIFFS/FreeRTOS or UI.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components" / "ryz_apps"
STORE = ROOT / "components" / "ryz_script_store"
METADATA = ROOT / "components" / "ryz_script_metadata"
STORE_HOST = ROOT / "tests" / "script_store_stubs"


class AppsHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-script-apps-build-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "apps_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                   "-Wall", "-Wextra", "-Werror", "-pthread",
                   "-fsanitize=address,undefined", "-g"]
        for include in [STORE_HOST, APPS, APPS / "include", STORE,
                        STORE / "include", METADATA / "include"]:
            command.extend(["-I", str(include)])
        command.extend([str(APPS / "ryz_apps.c"),
                        str(APPS / "ryz_apps_reader.c"),
                        str(STORE / "ryz_script_store.c"),
                        str(METADATA / "ryz_script_metadata.c"),
                        str(STORE_HOST / "script_store_host.c"),
                        str(ROOT / "tests" / "apps_test.c"), "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=60)

    def run_case(self, name):
        with tempfile.TemporaryDirectory(prefix="ryz-script-apps-data-") as root:
            result = subprocess.run(
                [str(self.binary), name, root], capture_output=True, text=True,
                timeout=30,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("APPS_PASS " + name, result.stdout)

    def test_start_failure_retry_concurrent_start_and_unready_reader(self):
        self.run_case("start")

    def test_argument_output_clearing_empty_and_explicit_offset_error(self):
        self.run_case("arguments")

    def test_sorted_pages_copy_cache_revision_refresh_and_deleted_page_fallback(self):
        self.run_case("catalog")

    def test_deleted_last_page_at_total_falls_back_and_retains_new_offset(self):
        self.run_case("page_boundary")

    def test_detail_metadata_raw_invalid_identity_and_revision_revocation(self):
        self.run_case("detail")

    def test_detail_metadata_and_sha_are_from_one_owned_source_snapshot(self):
        self.run_case("snapshot_identity")

    def test_close_and_rapid_requests_discard_inflight_stale_results(self):
        self.run_case("supersede")

    def test_missing_and_catalog_io_failure_are_not_empty_and_do_not_spin(self):
        self.run_case("read_failure")

    def test_recovery_invalidates_completed_data_without_revision_change(self):
        self.run_case("same_revision_recovery")

    def test_status_contention_retains_cache_but_requested_read_requires_retry(self):
        self.run_case("store_busy")


if __name__ == "__main__":
    unittest.main()
