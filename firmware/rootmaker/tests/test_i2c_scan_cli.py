"""Real CLI parsing/control with a fake transport; never imports/opens a port."""
import argparse
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import Mock, patch


SOURCE = Path(__file__).resolve().parents[1] / "tools/board_lua.py"
spec = importlib.util.spec_from_file_location("scan_board_cli", SOURCE)
cli = importlib.util.module_from_spec(spec)
serial_stub = types.ModuleType("serial")
serial_stub.Serial = Mock(side_effect=AssertionError("real serial construction is forbidden"))
with patch.dict(sys.modules, {"serial": serial_stub}):
    spec.loader.exec_module(cli)


class FakeBoard:
    def __init__(self, replies):
        self.replies = iter(replies)
        self.calls = []
        self.events = []
        self.closed = False

    def call(self, op, **fields):
        self.calls.append((op, fields))
        response = next(self.replies)
        if isinstance(response, Exception):
            raise response
        self.events.append(response)
        return response

    def close(self):
        self.closed = True


class I2cScanCliTest(unittest.TestCase):
    def tearDown(self):
        serial_stub.Serial.assert_not_called()

    def test_status_only_copies_server_status_without_start_or_info(self):
        board = FakeBoard([{"ok": True, "phase": "unstarted", "empty": False}])
        self.assertFalse(cli.call_i2c_scan(board, "status")["empty"])
        self.assertEqual(board.calls, [("i2c_scan", {"action": "status"})])

    def test_start_uses_current_boot_and_never_retries_unknown_ack(self):
        board = FakeBoard([{"ok": True, "boot_id": "boot-a"}, TimeoutError("lost ack")])
        with self.assertRaises(TimeoutError):
            cli.call_i2c_scan(board, "start")
        self.assertEqual(board.calls, [("info", {}),
            ("i2c_scan", {"action": "start", "boot_id": "boot-a"})])

    def test_missing_boot_or_old_firmware_cannot_fallback_to_raw_i2c(self):
        for info in ({"ok": False}, {"ok": True},
                     {"ok": True, "boot_id": ""}, {"ok": True, "boot_id": 1}):
            with self.subTest(info=info):
                board = FakeBoard([info])
                with self.assertRaises((AssertionError, ValueError)):
                    cli.call_i2c_scan(board, "start")
                self.assertEqual(board.calls, [("info", {})])
        board = FakeBoard([{"ok": True, "boot_id": "boot-a"},
                           {"ok": False, "error": "unknown op"}])
        self.assertFalse(cli.call_i2c_scan(board, "start")["ok"])
        self.assertEqual(len(board.calls), 2)

    def test_cancel_preserves_original_boot_without_refresh_or_retry(self):
        board = FakeBoard([{"ok": False, "error": "stale boot"}])
        self.assertFalse(cli.call_i2c_scan(board, "cancel", 7, "old-boot")["ok"])
        self.assertEqual(board.calls, [("i2c_scan", {
            "action": "cancel", "scan_id": 7, "boot_id": "old-boot"})])
        board = FakeBoard([TimeoutError("cancel ack unknown")])
        with self.assertRaises(TimeoutError):
            cli.call_i2c_scan(board, "cancel", 7, "old-boot")
        self.assertEqual(len(board.calls), 1)

    def test_invalid_tokens_or_action_send_nothing(self):
        board = FakeBoard([])
        for token in (None, False, 0, -1, 1.2, "7", 0x100000000):
            with self.subTest(token=token), self.assertRaises(ValueError):
                cli.call_i2c_scan(board, "cancel", token, "boot-a")
        for boot in (None, "", 123):
            with self.subTest(boot=boot), self.assertRaises(ValueError):
                cli.call_i2c_scan(board, "cancel", 7, boot)
        with self.assertRaises(ValueError):
            cli.call_i2c_scan(board, "configure")
        self.assertEqual(board.calls, [])
        self.assertEqual(cli.scan_id_argument("4294967295"), 0xFFFFFFFF)
        for value in ("1.2", "0", "-1", "4294967296"):
            with self.assertRaises(argparse.ArgumentTypeError):
                cli.scan_id_argument(value)

    def invoke(self, arguments, board):
        with patch.object(cli, "Board", return_value=board) as factory, \
                patch.object(sys, "argv", [str(SOURCE), "--port", "FAKE", *arguments]), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            result = cli.main()
            factory.assert_called_once_with("FAKE")
        self.assertTrue(board.closed)
        return result

    def test_real_cli_dispatches_start_status_cancel(self):
        cases = [(["i2c-scan", "status"], [{"ok": True}], "status"),
                 (["i2c-scan", "start"], [{"ok": True, "boot_id": "a"},
                                          {"ok": True, "scan_id": 1}], "start"),
                 (["i2c-scan", "cancel", "--scan-id", "1", "--boot-id", "a"],
                  [{"ok": True}], "cancel")]
        for argv, responses, action in cases:
            with self.subTest(action=action):
                board = FakeBoard(responses)
                self.assertEqual(self.invoke(argv, board), 0)
                self.assertEqual(board.calls[-1][1]["action"], action)

    def test_real_cli_rejection_and_unknown_ack_fail_and_close(self):
        for response in ({"ok": False, "error": "busy"}, TimeoutError("unknown")):
            with self.subTest(response=response):
                board = FakeBoard([response])
                self.assertEqual(self.invoke(["i2c-scan", "status"], board), 1)
                self.assertEqual(len(board.calls), 1)

    def test_invalid_cli_cancel_is_rejected_before_transport_creation(self):
        with patch.object(cli, "Board") as factory, \
                patch.object(sys, "argv", [str(SOURCE), "--port", "FAKE", "i2c-scan", "cancel"]), \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            cli.main()
        self.assertEqual(raised.exception.code, 2)
        factory.assert_not_called()

    def test_configure_reads_same_boot_v2_revision_then_submits_once_without_scan(self):
        board = FakeBoard([{"ok": True, "boot_id": "boot-a"},
                           {"ok": True, "schema": "ryz-i2c-scan/2", "boot_id": "boot-a", "config_revision": 7},
                           {"ok": True, "accepted": True, "config_revision": 8}])
        self.assertEqual(cli.call_i2c_configure(board, 4, 5, 400000)["config_revision"], 8)
        self.assertEqual(board.calls, [("info", {}),
            ("i2c_scan", {"action": "status", "boot_id": "boot-a"}),
            ("i2c_scan", {"action": "configure", "boot_id": "boot-a", "sda": 4, "scl": 5,
                          "hz": 400000, "expected_revision": 7})])

    def test_configure_reboot_old_schema_bad_revision_and_status_failure_block_commit(self):
        valid = {"ok": True, "schema": "ryz-i2c-scan/2", "boot_id": "boot-a", "config_revision": 1}
        bad = [{**valid, "boot_id": "new-boot"}, {**valid, "schema": "ryz-i2c-scan/1"},
               {**valid, "ok": False}, {"ok": True}]
        bad.extend({**valid, "config_revision": revision} for revision in
                   (None, True, 0, -1, 1.5, "1", 0x100000000))
        for status in bad:
            with self.subTest(status=status):
                board = FakeBoard([{"ok": True, "boot_id": "boot-a"}, status])
                with self.assertRaises((AssertionError, ValueError)):
                    cli.call_i2c_configure(board, 4, 5, 100000)
                self.assertEqual(len(board.calls), 2)
                self.assertEqual(board.calls[-1], ("i2c_scan", {"action": "status", "boot_id": "boot-a"}))
        for info in ({"ok": False}, {"ok": True}, {"ok": True, "boot_id": ""}, {"ok": True, "boot_id": 1}):
            board = FakeBoard([info])
            with self.assertRaises((AssertionError, ValueError)):
                cli.call_i2c_configure(board, 4, 5, 100000)
            self.assertEqual(board.calls, [("info", {})])

    def test_configure_unknown_ack_or_stale_cas_is_not_retried(self):
        for final in (TimeoutError("unknown configuration ACK"), {"ok": False, "error": "stale revision"}):
            board = FakeBoard([{"ok": True, "boot_id": "boot-a"},
                               {"ok": True, "schema": "ryz-i2c-scan/2", "boot_id": "boot-a", "config_revision": 9}, final])
            if isinstance(final, Exception):
                with self.assertRaises(TimeoutError):
                    cli.call_i2c_configure(board, 4, 5, 100000)
            else:
                self.assertFalse(cli.call_i2c_configure(board, 4, 5, 100000)["ok"])
            self.assertEqual(len(board.calls), 3)
            self.assertEqual([fields["action"] for op, fields in board.calls if op == "i2c_scan"], ["status", "configure"])

    def test_configure_invalid_pins_or_clock_never_creates_transport_calls(self):
        board = FakeBoard([])
        for channel in range(3):
            invalid = (None, False, "4", 1.5, -1, 49) if channel < 2 else (None, True, "100000", 200000, 0)
            for value in invalid:
                params = [4, 5, 100000]
                params[channel] = value
                with self.subTest(params=params), self.assertRaises(ValueError):
                    cli.call_i2c_configure(board, *params)
        self.assertEqual(board.calls, [])
        self.assertEqual(cli.i2c_pin_argument("0"), 0)
        self.assertEqual(cli.i2c_pin_argument("48"), 48)
        for invalid in ("-1", "49", "4.5", "gpio4"):
            with self.assertRaises(argparse.ArgumentTypeError):
                cli.i2c_pin_argument(invalid)

    def test_real_cli_configure_dispatch_and_invalid_args_before_board(self):
        board = FakeBoard([{"ok": True, "boot_id": "boot-a"},
                           {"ok": True, "schema": "ryz-i2c-scan/2", "boot_id": "boot-a", "config_revision": 1},
                           {"ok": True}])
        self.assertEqual(self.invoke(["i2c-scan", "configure", "--sda", "41", "--scl", "40", "--hz", "100000"], board), 0)
        self.assertEqual(board.calls[-1], ("i2c_scan", {"action": "configure", "boot_id": "boot-a",
            "sda": 41, "scl": 40, "hz": 100000, "expected_revision": 1}))
        bad = [[], ["--sda", "49", "--scl", "4", "--hz", "100000"],
               ["--sda", "4", "--scl", "5", "--hz", "200000"],
               ["--sda", "4", "--scl", "5", "--hz", "100000", "--boot-id", "a"]]
        for arguments in bad:
            with self.subTest(arguments=arguments), patch.object(cli, "Board") as factory, \
                    patch.object(sys, "argv", [str(SOURCE), "--port", "FAKE", "i2c-scan", "configure", *arguments]), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                cli.main()
            self.assertEqual(raised.exception.code, 2)
            factory.assert_not_called()


if __name__ == "__main__":
    unittest.main()
