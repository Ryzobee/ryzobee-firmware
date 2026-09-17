"""Production bootstrap policy and real IDF cJSON with a bounded heap peer.

Host allocator/sanitizer evidence is not a PSRAM or low-internal-RAM device test.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class JsonMemoryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise RuntimeError("Set IDF_PATH to the project's ESP-IDF 5.5.4 checkout")
        json = Path(idf) / "components/json/cJSON"
        if not (json / "cJSON.c").is_file():
            raise RuntimeError("IDF_PATH does not contain the real cJSON sources")
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-json-memory-")
        cls.addClassCleanup(cls.directory.cleanup)
        temporary = Path(cls.directory.name)
        cls.binary = temporary / "json-memory"
        flags = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-pthread",
                 "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined"]
        # Capture the public InitHooks boundary without replacing cJSON's
        # parsing, printing, allocation paths or its global hook implementation.
        subprocess.run(flags + ["-Wno-deprecated-declarations", "-I", str(json),
                       "-DcJSON_InitHooks=cJSON_RealInitHooks", "-c", str(json / "cJSON.c"),
                       "-o", str(temporary / "json.o")], check=True, timeout=30)
        subprocess.run(flags + ["-I", str(ROOT / "tests/json_memory_stubs"),
                       "-I", str(ROOT / "main"), "-I", str(json),
                       str(ROOT / "main/ryz_json_memory.c"), str(ROOT / "tests/json_memory_test.c"),
                       str(temporary / "json.o"), "-o", str(cls.binary)], check=True, timeout=30)

    def case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True,
                                timeout=15, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("JSON_MEMORY_PASS " + name, result.stdout)

    def test_real_parse_create_print_delete_lifecycle_uses_only_external_caps(self):
        self.case("lifecycle")

    def test_printed_output_can_be_released_by_json_or_standard_free(self):
        self.case("output-free")

    def test_external_exhaustion_never_falls_back_and_partial_trees_are_released(self):
        self.case("failure")

    def test_fixed_hooks_remain_stable_across_concurrent_independent_documents(self):
        self.case("threads")


if __name__ == "__main__":
    unittest.main()
