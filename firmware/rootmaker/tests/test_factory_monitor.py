"""Packaging and honest Host capability boundary; pixels have CTest fixtures."""
import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FactoryMonitorTest(unittest.TestCase):
    def test_generated_factory_copy_is_current_and_within_existing_quota(self):
        subprocess.run(["node", "tools/build_factory_monitor.mjs", "--check"],
                       cwd=ROOT, check=True, capture_output=True, timeout=10)
        source = (ROOT / "fs/tool_monitor.lua").read_bytes()
        self.assertTrue(source.startswith(b"-- ryz-app/1\n"))
        self.assertIn(b"-- @version: 2.0.0", source)
        self.assertNotIn(b"placeholder", source)
        self.assertLessEqual(len(source), 16384)

    def test_normal_studio_host_does_not_claim_hardware_success(self):
        subprocess.run(["sh", "tools/build_app_host.sh"], cwd=ROOT,
                       check=True, capture_output=True, timeout=60)
        result = subprocess.run([str(ROOT / "build-host/app-host"),
                                 str(ROOT / "fs/tool_monitor.lua")],
                                input="end 200\n", text=True, capture_output=True, timeout=10)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        summary = rows[-1]
        self.assertTrue(summary["summary"])
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["phase"], "unsupported")
        self.assertIn("Hardware tools are unavailable", summary["error"])


if __name__ == "__main__":
    unittest.main()
