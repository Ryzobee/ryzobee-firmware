"""Real C storage core, private POSIX files and injected faults; not board evidence."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class LuaFsStoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="ryz-lua-fs-build-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = Path(cls.build.name) / "lua_fs_store_test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-g",
            "-I", str(ROOT / "components/ryz_runtime/include"),
            "-I", str(ROOT / "components/ryz_app_fs"),
            str(ROOT / "components/ryz_app_fs/ryz_app_fs.c"),
            str(ROOT / "tests/app_fs_stubs/app_fs_host.c"),
            str(ROOT / "tests/lua_fs_store_test.c"), "-o", str(cls.binary),
        ], check=True, timeout=60)

    def run_case(self, name):
        with tempfile.TemporaryDirectory(prefix="ryz-lua-fs-data-") as root:
            result = subprocess.run([str(self.binary), name, root], capture_output=True,
                                    text=True, timeout=60,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("LUA_FS_STORE_PASS " + name, result.stdout)

    def test_validation_and_no_storage_side_effects(self): self.run_case("validation")
    def test_binary_empty_files_replace_sorted_list_info_and_remove(self): self.run_case("roundtrip")
    def test_namespace_isolation_and_collision_defence(self): self.run_case("isolation")
    def test_per_app_payload_and_file_count_quotas(self): self.run_case("quota")
    def test_global_payload_quota(self): self.run_case("global_quota")
    def test_global_file_count_and_reclaimed_deleted_keys(self): self.run_case("global_count")
    def test_short_write_sync_and_physical_no_space_preserve_old_data(self): self.run_case("write_faults")
    def test_close_failure_latches_and_does_not_report_success(self): self.run_case("close_faults")
    def test_torn_commit_is_unknown_then_fail_closed_until_explicit_remove(self): self.run_case("torn_commit")
    def test_committed_corruption_is_never_silent_old_value_or_not_found(self): self.run_case("corrupt")
    def test_symlinks_nonregular_files_and_oom(self): self.run_case("nonregular")
    def test_reentrant_busy_and_recovery(self): self.run_case("busy")
    def test_deletion_fault_leaves_tombstone_not_old_data(self): self.run_case("remove_fault")
    def test_ack_short_write_sync_and_close_failures_are_never_success(self): self.run_case("ack_faults")
    def test_missing_acknowledged_commit_cannot_rollback_or_reset(self): self.run_case("missing_commit")
    def test_broken_older_commit_is_conservative_recovery_required(self): self.run_case("old_commit")
    def test_first_uncommitted_orphan_is_not_missing(self): self.run_case("first_orphan")
    def test_shared_volume_write_reserve_still_allows_explicit_delete(self): self.run_case("reserve")

    def restart(self, initial, reopened):
        with tempfile.TemporaryDirectory(prefix="ryz-lua-fs-restart-") as root:
            first = subprocess.run([str(self.binary), initial, root], capture_output=True, text=True, timeout=30)
            self.assertEqual(first.returncode, 86, first.stdout + first.stderr)
            second = subprocess.run([str(self.binary), reopened, root], capture_output=True, text=True, timeout=30)
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertIn("LUA_FS_STORE_PASS " + reopened, second.stdout)

    def test_new_process_ignores_uncommitted_payload(self): self.restart("interrupt_data", "reopen_old")
    def test_new_process_reads_completed_publication(self): self.restart("interrupt_commit", "reopen_new")
    def test_new_process_preserves_committed_deletion(self): self.restart("interrupt_remove", "reopen_removed")
    def test_first_write_interruption_requires_explicit_recovery(self): self.restart("interrupt_first", "reopen_orphan")


if __name__ == "__main__": unittest.main()
