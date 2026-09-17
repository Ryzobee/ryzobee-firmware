"""Run ordinary user Lua against deterministic virtual peripheral peers.

This checks the public Lua contract, not physical electrical behaviour.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LuaPeripheralsTest(unittest.TestCase):
    def test_public_lua_contract(self):
        with tempfile.TemporaryDirectory(prefix="ryz-lua-peripherals-") as directory:
            binary = Path(directory) / "peripherals"
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
                str(ROOT / "tests/lua_peripherals_test.c"),
                str(ROOT / "components/ryz_runtime/app_runtime.c"),
                str(ROOT / "components/ryz_runtime/app_tools.c"),
                str(lua / "lua/onelua.c"), "-lm", "-o", str(binary)], check=True, timeout=45)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("LUA_PERIPHERALS_PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
