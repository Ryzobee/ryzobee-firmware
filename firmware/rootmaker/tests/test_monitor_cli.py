"""Real Monitor CLI with fake Board; constructing physical serial is forbidden."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import Mock, patch


SOURCE = Path(__file__).resolve().parents[1] / "tools/board_lua.py"
spec = importlib.util.spec_from_file_location("monitor_board_cli", SOURCE)
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


def status(**changes):
    return {"ok": True, "schema": "ryz-monitor/1", "boot_id": "boot-a",
            "config_revision": 7, "session_id": 31, **changes}


def auto_replies(response=None):
    return [{"ok": True, "boot_id": "boot-a"}, status(),
            {"ok": True, "accepted": True, "operation_id": 36} if response is None else response]


class MonitorCliTest(unittest.TestCase):
    def tearDown(self):
        serial_stub.Serial.assert_not_called()

    def test_status_only_reads_without_info_or_implicit_worker(self):
        response = status(phase="unstarted", session_id=0, config_revision=1)
        board = FakeBoard([response])
        self.assertIs(cli.call_monitor(board, "status"), response)
        self.assertEqual(board.calls, [("monitor", {"action": "status"})])

    def test_start_configure_use_boot_bound_cas_and_do_not_wait_for_completion(self):
        for action, fields in (("start", {}), ("configure", {"source": "system"}),
                               ("configure", {"source": "uart", "rx": 4, "tx": 5, "baud": 115200})):
            with self.subTest(action=action, fields=fields):
                board = FakeBoard(auto_replies())
                response = cli.call_monitor(board, action, **fields)
                self.assertTrue(response["accepted"])
                expected = {"action": action, "boot_id": "boot-a", "expected_revision": 7, **fields}
                if action == "configure":
                    expected["session_id"] = 31
                self.assertEqual(board.calls, [("info", {}),
                    ("monitor", {"action": "status", "boot_id": "boot-a"}), ("monitor", expected)])
        board = FakeBoard([{"ok": True, "boot_id": "boot-a"}, status(session_id=0, config_revision=1), {"ok": True}])
        cli.call_monitor(board, "configure", source="system")
        self.assertEqual(board.calls[-1][1]["session_id"], 0)

    def test_explicit_tokens_never_refresh_or_adopt_another_session(self):
        common = {"boot_id": "saved-boot", "session_id": 4294967295}
        for action, fields in (("stop", {}), ("pause", {"expected_generation": 9, "paused": True}),
                               ("pause", {"expected_generation": 9, "paused": False}),
                               ("clear", {"expected_generation": 9}),
                               ("read", {"view_generation": 9, "after_sequence": 4294967295}),
                               ("read", {"view_generation": 9})):
            with self.subTest(action=action, fields=fields):
                board = FakeBoard([{"ok": True}])
                cli.call_monitor(board, action, **common, **fields)
                expected = {"action": action, **common, **fields}
                if action == "read":
                    expected.setdefault("after_sequence", 0)
                self.assertEqual(board.calls, [("monitor", expected)])

    def test_reboot_schema_invalid_revision_and_bad_info_prevent_submission(self):
        bad_status = [status(ok=False), status(boot_id="boot-b"), status(schema="ryz-monitor/2"),
                      status(schema=None), *[status(config_revision=value) for value in
                          (0, -1, 4294967296, 1.5, "1", True, None)],
                      *[status(session_id=value) for value in (-1, 4294967296, 1.5, "1", True, None)]]
        for response in bad_status:
            with self.subTest(response=response):
                board = FakeBoard([{"ok": True, "boot_id": "boot-a"}, response])
                with self.assertRaises((ValueError, AssertionError)):
                    cli.call_monitor(board, "configure", source="system")
                self.assertEqual(len(board.calls), 2)
        for info in ({"ok": False}, {"ok": True}, {"ok": True, "boot_id": ""},
                     {"ok": True, "boot_id": 7}, {"ok": True, "boot_id": None}):
            board = FakeBoard([info])
            with self.assertRaises((ValueError, AssertionError)):
                cli.call_monitor(board, "start")
            self.assertEqual(board.calls, [("info", {})])

    def test_unknown_ack_and_rejections_do_not_retry_or_fallback(self):
        actions = [("start", {}), ("configure", {"source": "system"}),
                   ("stop", {"boot_id": "boot-a", "session_id": 31}),
                   ("pause", {"boot_id": "boot-a", "session_id": 31, "expected_generation": 9, "paused": True}),
                   ("clear", {"boot_id": "boot-a", "session_id": 31, "expected_generation": 9})]
        for action, fields in actions:
            for response in (TimeoutError("unknown ACK; inspect status"), {"ok": False, "error": "stale"}):
                with self.subTest(action=action, response=response):
                    replies = auto_replies(response) if action in ("start", "configure") else [response]
                    board = FakeBoard(replies)
                    if isinstance(response, Exception):
                        with self.assertRaises(TimeoutError):
                            cli.call_monitor(board, action, **fields)
                    else:
                        self.assertFalse(cli.call_monitor(board, action, **fields)["ok"])
                    self.assertEqual(len(board.calls), len(replies))
                    self.assertTrue(all(call[0] in ("info", "monitor") for call in board.calls))

    def test_invalid_helper_inputs_never_reach_transport(self):
        board = FakeBoard([])
        invalid = [("watch", {}), ("status", {"limit": 8}), ("start", {"session_id": 1}),
                   ("configure", {}), ("configure", {"source": "stdout"}),
                   ("configure", {"source": "system", "rx": -1}),
                   ("configure", {"source": "uart", "rx": 4, "tx": 5}),
                   ("stop", {}), ("stop", {"session_id": 1, "boot_id": ""}),
                   ("pause", {"session_id": 1, "boot_id": "boot-a", "expected_generation": 1, "paused": "true"}),
                   ("read", {"session_id": 1, "boot_id": "boot-a", "view_generation": 1, "limit": 8})]
        for action, fields in invalid:
            with self.subTest(action=action, fields=fields), self.assertRaises(ValueError):
                cli.call_monitor(board, action, **fields)
        for token in (0, -1, 4294967296, 1.5, "1", True, None):
            for name in ("session_id", "expected_generation"):
                fields = {"session_id": 1, "boot_id": "boot-a", "expected_generation": 1}
                fields[name] = token
                with self.subTest(name=name, token=token), self.assertRaises(ValueError):
                    cli.call_monitor(board, "clear", **fields)
        for name, values in (("rx", (-1, 49, True, "1")), ("tx", (-1, 49, True, "1")),
                             ("baud", (0, 12345, "115200", True))):
            for value in values:
                fields = {"source": "uart", "rx": 4, "tx": 5, "baud": 115200, name: value}
                with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                    cli.call_monitor(board, "configure", **fields)
        self.assertEqual(board.calls, [])

    def invoke(self, arguments, board):
        output = io.StringIO()
        with patch.object(cli, "Board", return_value=board) as factory, \
                patch.object(sys, "argv", [str(SOURCE), "--port", "FAKE", "monitor", *arguments]), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            result = cli.main()
            factory.assert_called_once_with("FAKE")
        self.assertTrue(board.closed)
        return result, output.getvalue()

    def test_real_parser_and_dispatch_all_actions_close_fake_board(self):
        tokens = ["--boot-id", "saved", "--session-id", "31"]
        cases = [(["status"], {"action": "status"}),
                 (["start"], {"action": "start", "boot_id": "boot-a", "expected_revision": 7}),
                 (["configure", "system"], {"action": "configure", "source": "system", "boot_id": "boot-a", "session_id": 31, "expected_revision": 7}),
                 (["configure", "uart", "--rx", "4", "--tx", "5", "--baud", "115200"],
                  {"action": "configure", "source": "uart", "rx": 4, "tx": 5, "baud": 115200, "boot_id": "boot-a", "session_id": 31, "expected_revision": 7}),
                 (["stop", *tokens], {"action": "stop", "boot_id": "saved", "session_id": 31}),
                 (["pause", *tokens, "--expected-generation", "9", "--paused", "false"],
                  {"action": "pause", "boot_id": "saved", "session_id": 31, "expected_generation": 9, "paused": False}),
                 (["clear", *tokens, "--expected-generation", "9"],
                  {"action": "clear", "boot_id": "saved", "session_id": 31, "expected_generation": 9}),
                 (["read", *tokens, "--view-generation", "9"],
                  {"action": "read", "boot_id": "saved", "session_id": 31, "view_generation": 9, "after_sequence": 0})]
        for arguments, expected in cases:
            with self.subTest(arguments=arguments):
                board = FakeBoard(auto_replies() if arguments[0] in ("start", "configure") else [{"ok": True}])
                self.assertEqual(self.invoke(arguments, board)[0], 0)
                self.assertEqual(board.calls[-1], ("monitor", expected))

    def test_read_leaves_binary_as_base64_and_does_not_watch(self):
        board = FakeBoard([{"ok": True, "records": [{"length": 3, "data_b64": "AAEC"}], "more": True}])
        result, output = self.invoke(["read", "--boot-id", "saved", "--session-id", "31",
                                      "--view-generation", "9", "--after-sequence", "4294967295"], board)
        self.assertEqual(result, 0)
        self.assertIn('"data_b64": "AAEC"', output)
        self.assertEqual(len(board.calls), 1)
        self.assertEqual(board.calls[0][1]["after_sequence"], 4294967295)

    def test_main_failure_or_unknown_ack_exits_once_and_closes(self):
        for response in ({"ok": False}, TimeoutError("unknown ACK")):
            board = FakeBoard(auto_replies(response))
            self.assertEqual(self.invoke(["start"], board)[0], 1)
            self.assertEqual(len(board.calls), 3)

    def test_invalid_cli_arguments_fail_before_board_construction(self):
        tokens = ["--boot-id", "saved", "--session-id", "31"]
        cases = [[], ["watch"], ["start", "--session-id", "31"],
                 ["configure"], ["configure", "system", "--rx", "4"],
                 ["configure", "uart", "--rx", "4", "--tx", "5", "--baud", "12345"],
                 ["configure", "uart", "--rx", "49", "--tx", "5", "--baud", "115200"],
                 ["stop"], ["stop", "--boot-id", "", "--session-id", "31"],
                 ["stop", "--boot-id", "saved", "--session-id", "0"],
                 ["stop", "--boot-id", "saved", "--session-id", "4294967296"],
                 ["pause", *tokens, "--expected-generation", "9", "--paused", "1"],
                 ["clear", *tokens], ["read", *tokens],
                 ["read", *tokens, "--view-generation", "9", "--after-sequence", "-1"],
                 ["read", *tokens, "--view-generation", "9", "--limit", "8"]]
        for arguments in cases:
            with self.subTest(arguments=arguments), patch.object(cli, "Board") as factory, \
                    patch.object(sys, "argv", [str(SOURCE), "--port", "FAKE", "monitor", *arguments]), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                cli.main()
            self.assertEqual(raised.exception.code, 2)
            factory.assert_not_called()


if __name__ == "__main__":
    unittest.main()
