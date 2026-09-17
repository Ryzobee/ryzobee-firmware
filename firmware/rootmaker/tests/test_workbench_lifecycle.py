#!/usr/bin/env python3
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "ryz_workbench"


class WorkbenchLifecycleHostTest(unittest.TestCase):
    def test_completion_is_ready_before_deferred_render(self):
        compiler = os.environ.get("CC", "cc")
        with tempfile.TemporaryDirectory(prefix="ryz-workbench-lifecycle-") as directory:
            binary = Path(directory) / "workbench_lifecycle_test"
            subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(COMPONENT),
                    str(COMPONENT / "workbench_lifecycle.c"),
                    str(ROOT / "tests" / "workbench_lifecycle_test.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            completed = subprocess.run(
                [str(binary)], check=False, capture_output=True, text=True
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("WORKBENCH_LIFECYCLE_PASS", completed.stdout)


if __name__ == "__main__":
    unittest.main()
