"""Real network CLI dispatch with fake Board; never instantiate a serial port."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import Mock, patch


SOURCE = Path(__file__).resolve().parents[1] / "tools/board_lua.py"
spec = importlib.util.spec_from_file_location("network_board_cli", SOURCE)
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


class NetworkCliTest(unittest.TestCase):
    def tearDown(self):
        serial_stub.Serial.assert_not_called()

    def test_status_only_reads_without_info_or_implicit_initialization(self):
        report = {"ok": True, "schema": "ryz-network/1", "network_valid": False}
        board = FakeBoard([report])
        self.assertIs(cli.call_network(board, "status"), report)
        self.assertEqual(board.calls, [("network", {"action": "status"})])

    def test_mutations_read_current_boot_and_submit_once_with_explicit_confirmation(self):
        for action in ("on", "off", "setup", "reprovision"):
            with self.subTest(action=action):
                report = {"ok": True, "accepted": True, "operation_id": 17}
                board = FakeBoard([{"ok": True, "boot_id": "current-boot"}, report])
                self.assertIs(cli.call_network(board, action, confirm=action == "reprovision"), report)
                fields = {"action": action, "boot_id": "current-boot"}
                if action == "reprovision":
                    fields["confirm"] = True
                self.assertEqual(board.calls, [("info", {}), ("network", fields)])

    def test_bad_parameters_never_make_transport_calls(self):
        board = FakeBoard([])
        for action in ("cancel", "forget", "ON", "", None):
            with self.assertRaises(ValueError):
                cli.call_network(board, action)
        with self.assertRaises(ValueError):
            cli.call_network(board, "reprovision")
        for action in ("status", "on", "off", "setup"):
            with self.assertRaises(ValueError):
                cli.call_network(board, action, confirm=True)
        for confirm in (1, 0, "true", None):
            with self.assertRaises(ValueError):
                cli.call_network(board, "reprovision", confirm=confirm)
        self.assertEqual(board.calls, [])

    def test_invalid_info_blocks_mutation_and_does_not_fall_back(self):
        for info in ({"ok": False}, {"ok": True}, {"ok": True, "boot_id": ""},
                     {"ok": True, "boot_id": 1}, {"ok": True, "boot_id": None}):
            for action in ("on", "off", "setup", "reprovision"):
                with self.subTest(info=info, action=action):
                    board = FakeBoard([info])
                    with self.assertRaises((AssertionError, ValueError)):
                        cli.call_network(board, action, confirm=action == "reprovision")
                    self.assertEqual(board.calls, [("info", {})])

    def test_rejected_and_unknown_ack_have_no_retry_reset_or_lua_fallback(self):
        for action in ("on", "off", "setup", "reprovision"):
            for reply in ({"ok": False, "error_code": 259}, TimeoutError("unknown ACK")):
                with self.subTest(action=action, reply=reply):
                    board = FakeBoard([{"ok": True, "boot_id": "boot-a"}, reply])
                    if isinstance(reply, Exception):
                        with self.assertRaises(TimeoutError):
                            cli.call_network(board, action, confirm=action == "reprovision")
                    else:
                        self.assertIs(cli.call_network(board, action,
                                                       confirm=action == "reprovision"), reply)
                    self.assertEqual([op for op, _ in board.calls], ["info", "network"])

    def invoke(self, arguments, board):
        with patch.object(cli, "Board", return_value=board) as factory, \
                patch.object(sys, "argv", [str(SOURCE), "--port", "FAKE", *arguments]), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = cli.main()
            factory.assert_called_once_with("FAKE")
        self.assertTrue(board.closed)
        return result

    def test_cli_dispatches_all_actions_and_does_not_wait_for_completion(self):
        for action in ("status", "on", "off", "setup", "reprovision"):
            with self.subTest(action=action):
                info = [] if action == "status" else [{"ok": True, "boot_id": "boot-a"}]
                reply = {"ok": True, "accepted": True, "operation_id": 4}
                board = FakeBoard([*info, reply])
                arguments = ["network", action]
                if action == "reprovision":
                    arguments.append("--confirm")
                self.assertEqual(self.invoke(arguments, board), 0)
                fields = {"action": action}
                if action != "status":
                    fields["boot_id"] = "boot-a"
                if action == "reprovision":
                    fields["confirm"] = True
                self.assertEqual(board.calls[-1], ("network", fields))
                self.assertEqual(len(board.calls), len(info) + 1)

    def test_cli_unknown_or_failed_mutation_closes_board_without_retry(self):
        for reply in ({"ok": False}, TimeoutError("unknown ACK")):
            board = FakeBoard([{"ok": True, "boot_id": "boot-a"}, reply])
            self.assertEqual(self.invoke(["network", "reprovision", "--confirm"], board), 1)
            self.assertEqual(len(board.calls), 2)

    def test_missing_confirmation_or_unrelated_cli_fields_fail_before_board_construction(self):
        for arguments in (["network"], ["network", "reprovision"],
                          ["network", "reprovision", "--confirm", "false"],
                          ["network", "on", "--confirm"], ["network", "off", "--confirm"],
                          ["network", "setup", "--confirm"], ["network", "setup", "--ssid", "x"],
                          ["network", "status", "--boot-id", "x"],
                          ["network", "off", "--password", "secret"], ["network", "cancel"]):
            with self.subTest(arguments=arguments), patch.object(cli, "Board") as factory, \
                    patch.object(sys, "argv", [str(SOURCE), "--port", "FAKE", *arguments]), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                cli.main()
            self.assertEqual(raised.exception.code, 2)
            factory.assert_not_called()


if __name__ == "__main__":
    unittest.main()
