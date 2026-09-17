"""Production counters/wrappers with real pthreads and an IDF call boundary.

This does not prove GNU target relocation, radio traffic or air delivery.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "ryz_provisioning"
STUBS = ROOT / "tests" / "net_traffic_stubs"


class NetTrafficTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-net-traffic-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "net-traffic"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-pthread", "-g", "-fsanitize=address,undefined", "-I" + str(STUBS),
            "-I" + str(COMPONENT), str(ROOT / "tests" / "net_traffic_test.c"),
            str(COMPONENT / "ryz_net_traffic.c"), "-o", str(cls.binary),
        ], check=True, timeout=60)

    def run_case(self, case):
        result = subprocess.run(
            [str(self.binary), case], capture_output=True, text=True, timeout=10,
            env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("NET_TRAFFIC_PASS " + case, result.stdout)

    def test_first_observation_and_three_real_io_totals(self):
        self.run_case("totals")

    def test_arguments_results_and_unowned_netifs_are_transparent(self):
        self.run_case("transparent")

    def test_bad_observe_arguments_clear_output_without_rebinding(self):
        self.run_case("arguments")

    def test_identity_change_revoke_and_same_pointer_rebind_get_new_epoch(self):
        self.run_case("identity")

    def test_all_three_inflight_io_paths_cannot_credit_rebound_epoch(self):
        self.run_case("inflight")

    def test_sdk_reentry_and_rx_buffer_consumption_are_safe(self):
        self.run_case("reentry")

    def test_real_threads_accumulate_exactly_while_snapshots_remain_monotonic(self):
        self.run_case("concurrent")

    def test_tx_overflow_is_invalid_until_explicit_revoke(self):
        self.run_case("overflow_tx")

    def test_rx_overflow_is_invalid_until_explicit_revoke(self):
        self.run_case("overflow_rx")


if __name__ == "__main__":
    unittest.main()
