"""Real Apps Controller/reader/Store/RPC/job_start; no GUI or board access."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WorkbenchAppsHostTest(unittest.TestCase):
    external_source_memory = False

    @classmethod
    def setUpClass(cls):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise RuntimeError("Set IDF_PATH to the project's ESP-IDF 5.5.4 checkout")
        json = Path(idf) / "components/json/cJSON"
        if not (json / "cJSON.c").is_file():
            raise RuntimeError("IDF_PATH does not contain locked cJSON sources")
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-wb-apps-build-")
        cls.addClassCleanup(cls.directory.cleanup)
        build = Path(cls.directory.name)
        cls.binary = build / "workbench_apps_test"
        strict = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                  "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                  "-fsanitize=address,undefined"]
        if cls.external_source_memory:
            strict += ["-DWB_TEST_EXTERNAL_SOURCE"]
        json_object = build / "cJSON.o"
        subprocess.run(strict + ["-Wno-deprecated-declarations", "-I", str(json),
                                 "-c", str(json / "cJSON.c"), "-o", str(json_object)],
                       check=True, timeout=60)
        includes = [ROOT / "tests/script_store_stubs", ROOT / "tests/workbench_apps_stubs", json]
        for name in ("ryz_apps", "ryz_script_store", "ryz_script_metadata", "ryz_workbench",
                     "ryz_system_ui", "ryz_runtime", "ryz_tools", "ryz_i2c_scan",
                     "ryz_rgb", "ryz_monitor"):
            component = ROOT / "components" / name
            includes.extend([component, component / "include"])
        flags = [item for include in includes for item in ("-I", str(include))]
        workbench = ROOT / "components/ryz_workbench"
        objects = [json_object]
        for name in ("workbench_apps", "workbench_job_start"):
            output = build / (name + ".o")
            policy = ["-DESP_PLATFORM"] if cls.external_source_memory and name == "workbench_job_start" else []
            subprocess.run(strict + policy + flags + ["-include", str(ROOT / "tests/workbench_apps_stubs/allocation.h"),
                           "-c", str(workbench / (name + ".c")), "-o", str(output)],
                           check=True, timeout=60)
            objects.append(output)
        sources = [ROOT / "tests/workbench_apps_test.c",
                   ROOT / "components/ryz_apps/ryz_apps.c",
                   ROOT / "components/ryz_apps/ryz_apps_reader.c",
                   ROOT / "components/ryz_script_store/ryz_script_store.c",
                   ROOT / "components/ryz_script_metadata/ryz_script_metadata.c",
                   ROOT / "tests/script_store_stubs/script_store_host.c",
                   workbench / "workbench_file_rpc.c"]
        subprocess.run(strict + flags + list(map(str, sources + objects)) + ["-lm", "-o", str(cls.binary)],
                       check=True, timeout=60)

    def run_case(self, name):
        with tempfile.TemporaryDirectory(prefix="ryz-script-wb-apps-data-") as directory:
            result = subprocess.run([str(self.binary), name, directory], capture_output=True,
                                    text=True, timeout=20,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_APPS_PASS " + name, result.stdout)

    def test_catalog_selection_and_back_preserve_page_offset_and_limit(self):
        self.run_case("navigation")

    def test_back_is_page_bound_across_reader_refresh_before_submit_and_consume(self):
        self.run_case("back_reader_refresh")

    def test_rx_prepares_owned_a_owner_enqueues_a_after_disk_changes_without_duplicate_run(self):
        self.run_case("run_snapshot")

    def test_maximum_16k_source_reaches_job_intact_and_is_released(self):
        self.run_case("run_max_source")

    def test_stale_navigation_reader_invalid_index_and_invalid_source_do_not_run(self):
        self.run_case("identities")

    def test_changed_file_before_rx_is_rejected_without_automatic_retry(self):
        self.run_case("change_before_rx")

    def test_back_close_new_page_or_stale_reader_discard_prepared_source(self):
        for name in ("discard_back", "discard_close", "discard_page", "discard_reader"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_owner_closes_while_rx_is_blocked_in_real_store_then_discards_result(self):
        self.run_case("inflight_close")

    def test_final_admission_failure_unknown_and_preparation_oom_release_owned_source(self):
        for name in ("gateway", "queue", "busy", "no_handler", "uncertain", "source_oom", "json_oom"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_back_after_last_page_deletion_falls_back_once_and_preserves_limit(self):
        self.run_case("deleted_page")

    def test_confirmed_delete_runs_cas_on_rx_then_returns_to_list(self):
        self.run_case("delete_selected")

    def test_delete_requires_complete_granted_guard_and_rechecks_new_job(self):
        for name in ("delete_guard_missing", "delete_guard_no_begin", "delete_guard_no_end",
                     "delete_guard_denied", "delete_guard_new_job"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_store_busy_after_writer_admission_releases_lease_without_retry(self):
        self.run_case("delete_store_busy")

    def test_delete_rejects_wrong_identity_changed_file_busy_and_leaving_before_acceptance(self):
        for name in ("delete_mismatch", "delete_changed", "delete_busy", "delete_back",
                     "delete_close", "delete_newpage"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_delete_allows_ordinary_boot_empty_nul_and_oversize_files(self):
        for name in ("delete_boot", "delete_empty", "delete_nul", "delete_oversize"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_accepted_delete_survives_close_and_writer_gate_blocks_real_job_admission(self):
        self.run_case("delete_inflight_close")

    def test_delete_last_page_returns_to_valid_page_preserving_limit(self):
        self.run_case("delete_last_page")

    def test_delete_commit_cleanup_and_acknowledgement_unknown_remain_distinct_without_retry(self):
        for name in ("delete_cleanup", "delete_request_oom", "delete_ack_unknown", "delete_storage_unknown"):
            with self.subTest(name=name):
                self.run_case(name)


class WorkbenchAppsExternalSourceTest(WorkbenchAppsHostTest):
    """Same real pipeline, only ESP heap capabilities replaced by a host peer."""
    external_source_memory = True


if __name__ == "__main__":
    unittest.main()
