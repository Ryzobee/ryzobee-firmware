"""Real Store C, POSIX temp files and CommonCrypto SHA-256; not board evidence."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "ryz_script_store"
STUBS = ROOT / "tests" / "script_store_stubs"


class ScriptStoreHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-script-build-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "script_store_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                   "-Werror", "-pthread", "-fsanitize=address,undefined", "-g"]
        for include in [STUBS, COMPONENT, COMPONENT / "include"]:
            command.extend(["-I", str(include)])
        command.extend([str(COMPONENT / "ryz_script_store.c"),
                        str(STUBS / "script_store_host.c"),
                        str(ROOT / "tests" / "script_store_test.c"),
                        "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=60)

    def run_case(self, name):
        with tempfile.TemporaryDirectory(prefix="ryz-script-data-") as root:
            result = subprocess.run([str(self.binary), name, root],
                                    capture_output=True, text=True, timeout=30,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SCRIPT_STORE_PASS " + name, result.stdout)

    def test_name_source_hash_and_argument_validation(self):
        self.run_case("validation")

    def test_real_roundtrip_known_sha_max_source_and_snapshot_ownership(self):
        self.run_case("roundtrip")

    def test_sorted_paginated_index_and_revision(self):
        self.run_case("catalog")

    def test_nonregular_symlink_fifo_and_corrupt_sources(self):
        self.run_case("nonregular")

    def test_source_version_cas_and_stale_selection(self):
        self.run_case("cas")

    def test_boot_is_an_ordinary_user_file_with_cas_and_not_protected(self):
        self.run_case("boot")

    def test_unidentified_legacy_upload_artifacts_are_preserved(self):
        self.run_case("legacy")

    def test_stateful_public_operations_reject_concurrent_owner(self):
        self.run_case("concurrency")

    def test_capacity_rejection_preserves_existing_source(self):
        self.run_case("capacity")

    def test_index_overflow_is_explicit_not_truncated(self):
        self.run_case("index_limit")

    def test_write_close_rename_remove_errors_preserve_put_outcome(self):
        self.run_case("put_io")

    def test_close_rename_cleanup_errors_preserve_delete_outcome(self):
        self.run_case("remove_io")

    def test_partial_journal_is_preserved_and_blocks_further_mutations(self):
        self.run_case("partial_journal")

    def test_failed_rollback_before_and_after_rename_recovers_safely(self):
        self.run_case("rollback_io")

    def test_partial_stage_cleanup_before_and_after_remove_recovers_safely(self):
        self.run_case("stage_cleanup")

    def test_unidentified_new_transaction_artifacts_are_preserved(self):
        self.run_case("orphan_artifacts")

    def test_platform_degraded_read_and_visit_latch_public_status(self):
        self.run_case("io_degraded")

    def test_oversized_files_stream_describe_cas_remove_and_replace_without_allocation(self):
        self.run_case("oversized_maintenance")

    def test_empty_nul_and_nonregular_describe_and_repair(self):
        self.run_case("corrupt_maintenance")

    def test_stream_hash_io_failure_latches_degraded_capacity(self):
        self.run_case("describe_degraded")

    def test_status_is_cached_even_when_capacity_fails_or_is_gated(self):
        self.run_case("status_cache")

    def test_new_file_count_scan_fault_publishes_degraded_status(self):
        self.run_case("new_scan_degraded")

    def run_restart(self, interrupted, reopened):
        with tempfile.TemporaryDirectory(prefix="ryz-script-restart-") as root:
            interrupted_result = subprocess.run(
                [str(self.binary), interrupted, root], capture_output=True,
                text=True, timeout=30,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
            self.assertEqual(interrupted_result.returncode, 86,
                             interrupted_result.stdout + interrupted_result.stderr)
            result = subprocess.run([str(self.binary), reopened, root],
                                    capture_output=True, text=True, timeout=30,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SCRIPT_STORE_PASS " + reopened, result.stdout)

    def test_fresh_process_recovers_interrupted_precommit_to_old_source(self):
        self.run_restart("interrupt_before_commit", "reopen_rollback")

    def test_fresh_process_recognizes_interrupted_committed_put(self):
        self.run_restart("interrupt_after_commit", "reopen_put_commit")

    def test_fresh_process_recognizes_interrupted_committed_remove(self):
        self.run_restart("interrupt_remove", "reopen_remove_commit")

    def test_fresh_process_restores_complete_oversized_backup_without_allocation(self):
        self.run_restart("interrupt_large_put", "reopen_large_rollback")

    def test_fresh_process_recognizes_interrupted_oversized_remove(self):
        self.run_restart("interrupt_large_remove", "reopen_remove_commit")

    def test_copy_boot_uses_selected_sha_revision_and_self_copy_noop(self):
        self.run_case("copy_boot")

    def test_copy_boot_rejects_invalid_source_oom_and_capacity_without_replacing_old_boot(self):
        self.run_case("copy_boot_bad")

    def test_copy_boot_failure_and_cleanup_results_use_real_transaction(self):
        self.run_case("copy_boot_faults")

    def test_delete_all_includes_boot_and_invalid_sources_but_preserves_non_scripts(self):
        self.run_case("delete_all")

    def test_delete_all_invalid_revision_oom_capacity_and_legacy_artifacts_have_no_deletions(self):
        self.run_case("bulk_admission")

    def test_delete_all_reports_partial_committed_cleanup_and_unknown_without_continuing(self):
        self.run_case("bulk_faults")

    def test_copy_and_bulk_hold_the_same_store_lock_through_their_filesystem_work(self):
        self.run_case("combined_lock")

    def test_postcommit_capacity_fault_preserves_copy_and_bulk_visible_results(self):
        self.run_case("combined_capacity_fault")

    def test_v1_journal_put_and_remove_recover_but_legacy_boot_is_still_rejected(self):
        for case in ["v1_put", "v1_remove", "v1_boot"]:
            with self.subTest(case=case):
                self.run_case(case)

    def test_restart_recovers_one_bulk_file_and_does_not_resume_remaining_deletions(self):
        self.run_restart("interrupt_bulk", "reopen_bulk")

    def test_restart_recovers_v2_boot_copy_before_and_after_commit(self):
        for phase in ["before", "after"]:
            with self.subTest(phase=phase):
                self.run_restart("interrupt_boot_" + phase, "reopen_boot_" + phase)


if __name__ == "__main__":
    unittest.main()
