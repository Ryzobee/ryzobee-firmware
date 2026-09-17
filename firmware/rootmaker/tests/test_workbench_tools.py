"""Production Workbench lifecycle helper, not a FreeRTOS/UI-loop simulation."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WorkbenchToolsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = Path(os.environ["IDF_PATH"])
        json = idf / "components/json/cJSON"
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-workbench-tools-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "workbench_tools"
        flags = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-pthread", "-fsanitize=address,undefined", "-g"]
        obj = Path(cls.directory.name) / "json.o"
        subprocess.run(flags + ["-Wno-deprecated-declarations", "-I" + str(json), "-c",
                       str(json / "cJSON.c"), "-o", str(obj)], check=True, timeout=60)
        command = flags + ["-I" + str(json), "-I" + str(ROOT / "components/ryz_workbench"),
                           "-I" + str(ROOT / "tests/workbench_rgb_rpc_stubs")]
        for name in ("ryz_tools", "ryz_i2c_scan", "ryz_monitor", "ryz_rgb"):
            command += ["-I" + str(ROOT / "components" / name / "include")]
        command += [str(ROOT / "components/ryz_workbench/workbench_tools.c"),
                    str(ROOT / "components/ryz_tools/ryz_tools.c"),
                    str(ROOT / "tests/workbench_tools_test.c"), str(obj), "-lm", "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=60)

    def case(self, name):
        result = subprocess.run([str(self.binary), name], text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("WORKBENCH_TOOLS_PASS " + name, result.stdout)

    def test_unused_job_finishes_without_tool_session(self):
        self.case("unused")

    def test_lazy_session_survives_vm_end_timeout_and_another_job(self):
        self.case("retire")

    def test_failed_cleanup_requires_explicit_token_bound_recovery(self):
        self.case("recovery")

    def test_retry_complete_ack_precedes_every_possible_allocation_failure(self):
        self.case("oom")

    def test_strict_rpc_fields_boot_and_session(self):
        self.case("validation")

    def test_broker_gate_busy_zero_observation_retains_session_until_tick(self):
        self.case("broker_busy")

    def test_helper_contention_does_not_change_unused_job_result(self):
        self.case("helper_busy")

    def test_failed_tool_does_not_stop_other_pending_cleanup(self):
        self.case("mixed")

    def test_three_failed_retirements_leave_one_safe_current_session_slot(self):
        self.case("capacity")

    def test_rejected_broker_open_never_consumes_retention(self):
        self.case("open_failure")

    def test_job_allocation_can_be_destroyed_while_cleanup_keeps_progressing(self):
        self.case("destroyed_job")

    def test_missing_retirement_record_is_never_inferred_clean(self):
        self.case("missing_record")
