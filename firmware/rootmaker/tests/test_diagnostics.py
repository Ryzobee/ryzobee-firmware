"""Bounded report store tests with the real C implementation and fake entropy/time."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DiagnosticsTest(unittest.TestCase):
    def test_production_http_handlers_and_ap_access_guard(self):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            self.skipTest("IDF_PATH required for the pinned IDF cJSON implementation")
        with tempfile.TemporaryDirectory(prefix="ryz-diagnostics-http-") as directory:
            directory = Path(directory)
            source = (ROOT / "components/ryz_provisioning/ryz_provisioning_esp.c").read_text()
            start = source.index("static bool diagnostics_ap_request_allowed(")
            end = source.index("\n#endif\n\nstatic esp_err_t root_get_handler", start)
            (directory / "diagnostics_adapter.inc").write_text(source[start:end])
            binary = directory / "http"
            json = Path(idf) / "components/json/cJSON"
            flags = ["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DRYZ_DIAGNOSTICS_HOST_TEST",
                     "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror", "-pthread"]
            subprocess.run(flags + ["-Wno-deprecated-declarations", "-I", str(json), "-c",
                                    str(json / "cJSON.c"), "-o", str(directory / "json.o")], check=True)
            subprocess.run(flags + ["-I", str(ROOT / "components/ryz_diagnostics/include"),
                                   "-I", str(ROOT / "components/ryz_provisioning/test_host/stubs"),
                                   "-I", str(directory), "-I", str(json),
                                   str(ROOT / "components/ryz_diagnostics/ryz_diagnostics.c"),
                                   str(ROOT / "tests/diagnostics_http_test.c"), str(directory / "json.o"),
                                   "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, check=True, timeout=20)
            self.assertIn("DIAGNOSTICS_HTTP_PASS", result.stdout)

    def test_frozen_report_lifetime_identity_and_concurrency(self):
        with tempfile.TemporaryDirectory(prefix="ryz-diagnostics-") as directory:
            binary = Path(directory) / "diagnostics"
            subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DRYZ_DIAGNOSTICS_HOST_TEST",
                            "-fsanitize=address,undefined", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-I", str(ROOT / "components/ryz_diagnostics/include"),
                            "-I", str(ROOT / "components/ryz_provisioning/test_host/stubs"),
                            str(ROOT / "components/ryz_diagnostics/ryz_diagnostics.c"),
                            str(ROOT / "tests/diagnostics_test.c"), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, check=True, timeout=20)
            self.assertIn("DIAGNOSTICS_PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
