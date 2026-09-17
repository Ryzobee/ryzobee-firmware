"""Pure production metadata parser, no Lua VM, file reads or platform adapter."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "ryz_script_metadata"


class ScriptMetadataHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-metadata-build-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "script_metadata_test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-g",
            "-I", str(ROOT / "tests/script_metadata_stubs"),
            "-I", str(COMPONENT / "include"),
            str(COMPONENT / "ryz_script_metadata.c"),
            str(ROOT / "tests/script_metadata_test.c"), "-o", str(cls.binary),
        ], check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=10,
                                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SCRIPT_METADATA_PASS " + name, result.stdout)

    def test_header_fields_bom_crlf_whitespace_and_unknown_tags(self):
        self.run_case("grammar")

    def test_code_or_long_comment_ends_header_and_metadata_is_literal_text(self):
        self.run_case("boundary")

    def test_duplicate_fields_retain_first_including_empty_or_invalid(self):
        self.run_case("duplicates")

    def test_strict_utf8_control_rejection_and_codepoint_prefix_truncation(self):
        self.run_case("unicode")

    def test_explicit_source_bounds_and_output_ownership(self):
        self.run_case("bounds")

    def test_existing_factory_and_example_scripts_have_complete_metadata(self):
        for source in sorted([*(ROOT / "fs").glob("*.lua"),
                              *(ROOT / "scripts").glob("*.lua")]):
            with self.subTest(source=source.name):
                result = subprocess.run([str(self.binary), "file", str(source)],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
