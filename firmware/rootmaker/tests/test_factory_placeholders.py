"""Factory entrypoints through app-host, without attached hardware.

The historical filename is retained for test discovery. These scripts are now
real tools; native pixels and interactive behavior have same-source fixtures.
"""
import json
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]


class FactoryPlaceholdersTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["sh", str(ROOT / "tools/build_app_host.sh")],
                       cwd=ROOT, check=True, timeout=60)

    def run_factory(self, name):
        source_path = ROOT / "fs" / name
        source = source_path.read_text(encoding="utf-8")
        self.assertEqual(source_path.read_bytes(), (ROOT / "scripts" / name).read_bytes())
        self.assertTrue(source.startswith("-- ryz-app/1\n"))
        self.assertNotIn("placeholder", source)
        self.assertIn("-- @author: RyzoBee", source)
        self.assertLessEqual(len(source.encode("utf-8")), 16384)
        completed = subprocess.run(
            [str(ROOT / "build-host/app-host"), str(source_path)],
            input="end 200\n", capture_output=True, text=True, timeout=10,
            check=False,
        )
        self.assertEqual(completed.stderr, "")
        rows = [json.loads(line) for line in completed.stdout.splitlines() if line]
        frames = [row for row in rows if row.get("event") == "frame"]
        self.assertGreater(len(frames), 0)
        summary = rows[-1]
        self.assertTrue(summary["summary"])
        self.assertEqual(summary["runtime"], "ryz-app/1")
        texts = [op["text"] for frame in frames for op in frame["ops"] if op["op"] == "text"]
        return completed, summary, texts

    def test_i2c_renders_unavailable_without_claiming_hardware_success(self):
        completed, summary, texts = self.run_factory("tool_i2c.lua")
        self.assertEqual(completed.returncode, 1)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["phase"], "unsupported")
        self.assertIn("Hardware tools are unavailable", summary["error"])
        self.assertIn("INTERNAL I2C0", texts)
        self.assertIn("OPEN: UNAVAILABLE", texts)

    def test_hardware_hub_is_ui_only_until_user_starts_a_test(self):
        completed, summary, texts = self.run_factory("tool_hardware.lua")
        self.assertEqual(completed.returncode, 0)
        self.assertTrue(summary["ok"])
        self.assertEqual(summary["phase"], "done")
        self.assertEqual(summary["error"], "")
        for title in ("LCD / PIXELS", "TOUCH / GRID", "IMU / MOTION", "RGB LED", "BATTERY"):
            self.assertIn(title, texts)
        self.assertNotIn("PASS", texts)


if __name__ == "__main__":
    unittest.main()
