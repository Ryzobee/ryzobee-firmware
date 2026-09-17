"""Store -> real RPC -> real job_start -> real Lua; Host BSP spies, not pixels."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ScriptRunIntegrationHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise RuntimeError("Set IDF_PATH to the project's ESP-IDF 5.5.4 checkout")
        json = Path(idf) / "components" / "json" / "cJSON"
        if not (json / "cJSON.c").is_file():
            raise RuntimeError("IDF_PATH does not contain the locked cJSON sources")
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-script-run-build-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "script_run_integration_test"
        json_object = Path(cls.directory.name) / "cJSON.o"
        cc = os.environ.get("CC", "cc")
        strict = [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                  "-fsanitize=address,undefined", "-pthread"]
        # Only locked, unmodified third-party cJSON needs the macOS sprintf
        # deprecation exclusion. Every project source keeps strict warnings.
        subprocess.run(strict + ["-Wno-deprecated-declarations", "-I", str(json),
                                 "-c", str(json / "cJSON.c"), "-o", str(json_object)],
                       check=True, timeout=60)
        runtime = ROOT / "components/ryz_runtime"
        workbench = ROOT / "components/ryz_workbench"
        store = ROOT / "components/ryz_script_store"
        metadata = ROOT / "components/ryz_script_metadata"
        lua = ROOT / "managed_components/georgik__lua"
        command = strict + ["-DMAKE_LIB", "-include", str(ROOT / "host/sdkconfig.h")]
        for include in [ROOT / "tests/script_store_stubs", ROOT / "tests/runtime_owner_stubs",
                        runtime / "include", workbench, store, store / "include",
                        metadata / "include", ROOT / "components/ryz_board/include",
                        ROOT / "components/ryz_lvgl/include", lua / "include", lua / "lua", json,
                        *[ROOT / "components" / name / "include" for name in
                          ("ryz_tools", "ryz_i2c_scan", "ryz_rgb", "ryz_monitor")]]:
            command.extend(["-I", str(include)])
        # Keep the shared Host adapter source unchanged. Only its rename
        # symbol is delegated through this executable's bounded commit gate;
        # after release it still performs the original real POSIX rename.
        host_object = Path(cls.directory.name) / "script_store_host.o"
        subprocess.run(command + [
            "-Dryz_script_store_platform_rename=script_run_host_rename", "-c",
            str(ROOT / "tests/script_store_stubs/script_store_host.c"), "-o", str(host_object),
        ], check=True, timeout=60)
        command.extend(map(str, [
            ROOT / "tests/script_run_integration_test.c",
            runtime / "lua_runtime.c", runtime / "app_runtime.c",
            runtime / "app_tools.c",
            runtime / "nervous_runtime.c", runtime / "ryz_runtime_io.c",
            workbench / "workbench_io.c", workbench / "workbench_job_start.c",
            workbench / "workbench_file_rpc.c", store / "ryz_script_store.c",
            metadata / "ryz_script_metadata.c", host_object,
            ROOT / "components/ryz_board/touch_protocol.c", lua / "lua/onelua.c", json_object,
        ]))
        command.extend(["-lm", "-o", str(cls.binary)])
        subprocess.run(command, check=True, timeout=120)

    def run_case(self, name):
        with tempfile.TemporaryDirectory(prefix="ryz-script-run-data-") as root:
            result = subprocess.run([str(self.binary), name, root], capture_output=True,
                                    text=True, timeout=30,
                                    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SCRIPT_RUN_INTEGRATION_PASS " + name, result.stdout)

    def test_selected_snapshot_runs_after_disk_is_replaced(self):
        self.run_case("selected")

    def test_schema_boot_hash_busy_handler_and_recovery_refuse_to_enqueue(self):
        self.run_case("rejections")

    def test_final_owner_admission_rechecks_competing_job_and_gateway(self):
        self.run_case("admission")

    def test_queue_rejection_legacy_reply_later_and_job_id_exhaustion(self):
        self.run_case("modes")

    def test_real_job_start_never_enqueues_without_complete_ack(self):
        self.run_case("ack_budget")

    def test_rpc_version_allocation_failure_keeps_queued_source_and_unknown_reply(self):
        self.run_case("version_budget")

    def test_shared_write_lease_rejects_after_a_real_job_wins_final_admission(self):
        self.run_case("writer_existing_job")

    def test_real_put_commit_rejects_prepared_job_and_second_writer_then_returns_lease(self):
        self.run_case("writer_put_commit")

    def test_real_remove_commit_rejects_prepared_job_and_second_writer_then_returns_lease(self):
        self.run_case("writer_remove_commit")

    def test_store_failures_unknown_and_committed_cleanup_return_lease_without_faking_recovery(self):
        self.run_case("writer_faults")

    def test_put_ack_allocation_failure_after_commit_returns_write_lease(self):
        self.run_case("writer_put_oom")

    def test_remove_ack_allocation_failure_after_commit_returns_write_lease(self):
        self.run_case("writer_remove_oom")


if __name__ == "__main__":
    unittest.main()
