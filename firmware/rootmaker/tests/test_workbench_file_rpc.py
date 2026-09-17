"""Actual serial file Adapter + Store + locked IDF cJSON, no board access."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
STORE = ROOT / "components" / "ryz_script_store"
WORKBENCH = ROOT / "components" / "ryz_workbench"
METADATA = ROOT / "components" / "ryz_script_metadata"
STUBS = ROOT / "tests" / "script_store_stubs"


class WorkbenchFileRpcHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise RuntimeError("Set IDF_PATH to the project's ESP-IDF 5.5.4 checkout")
        json = Path(idf) / "components" / "json" / "cJSON"
        if not (json / "cJSON.c").is_file():
            raise RuntimeError("IDF_PATH does not contain cJSON sources")
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-script-rpc-build-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "file_rpc_test"
        json_object = Path(cls.directory.name) / "cJSON.o"
        # The pinned third-party cJSON uses sprintf, deprecated by recent macOS
        # SDKs. Suppress that one warning for that object only; project sources
        # below retain -Werror without exclusions. Do not modify the IDF source.
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-Wno-deprecated-declarations",
                        "-fsanitize=address,undefined", "-g", "-I", str(json),
                        "-c", str(json / "cJSON.c"), "-o", str(json_object)],
                       check=True, timeout=60)
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                   "-Werror", "-pthread", "-fsanitize=address,undefined", "-g"]
        for include in [STUBS, STORE, STORE / "include", METADATA / "include", WORKBENCH, json]:
            command.extend(["-I", str(include)])
        command.extend([str(STORE / "ryz_script_store.c"),
                        str(STUBS / "script_store_host.c"),
                        str(WORKBENCH / "workbench_file_rpc.c"),
                        str(METADATA / "ryz_script_metadata.c"),
                        str(json_object),
                        str(ROOT / "tests" / "workbench_file_rpc_test.c"),
                        "-lm", "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=60)

    def run_case(self, name):
        with tempfile.TemporaryDirectory(prefix="ryz-script-rpc-data-") as directory:
            result = subprocess.run([str(self.binary), name, directory],
                                    capture_output=True, text=True, timeout=30,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_FILE_RPC_PASS " + name, result.stdout)

    def test_legacy_wire_shapes_and_real_crud(self):
        self.run_case("compatibility")

    def test_version_boot_identity_and_device_cas(self):
        self.run_case("version_cas")

    def test_pagination_sorting_stale_revision_and_complete_legacy_list(self):
        self.run_case("paging")

    def test_unknown_times_validation_ordinary_boot_cas_and_runtime_exclusion(self):
        self.run_case("detail_validation")

    def test_journaled_utc_iso_fields_and_incomplete_time_commit(self):
        self.run_case("reliable_times")

    def test_committed_cleanup_failure_is_not_a_rejected_mutation(self):
        self.run_case("cleanup_outcome")

    def test_raw_description_allows_cas_removal_of_unexecutable_old_files(self):
        self.run_case("raw_description")

    def test_metadata_and_identity_come_from_the_same_source_snapshot(self):
        self.run_case("inspect_metadata")

    def test_reply_allocation_failure_never_claims_uncommitted_after_commit(self):
        self.run_case("allocation_outcomes")

    def test_missing_write_guard_cannot_mutate_the_store(self):
        self.run_case("missing_guard")

    def test_missing_partial_and_rejected_guards_fail_closed_for_all_mutations(self):
        self.run_case("guard_matrix")

    def test_validation_and_runtime_busy_precede_write_admission(self):
        self.run_case("validation_before_guard")

    def test_readonly_and_prepared_run_do_not_need_a_write_guard(self):
        self.run_case("reads_without_guard")

    def test_store_failure_releases_lease_before_an_explicit_retry(self):
        self.run_case("store_failure_release")

    def test_unknown_commit_and_recovery_rejection_release_lease(self):
        self.run_case("unknown_release")

    def test_concurrent_rpc_is_rejected_before_store_then_can_reuse_the_lease(self):
        self.run_case("concurrent_guard")

    def test_legacy_and_versioned_remove_oom_release_before_result_allocation(self):
        self.run_case("remove_allocation_outcomes")


if __name__ == "__main__":
    unittest.main()
