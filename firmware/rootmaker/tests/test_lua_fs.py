"""Real frozen Lua + application facade contract; no physical flash involved."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LuaFsTest(unittest.TestCase):
    def test_public_lua_storage_contract(self):
        with tempfile.TemporaryDirectory(prefix="ryz-lua-fs-") as directory:
            binary = Path(directory) / "lua-fs"
            include = [ROOT / "host"]
            include += [ROOT / "components" / name / "include" for name in (
                "ryz_runtime", "ryz_board", "ryz_lvgl", "ryz_tools",
                "ryz_i2c_scan", "ryz_rgb", "ryz_monitor")]
            lua = ROOT / "managed_components/georgik__lua"
            include += [lua / "include", lua / "lua"]
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
                "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-DMAKE_LIB",
                "-include", str(ROOT / "host/sdkconfig.h"),
                *[flag for path in include for flag in ("-I", str(path))],
                str(ROOT / "tests/lua_fs_test.c"),
                str(ROOT / "components/ryz_runtime/app_runtime.c"),
                str(ROOT / "components/ryz_runtime/app_tools.c"),
                str(lua / "lua/onelua.c"), "-lm", "-o", str(binary)], check=True, timeout=45)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("LUA_FS_PASS", result.stdout)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
