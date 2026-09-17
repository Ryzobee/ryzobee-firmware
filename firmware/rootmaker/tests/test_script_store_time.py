"""Real Store and POSIX files; controlled UTC, not SPIFFS or board evidence."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STORE = ROOT / "components" / "ryz_script_store"
STUBS = ROOT / "tests" / "script_store_stubs"


class ScriptStoreTimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="ryz-store-time-build-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = Path(cls.build.name) / "script_store_time_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                   "-Werror", "-pthread", "-fsanitize=address,undefined", "-g"]
        for directory in (STUBS, STORE, STORE / "include"):
            command.extend(["-I", str(directory)])
        command.extend([str(STORE / "ryz_script_store.c"),
                        str(STUBS / "script_store_host.c"),
                        str(ROOT / "tests" / "script_store_time_test.c"),
                        "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=60)

    def execute(self, case, root, expected=0):
        result = subprocess.run([str(self.binary), case, root],
                                capture_output=True, text=True, timeout=30,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        if expected == 0:
            self.assertIn("SCRIPT_STORE_TIME_PASS " + case, result.stdout)

    def run_case(self, case):
        with tempfile.TemporaryDirectory(prefix="ryz-script-time-data-") as root:
            self.execute(case, root)

    def test_trusted_creation_modification_offline_and_identical_noop(self):
        self.run_case("basic")

    def test_factory_dates_are_available_without_network(self):
        with tempfile.TemporaryDirectory(prefix="ryz-script-factory-") as root:
            output = Path(root) / "factory-scripts"
            subprocess.run([os.sys.executable, str(ROOT / "tools/prepare_factory_scripts.py"),
                            "--source", str(ROOT / "fs"), "--output", str(output)],
                           env={**os.environ, "SOURCE_DATE_EPOCH": "1735689600"},
                           check=True, capture_output=True, timeout=10)
            # The Store harness only allows a root with its explicit test prefix.
            store_root = Path(root) / "ryz-script-image"
            output.rename(store_root)
            self.execute("factory", str(store_root))

    def test_historic_and_unsynced_creation_never_backfilled(self):
        self.run_case("unknown_creation")

    def test_delete_then_same_content_recreate_does_not_resurrect_creation(self):
        self.run_case("recreate")

    def test_copy_boot_records_target_lifetime_not_source_lifetime(self):
        self.run_case("copy_boot")

    def test_calendar_range_and_backward_valid_utc_are_not_fabricated(self):
        self.run_case("bounded_clock")

    def sequence(self, first, second, interrupted=False):
        with tempfile.TemporaryDirectory(prefix="ryz-script-time-restart-") as root:
            self.execute(first, root, 86 if interrupted else 0)
            self.execute(second, root)

    def test_completed_dates_survive_fresh_process_with_no_trusted_clock(self):
        self.sequence("persist", "reopen_persisted")

    def test_time_partial_close_unlink_and_rename_failures_recover_original_epoch(self):
        for fault in range(6):
            with self.subTest(fault=fault):
                self.sequence(f"time_fault_{fault}", "reopen_put")

    def test_committed_deletion_sidecar_cleanup_failure_recovers_without_resurrection(self):
        for fault in range(2):
            with self.subTest(fault=fault):
                self.sequence(f"delete_fault_{fault}", "reopen_remove")

    def test_real_process_interruptions_at_source_and_time_rename_boundaries(self):
        for boundary, state in enumerate(("rollback", "put", "put", "remove")):
            with self.subTest(boundary=boundary):
                self.sequence(f"interrupt_{boundary}", "reopen_" + state, interrupted=True)

    def test_partial_timestamp_stage_cleanup_failure_is_retryable_without_new_time(self):
        for after in range(2):
            with self.subTest(after=after):
                self.run_case(f"stage_retry_{after}")

    def test_corrupt_colliding_mismatched_or_orphan_sidecars_are_preserved_and_stay_degraded(self):
        for kind in range(8):
            with self.subTest(kind=kind):
                self.run_case(f"sidecar_bad_{kind}")

    def test_unknown_stage_bytes_even_valid_unrelated_record_are_never_overwritten(self):
        for kind in range(3):
            with self.subTest(kind=kind):
                self.run_case(f"stage_bad_{kind}")

    def test_only_exact_old_or_new_record_prefixes_are_repairable(self):
        for kind in range(8):
            with self.subTest(kind=kind):
                self.run_case(f"stage_prefix_{kind}")

    def test_timestamp_stage_without_journal_is_preserved(self):
        self.run_case("orphan_stage")

    def test_v1_v2_recover_without_times_but_reject_mixed_timestamp_transactions(self):
        for version in (1, 2):
            for kind in range(4):
                with self.subTest(version=version, kind=kind):
                    self.run_case(f"legacy_{version}_{kind}")

    def test_bound_times_do_not_prevent_raw_empty_nul_oversize_cas_removal(self):
        self.run_case("raw_sources")

    def test_delete_all_stops_on_time_cleanup_failure_and_preserves_remaining_dates(self):
        self.run_case("bulk_cleanup")

    def test_v3_valid_checksums_cannot_hide_inconsistent_identity_or_time_fields(self):
        for kind in range(5):
            with self.subTest(kind=kind):
                self.run_case(f"journal_bad_{kind}")

    def test_fresh_process_init_cannot_clear_corrupt_sidecar_degradation(self):
        self.sequence("persist_corrupt", "reopen_corrupt")


if __name__ == "__main__":
    unittest.main()
