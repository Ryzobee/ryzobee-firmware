"""Run the actual public fs example across processes against the real core.

POSIX temporary files are host persistence evidence, not SPIFFS power-loss or
physical-device evidence. Reuse the storage suite's single platform adapter.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DEMO = ROOT / "scripts/fs_demo.lua"


class LuaFsPersistenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-lua-fs-build-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "lua-fs-persistence"
        include = [ROOT / "host", ROOT / "tests/app_fs_stubs", ROOT / "components/ryz_app_fs"]
        include += [ROOT / "components" / name / "include" for name in (
            "ryz_runtime", "ryz_board", "ryz_lvgl", "ryz_tools",
            "ryz_i2c_scan", "ryz_rgb", "ryz_monitor")]
        lua = ROOT / "managed_components/georgik__lua"
        include += [lua / "include", lua / "lua"]
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
            "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined", "-DMAKE_LIB",
            "-include", str(ROOT / "host/sdkconfig.h"),
            *[flag for path in include for flag in ("-I", str(path))],
            str(ROOT / "tests/lua_fs_persistence_test.c"),
            str(ROOT / "tests/app_fs_stubs/app_fs_host.c"),
            str(ROOT / "components/ryz_app_fs/ryz_app_fs.c"),
            str(ROOT / "components/ryz_runtime/app_runtime.c"),
            str(ROOT / "components/ryz_runtime/app_tools.c"),
            str(lua / "lua/onelua.c"), "-lm", "-o", str(cls.binary)], check=True, timeout=60)

    def invoke(self, root, mode, expected=0):
        result = subprocess.run([str(self.binary), str(root), mode, str(DEMO)],
            capture_output=True, text=True, timeout=30,
            env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    @staticmethod
    def snapshot(root):
        return {path.name: path.read_bytes() for path in Path(root).iterdir() if path.is_file()}

    def test_actual_demo_persists_across_processes_and_isolates_apps(self):
        with tempfile.TemporaryDirectory(prefix="ryz-lua-fs-data-") as root:
            first = self.invoke(root, "demo")
            self.assertIn("Persistent counter: 1\n", first.stdout)
            self.assertIn("counter.txt: 1 bytes\n", first.stdout)
            self.assertIn("App data: 1 / 32768 bytes\n", first.stdout)
            before = self.snapshot(root)
            self.assertTrue(before)
            self.assertIn("ISOLATION_PASS", self.invoke(root, "foreign").stdout)
            self.assertEqual(self.snapshot(root), before)
            second = self.invoke(root, "demo")
            self.assertIn("Persistent counter: 2\n", second.stdout)
            self.assertIn("counter.txt: 1 bytes\n", second.stdout)
            self.assertIn("App data: 1 / 32768 bytes\n", second.stdout)
            self.assertNotIn("scratch.bin:", second.stdout)

    def test_actual_demo_does_not_overwrite_semantically_invalid_saved_counter(self):
        with tempfile.TemporaryDirectory(prefix="ryz-lua-fs-data-") as root:
            self.invoke(root, "demo")
            self.invoke(root, "invalid_counter")
            before = self.snapshot(root)
            result = self.invoke(root, "demo", expected=3)
            self.assertIn("PHASE runtime", result.stdout)
            self.assertIn("Invalid counter; not overwriting it", result.stderr)
            self.assertNotIn("Persistent counter:", result.stdout)
            self.assertEqual(self.snapshot(root), before)
            self.assertIn("INVALID_COUNTER_PRESERVED", self.invoke(root, "check_invalid").stdout)

    def test_actual_demo_does_not_reset_storage_corruption(self):
        with tempfile.TemporaryDirectory(prefix="ryz-lua-fs-data-") as root:
            self.invoke(root, "demo")
            self.invoke(root, "demo")
            self.invoke(root, "corrupt_record")
            before = self.snapshot(root)
            result = self.invoke(root, "demo", expected=3)
            self.assertIn("Cannot load counter: corrupt", result.stderr)
            self.assertNotIn("Persistent counter:", result.stdout)
            self.assertEqual(self.snapshot(root), before)


if __name__ == "__main__":
    unittest.main()
