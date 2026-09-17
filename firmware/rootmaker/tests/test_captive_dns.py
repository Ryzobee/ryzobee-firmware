"""Production DNS lifecycle, deterministic RTOS/socket boundary substitutes; no network."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CaptiveDnsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-captive-dns-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "dns"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-g", "-fsanitize=address,undefined",
                        "-I" + str(ROOT / "tests/captive_dns_stubs"),
                        "-I" + str(ROOT / "components/ryz_provisioning"),
                        str(ROOT / "components/ryz_provisioning/ryz_captive_dns.c"),
                        str(ROOT / "tests/captive_dns_test.c"), "-o", str(cls.binary)],
                       check=True, timeout=30)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True,
                                timeout=10, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("CAPTIVE_DNS_PASS", result.stdout)

    def test_stop_timeout_retains_resources_and_retry_waits_for_ack(self):
        self.run_case("timeout")

    def test_close_error_retains_socket_without_renotifying_deleted_task(self):
        self.run_case("close")

    def test_null_stop_is_idempotent_and_self_stop_is_rejected(self):
        self.run_case("self")

    def test_bind_failure_and_close_error_expose_recoverable_owned_handle(self):
        self.run_case("bind_failure")

    def test_task_creation_failure_and_close_error_expose_recoverable_owned_handle(self):
        self.run_case("task_failure")


if __name__ == "__main__":
    unittest.main()
