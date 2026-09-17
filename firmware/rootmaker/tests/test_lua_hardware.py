"""Actual C Lua facade and device Adapter over deterministic peripheral peers."""
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
READY = b"LUA_HARDWARE_READY\n"


def run_after_ready(command, startup_timeout=15, execution_timeout=3):
    """Keep sanitizer/process startup separate from the actual test deadline.

    On macOS a freshly linked ASan executable can spend over three seconds
    before main, while the warmed suite takes about 60 ms. The C peer does no
    test work before READY and blocks for GO; startup remains bounded and a
    stuck Lua/cleanup case still has the original three-second deadline.
    """
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE,
                          env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"}) as process:
        try:
            ready = bytearray()
            deadline = time.monotonic() + startup_timeout
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while len(ready) < len(READY):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not selector.select(remaining):
                        raise subprocess.TimeoutExpired(command, startup_timeout,
                                                        output=bytes(ready))
                    chunk = os.read(process.stdout.fileno(), len(READY) - len(ready))
                    if not chunk:
                        raise RuntimeError("test executable closed stdout before READY")
                    ready.extend(chunk)
                    if not READY.startswith(ready):
                        raise RuntimeError("invalid test readiness response: " + repr(ready))
            stdout, stderr = process.communicate(input=b"\n", timeout=execution_timeout)
            return subprocess.CompletedProcess(command, process.returncode,
                                               stdout.decode(errors="replace"),
                                               stderr.decode(errors="replace"))
        except BaseException as error:
            if process.poll() is None:
                process.kill()
            stdout, stderr = process.communicate()
            if isinstance(error, subprocess.TimeoutExpired):
                error.output = bytes(ready) + stdout
                error.stderr = stderr
            elif isinstance(error, RuntimeError):
                raise RuntimeError(str(error) + "; stdout=" + repr(stdout) +
                                   "; stderr=" + repr(stderr)) from error
            raise


class LuaHardwareTest(unittest.TestCase):
    def test_slow_process_start_does_not_spend_the_execution_budget(self):
        peer = "import sys,time; time.sleep(.4); print('LUA_HARDWARE_READY',flush=True); " \
               "assert sys.stdin.readline()=='\\n'; print('EXECUTED')"
        result = run_after_ready([sys.executable, "-c", peer],
                                 startup_timeout=2, execution_timeout=.2)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "EXECUTED\n")

    def test_a_stuck_test_after_ready_still_times_out(self):
        peer = "import sys,time; print('LUA_HARDWARE_READY',flush=True); " \
               "assert sys.stdin.readline()=='\\n'; time.sleep(10)"
        with self.assertRaises(subprocess.TimeoutExpired) as error:
            run_after_ready([sys.executable, "-c", peer],
                            startup_timeout=2, execution_timeout=.2)
        self.assertEqual(error.exception.timeout, .2)

    def test_a_process_that_never_becomes_ready_still_times_out(self):
        with self.assertRaises(subprocess.TimeoutExpired) as error:
            run_after_ready([sys.executable, "-c", "import time; time.sleep(10)"],
                            startup_timeout=.2, execution_timeout=3)
        self.assertEqual(error.exception.timeout, .2)

    def test_device_adapter_and_lua_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="ryz-lua-hardware-") as directory:
            binary = Path(directory) / "hardware"
            include = [ROOT / "tests/imu_stubs", ROOT / "host"]
            include += [ROOT / "components" / name / "include" for name in (
                "ryz_runtime", "ryz_board", "ryz_lvgl", "ryz_tools", "ryz_i2c_scan",
                "ryz_rgb", "ryz_monitor", "ryz_provisioning", "ryz_ble")]
            lua = ROOT / "managed_components/georgik__lua"
            include += [lua / "include", lua / "lua"]
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
                "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-DMAKE_LIB",
                "-include", str(ROOT / "host/sdkconfig.h"),
                *[flag for path in include for flag in ("-I", str(path))],
                str(ROOT / "tests/lua_hardware_test.c"),
                *[str(ROOT / "components/ryz_runtime" / name) for name in (
                    "app_runtime.c", "app_tools.c", "lua_hardware_esp.c")],
                str(lua / "lua/onelua.c"), "-lm", "-o", str(binary)], check=True, timeout=45)
            try:
                result = run_after_ready([str(binary), "--wait-for-start"])
            except subprocess.TimeoutExpired as error:
                self.fail(f"hardware process exceeded {error.timeout}s phase budget; "
                          f"stdout={error.output!r}; stderr={error.stderr!r}")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("LUA_HARDWARE_PASS", result.stdout)

    def test_boot_does_not_start_or_wait_for_imu(self):
        source = (ROOT / "components/ryz_workbench/workbench.c").read_text()
        self.assertNotIn("ryz_sensors_start(", source)
        self.assertNotIn("ryz_imu_init(", source)


if __name__ == "__main__":
    unittest.main()
