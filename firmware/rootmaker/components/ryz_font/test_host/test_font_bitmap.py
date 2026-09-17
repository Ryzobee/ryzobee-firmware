"""Host tests for FreeType bitmap copying without ESP-IDF dependencies."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class FontBitmapTest(unittest.TestCase):
    def test_signed_pitch_and_capacity_contract(self):
        component = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="ryzobee-font-test-") as temporary:
            binary = Path(temporary) / "font-bitmap-test"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-I" + str(component),
                    str(component / "test_host/font_bitmap_test.c"),
                    str(component / "ryz_font_bitmap.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            completed = subprocess.run(
                [str(binary)], text=True, capture_output=True, check=False
            )
            self.assertEqual(
                completed.returncode, 0, completed.stderr + completed.stdout
            )
            self.assertIn("RYZ_FONT_BITMAP_PASS", completed.stdout)


if __name__ == "__main__":
    unittest.main()
