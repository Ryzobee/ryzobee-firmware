"""Factory packaging and ordinary host boundary; public UI scripts use CTest."""
import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FactoryHardwareTest(unittest.TestCase):
    def test_readable_factory_copy_matches_source_and_existing_quota(self):
        source = (ROOT / "scripts/tool_hardware.lua").read_bytes()
        self.assertEqual((ROOT / "fs/tool_hardware.lua").read_bytes(), source)
        self.assertTrue(source.startswith(b"-- ryz-app/1\n"))
        self.assertIn(b"-- @version: 1.0.0", source)
        self.assertNotIn(b"placeholder", source)
        self.assertLessEqual(len(source), 16384)

    def test_ordinary_host_does_not_report_device_acceptance(self):
        subprocess.run(["sh", "tools/build_app_host.sh"], cwd=ROOT,
                       check=True, capture_output=True, timeout=60)
        result = subprocess.run([str(ROOT / "build-host/app-host"),
                                 str(ROOT / "fs/tool_hardware.lua")],
                                input="pointer 20 down 80 112\npointer 60 up\nend 200\n",
                                text=True, capture_output=True, timeout=10)
        summary = [json.loads(line) for line in result.stdout.splitlines()][-1]
        self.assertTrue(summary["summary"])
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["phase"], "unsupported")


if __name__ == "__main__":
    unittest.main()
