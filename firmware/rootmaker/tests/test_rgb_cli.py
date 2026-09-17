"""Real RGB CLI dispatch with fake Board; constructing real serial is forbidden."""
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
spec = importlib.util.spec_from_file_location("rgb_board_cli", SOURCE)
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


class RgbCliTest(unittest.TestCase):
    def tearDown(self):
        serial_stub.Serial.assert_not_called()

    def test_status_only_reads_status_without_info_or_implicit_send(self):
        report = {"ok": True, "phase": "unstarted", "output_known": False, "color": None}
        board = FakeBoard([report])
        self.assertIs(cli.call_rgb(board, "status"), report)
        self.assertEqual(board.calls, [("rgb", {"action": "status"})])

    def test_set_preserves_raw_rgb_channels_and_off_is_an_explicit_request(self):
        board = FakeBoard([{"ok": True, "boot_id": "boot-a"},
                           {"ok": True, "accepted": True, "request_id": 7}])
        self.assertTrue(cli.call_rgb(board, "set", 255, 97, 0)["accepted"])
        self.assertEqual(board.calls, [("info", {}), ("rgb", {
            "action": "set", "boot_id": "boot-a", "red": 255, "green": 97, "blue": 0})])
        board = FakeBoard([{"ok": True, "boot_id": "boot-b"}, {"ok": True}])
        self.assertTrue(cli.call_rgb(board, "off")["ok"])
        self.assertEqual(board.calls, [("info", {}), ("rgb", {"action": "off", "boot_id": "boot-b"})])

    def test_unknown_ack_does_not_retry_or_reset_or_use_lua(self):
        for action, color in (("set", (0, 255, 128)), ("off", ())):
            with self.subTest(action=action):
                board = FakeBoard([{"ok": True, "boot_id": "boot-a"}, TimeoutError("unknown ACK")])
                with self.assertRaises(TimeoutError):
                    cli.call_rgb(board, action, *color)
                self.assertEqual(len(board.calls), 2)
                self.assertEqual([call[0] for call in board.calls], ["info", "rgb"])

    def test_missing_or_rejected_boot_blocks_send_and_rejected_rgb_has_no_fallback(self):
        for info in ({"ok": False}, {"ok": True}, {"ok": True, "boot_id": ""},
                     {"ok": True, "boot_id": 1}, {"ok": True, "boot_id": None}):
            for action, color in (("set", (1, 2, 3)), ("off", ())):
                with self.subTest(info=info, action=action):
                    board = FakeBoard([info])
                    with self.assertRaises((AssertionError, ValueError)):
                        cli.call_rgb(board, action, *color)
                    self.assertEqual(board.calls, [("info", {})])
        board = FakeBoard([{"ok": True, "boot_id": "boot-a"}, {"ok": False, "error": "stale boot"}])
        self.assertFalse(cli.call_rgb(board, "off")["ok"])
        self.assertEqual(len(board.calls), 2)

    def test_bad_helper_parameters_cause_no_transport_calls(self):
        board = FakeBoard([])
        for channel in range(3):
            for invalid in (-1, 256, 1.5, "7", True, None):
                channels = [0, 0, 0]
                channels[channel] = invalid
                with self.subTest(channel=channel, invalid=invalid), self.assertRaises(ValueError):
                    cli.call_rgb(board, "set", *channels)
        for action, color in (("blink", ()), ("off", (0,)), ("status", (0, 0, 0))):
            with self.assertRaises(ValueError):
                cli.call_rgb(board, action, *color)
        self.assertEqual(board.calls, [])
        self.assertEqual(cli.rgb_channel_argument("0"), 0)
        self.assertEqual(cli.rgb_channel_argument("255"), 255)
        for invalid in ("-1", "256", "1.5", "NaN", "red"):
            with self.assertRaises(argparse.ArgumentTypeError):
                cli.rgb_channel_argument(invalid)

    def invoke(self, arguments, board):
        with patch.object(cli, "Board", return_value=board) as factory, \
                patch.object(sys, "argv", [str(SOURCE), "--port", "FAKE", *arguments]), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            result = cli.main()
            factory.assert_called_once_with("FAKE")
        self.assertTrue(board.closed)
        return result

    def test_real_cli_dispatches_rgb_set_off_status_and_closes_fake_board(self):
        cases = [(["rgb", "set", "255", "97", "0"], "set"),
                 (["rgb", "off"], "off"), (["rgb", "status"], "status")]
        for arguments, action in cases:
            with self.subTest(action=action):
                replies = [] if action == "status" else [{"ok": True, "boot_id": "boot-a"}]
                board = FakeBoard([*replies, {"ok": True}])
                self.assertEqual(self.invoke(arguments, board), 0)
                self.assertEqual(board.calls[-1][0], "rgb")
                self.assertEqual(board.calls[-1][1]["action"], action)
                if action == "set":
                    self.assertEqual(board.calls[-1][1], {"action": "set", "boot_id": "boot-a",
                                                         "red": 255, "green": 97, "blue": 0})

    def test_cli_failed_or_unknown_submit_exits_once_and_closes(self):
        for response in ({"ok": False, "error": "busy"}, TimeoutError("unknown ACK")):
            board = FakeBoard([{"ok": True, "boot_id": "boot-a"}, response])
            self.assertEqual(self.invoke(["rgb", "off"], board), 1)
            self.assertEqual(len(board.calls), 2)

    def test_invalid_cli_arguments_fail_before_board_construction(self):
        cases = [["rgb"], ["rgb", "set", "1", "2"],
                 ["rgb", "set", "-1", "2", "3"], ["rgb", "set", "1", "256", "3"],
                 ["rgb", "set", "1", "2", "1.5"], ["rgb", "off", "0"],
                 ["rgb", "status", "--red", "1"], ["rgb", "cleanup"],
                 ["rgb", "cleanup", "--boot-id", "saved", "--request-id", "0"],
                 ["rgb", "cleanup", "--boot-id", "saved", "--request-id", "4294967296"],
                 ["rgb", "cleanup", "--boot-id", "saved", "--request-id", "1.5"],
                 ["rgb", "cleanup", "--boot-id", "", "--request-id", "31"],
                 ["rgb", "cleanup", "--boot-id", "saved", "--request-id", "31", "--red", "0"]]
        for arguments in cases:
            with self.subTest(arguments=arguments), patch.object(cli, "Board") as factory, \
                    patch.object(sys, "argv", [str(SOURCE), "--port", "FAKE", *arguments]), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                cli.main()
            self.assertEqual(raised.exception.code, 2)
            factory.assert_not_called()

    def test_cleanup_preserves_saved_token_and_has_no_read_takeover_or_retry(self):
        for result in ({"ok": True,"accepted":True,"request_id":31},
                       {"ok": False,"error":"owned by app"}, TimeoutError("unknown ACK")):
            board = FakeBoard([result])
            if isinstance(result, Exception):
                with self.assertRaises(TimeoutError):
                    cli.call_rgb(board,"cleanup",boot_id="saved",request_id=31)
            else:
                self.assertIs(cli.call_rgb(board,"cleanup",boot_id="saved",request_id=31),result)
            self.assertEqual(board.calls,[("rgb",{"action":"cleanup","boot_id":"saved","request_id":31})])
        board = FakeBoard([{"ok": True}])
        self.assertEqual(self.invoke(["rgb","cleanup","--boot-id","saved","--request-id","4294967295"],board),0)
        self.assertEqual(board.calls,[("rgb",{"action":"cleanup","boot_id":"saved","request_id":4294967295})])

    def test_cleanup_invalid_helper_tokens_never_open_transport(self):
        board = FakeBoard([])
        for token in (None,0,-1,4294967296,1.5,True,"31"):
            with self.assertRaises(ValueError):
                cli.call_rgb(board,"cleanup",boot_id="saved",request_id=token)
        for boot in (None,"",31):
            with self.assertRaises(ValueError):
                cli.call_rgb(board,"cleanup",boot_id=boot,request_id=31)
        with self.assertRaises(ValueError):
            cli.call_rgb(board,"cleanup",0,0,0,boot_id="saved",request_id=31)
        for action in ("set","off","status"):
            with self.assertRaises(ValueError):
                cli.call_rgb(board,action,request_id=31)
        self.assertEqual(board.calls,[])


if __name__ == "__main__":
    unittest.main()
