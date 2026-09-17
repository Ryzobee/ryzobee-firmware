import importlib.util
import io
import json
from pathlib import Path
from contextlib import redirect_stdout
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("ble_boot_probe", Path(__file__).resolve().parents[1] / "tools/ble_boot_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class FakeSerialWindow:
    """Only the serial and monotonic-clock seams are replaced."""
    def __init__(self, events):
        self.now = 0.0
        self.events = list(events)
        self.closed = False

    def open(self):
        pass

    def close(self):
        self.closed = True

    def reset_input_buffer(self):
        pass

    def write(self, data):
        return len(data)

    def flush(self):
        pass

    def sleep(self, seconds):
        self.now += seconds

    def readline(self):
        self.now += .25
        if self.events and self.events[0][0] <= self.now:
            _, line = self.events.pop(0)
            return (line + "\n").encode()
        return b""


class BleBootProbeTest(unittest.TestCase):
    def info_line(self, **changes):
        return "RYZOBEE_RPC " + json.dumps({"id": probe.REQUEST_ID, "ok": True,
            "ble": {"snapshot_error": 0, "available": True, "boot_settled": True,
                    "phase": 0, "boot_error": 0, "checking_state": False}, **changes})

    def run_window(self, events, *, reset=False):
        serial = FakeSerialWindow(events)
        output = io.StringIO()
        argv = ["ble_boot_probe.py", "--port", "fixture", "--timeout", "5",
                "--expect-available"] + (["--reset"] if reset else [])
        with patch.object(probe.serial, "Serial", return_value=serial), \
             patch.object(probe.time, "monotonic", side_effect=lambda: serial.now), \
             patch.object(probe.time, "sleep", side_effect=serial.sleep), \
             patch("sys.argv", argv), redirect_stdout(output):
            result = probe.main()
        self.assertTrue(serial.closed)
        return result, json.loads(output.getvalue().splitlines()[-1])

    def test_successful_reply_expires_when_device_stops_responding(self):
        result, summary = self.run_window([(1, self.info_line())])
        self.assertEqual(result, 2)
        self.assertFalse(summary["ble_available"])

    def test_fatal_log_fails_the_window_even_after_fresh_recovery(self):
        for fatal in ("assert failed: startup", "Guru Meditation Error", "Backtrace: 0x40300000"):
            with self.subTest(fatal=fatal):
                result, summary = self.run_window([
                    (1, self.info_line()), (2, fatal), (4.5, self.info_line())])
                self.assertEqual(result, 2)
                self.assertFalse(summary["ble_available"])
                self.assertTrue(summary["fatal_seen"])

    def test_restart_discards_a_previously_fresh_reply(self):
        for restart in ("ESP-ROM:esp32s3", "rst:0x3 (SW_RESET)"):
            with self.subTest(restart=restart):
                result, summary = self.run_window([(3, self.info_line()), (4, restart)])
                self.assertEqual(result, 2)
                self.assertFalse(summary["ble_available"])
                self.assertEqual(summary["info"], {})

    def test_requested_initial_reset_and_recent_reply_can_pass(self):
        result, summary = self.run_window([
            (.25, "ESP-ROM:esp32s3"), (.5, "rst:0x1 (POWERON)"),
            (4.5, self.info_line())], reset=True)
        self.assertEqual(result, 0)
        self.assertTrue(summary["ble_available"])
        self.assertTrue(summary["reply_fresh"])
        self.assertFalse(summary["fatal_seen"])

    def test_unrelated_replies_do_not_refresh_expired_evidence(self):
        result, summary = self.run_window([
            (1, self.info_line()), (4.5, self.info_line(id="other-request"))])
        self.assertEqual(result, 2)
        self.assertFalse(summary["reply_fresh"])

    def test_new_boot_requires_its_own_reply(self):
        result, summary = self.run_window([
            (1, self.info_line()), (3, "ESP-ROM:esp32s3"), (4.5, self.info_line())])
        self.assertEqual(result, 0)
        self.assertTrue(summary["ble_available"])

    def test_freshness_window_includes_three_seconds_but_not_older(self):
        window = probe.ProbeWindow()
        window.observe_reply(json.loads(self.info_line().split("RYZOBEE_RPC ", 1)[1]), 10)
        self.assertTrue(window.passed(13))
        self.assertFalse(window.passed(13.001))

    def test_missing_starting_failed_and_unknown_are_not_success(self):
        self.assertFalse(probe.available({"ok": True}))
        good = {"ok": True, "ble": {"snapshot_error": 0, "available": True,
                "boot_settled": True, "phase": 0, "boot_error": 0, "checking_state": False}}
        self.assertTrue(probe.available(good))
        for key, value in (("available", False), ("boot_settled", False), ("phase", 13),
                           ("phase", None), ("phase", 99), ("snapshot_error", -1), ("checking_state", True)):
            with self.subTest(key=key):
                self.assertFalse(probe.available({"ok": True, "ble": {**good["ble"], key: value}}))
        # Saved-peer reconnect timeout is a permitted OFF fallback, not failed
        # controller initialization; report boot_error without conflating it.
        self.assertTrue(probe.available({"ok": True, "ble": {**good["ble"], "boot_error": 263}}))

    def test_only_whitelisted_non_sensitive_diagnostics_leave_probe(self):
        result = probe.safe_info({"ok": True, "password": "secret", "ble": {
            "available": False, "compare_value": 123456, "local_address": "secret",
            "init": {"finished": True, "buffers": {"returned": True, "result": 257,
                     "irk": "secret"}}}})
        self.assertNotIn("secret", str(result))
        self.assertNotIn("compare_value", str(result))
        self.assertEqual(result["ble"]["init"]["buffers"]["result"], 257)

    def test_logs_keep_failures_but_not_network_credentials(self):
        self.assertIsNone(probe.log_signal("wifi: password=secret"))
        self.assertIn("hci inits failed", probe.log_signal("E BLE_INIT: hci inits failed"))


if __name__ == "__main__":
    unittest.main()
