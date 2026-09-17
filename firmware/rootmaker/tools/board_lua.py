#!/usr/bin/env python3
"""Load and test Lua on the Ryzobee bring-up firmware (no firmware flashing)."""
import argparse
import json
from pathlib import Path
import sys
import time
import uuid

import serial

PREFIX = "RYZOBEE_RPC "


class Board:
    def __init__(self, port):
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.2,
                                    write_timeout=5, exclusive=True)
        # pyserial applies DTR before RTS on open. Keep DTR asserted until RTS
        # has been released, avoiding the EN-low intermediate auto-reset state.
        self.serial.dtr = True
        self.serial.rts = False
        self.serial.port = port
        self.events = []
        try:
            self.serial.open()
            self.serial.dtr = False
            self.synchronize()
        except Exception:
            self.serial.close()
            raise

    def close(self):
        self.serial.close()

    def read_response(self):
        line = self.serial.readline().decode("utf-8", errors="replace").strip()
        if PREFIX not in line:
            return None
        try:
            result = json.loads(line.split(PREFIX, 1)[1])
        except json.JSONDecodeError:
            return None
        self.events.append(result)
        return result

    def synchronize(self):
        """Some USB drivers reset the board on open; retry only a read-only probe."""
        request_id = "sync-" + uuid.uuid4().hex[:12]
        probe = (json.dumps({"id": request_id, "op": "info"}) + "\n").encode()
        deadline = time.monotonic() + 5
        next_probe = 0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_probe:
                self.serial.write(probe)
                self.serial.flush()
                next_probe = time.monotonic() + 0.3
            response = self.read_response()
            if response and response.get("id") == request_id and response.get("ok"):
                return
        raise TimeoutError("Ryzobee did not become ready after opening the serial port")

    def call(self, op, **kwargs):
        request_id = uuid.uuid4().hex[:12]
        payload = {"id": request_id, "op": op, **kwargs}
        data = (json.dumps(payload, ensure_ascii=True, separators=(",", ":")) + "\n").encode()
        if len(data) >= 24 * 1024:
            raise ValueError("encoded serial request exceeds 24 KiB")
        self.serial.write(data)
        self.serial.flush()
        deadline = time.monotonic() + 8 + kwargs.get("timeout_ms", 1000) / 1000
        while time.monotonic() < deadline:
            result = self.read_response()
            if result and result.get("id") == request_id:
                return result
        raise TimeoutError("No matching Ryzobee response; check firmware, port and serial monitor ownership")

    def reset(self):
        """Reset once and wait for live product runtime readiness, not a Lua marker.

        The old boot identity is best effort so recovery from unresponsive or
        older firmware still pulses RTS. Only read-only probes are retried.
        A matching new request ID rules out cached boot announcements; when
        the old identity is known, its boot must also change.
        """
        previous_boot = None
        before_id = "pre-reset-" + uuid.uuid4().hex[:12]
        self.serial.write((json.dumps({"id": before_id, "op": "info"}) + "\n").encode())
        self.serial.flush()
        deadline = time.monotonic() + 0.6
        while time.monotonic() < deadline:
            response = self.read_response()
            if response and response.get("id") == before_id:
                boot = response.get("boot_id")
                if response.get("ok") is True and isinstance(boot, str) and boot:
                    previous_boot = boot
                break
        self.serial.reset_input_buffer()
        self.serial.dtr = False
        self.serial.rts = True
        time.sleep(0.1)
        self.serial.rts = False
        request_id = "reset-ready-" + uuid.uuid4().hex[:12]
        probe = (json.dumps({"id": request_id, "op": "info"}) + "\n").encode()
        deadline = time.monotonic() + 10
        next_probe = 0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_probe:
                self.serial.write(probe)
                self.serial.flush()
                next_probe = time.monotonic() + 0.3
            response = self.read_response()
            if response and response.get("id") == request_id and response.get("ok") is True:
                boot = response.get("boot_id")
                if (isinstance(boot, str) and boot and boot != previous_boot and
                        response.get("runtime_gateway_ready") is True):
                    return response
        raise TimeoutError("No live ready runtime with a new boot identity after RTS reset; "
                           "firmware may be unavailable, unready or unsupported")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def expect_ok(response, text=None):
    require(response.get("ok") is True, repr(response))
    if text is not None:
        require(text in response.get("output", ""), repr(response))


def checked_boot_diagnostics(events):
    """None means no diagnostic entry was observed, never a self-test PASS."""
    diagnostics = {event["id"]: event for event in events
                   if event.get("id") in ("boot-script", "boot-display", "boot-touch")}
    if not diagnostics:
        return None
    # Any of these explicit diagnostic entries requires the C-owned system
    # self-check result; a partial/lost diagnostic report is not success.
    expect_ok(diagnostics.get("boot-script", {}), "BOOT_LUA_PASS")
    return diagnostics


def run_tests(board):
    cases = []

    def case(name, check):
        started = time.monotonic()
        check()
        cases.append({"name": name, "ok": True,
                      "elapsed_ms": round((time.monotonic() - started) * 1000)})
        print("PASS", name, flush=True)

    info = board.call("info")
    expect_ok(info)
    require(info["chip"] == "ESP32-S3" and info["flash_bytes"] == 16 * 1024 * 1024
            and info["psram_bytes"] == 2 * 1024 * 1024 and info["filesystem_ready"], repr(info))
    print(json.dumps(info, ensure_ascii=False), flush=True)
    case("board_identity", lambda: expect_ok(info))
    hello = (Path(__file__).resolve().parents[1] / "scripts/hello.lua").read_text(encoding="utf-8")
    case("source_and_native_module", lambda: expect_ok(board.call("eval", source=hello), "LUA_SMOKE_PASS"))

    def expected_failure(source, phase, text, timeout=1000):
        response = board.call("eval", source=source, timeout_ms=timeout)
        require(response.get("ok") is False, repr(response))
        require(response.get("phase") == phase, repr(response))
        require(text in response.get("error", ""), repr(response))
        if phase == "timeout":
            require(timeout <= response["elapsed_ms"] < timeout + 500, repr(response))

    case("syntax_error", lambda: expected_failure("local =", "syntax", "expected"))
    case("runtime_error", lambda: expected_failure("error('EXPECTED_FAILURE')", "runtime", "EXPECTED_FAILURE"))
    case("infinite_loop_timeout", lambda: expected_failure("while true do end", "timeout", "timed out", 100))
    case("native_sleep_timeout", lambda: expected_failure("require('ryzobee').sleep_ms(5000)", "timeout", "timed out", 100))
    case("heap_quota", lambda: expected_failure(
        "local t = {}; while true do t[#t+1] = string.rep('x', 8192) end", "memory", "memory"))
    case("module_allowlist", lambda: expected_failure("require('gpio')", "runtime", "not allowed"))
    case("restricted_libraries", lambda: expect_ok(board.call("eval", source=
        "assert(io == nil and os == nil and debug == nil and package == nil and coroutine == nil); "
        "assert(load == nil and dofile == nil and pcall == nil); print('RESTRICTED_PASS')"), "RESTRICTED_PASS"))

    def output_limit():
        response = board.call("eval", source="print(string.rep('x', 6000))")
        expect_ok(response)
        require(response.get("output_truncated") is True and len(response["output"]) == 4095, repr(response))
    case("output_quota", output_limit)

    def path_safety():
        # boot.lua is now an ordinary user file: never overwrite it while
        # probing invalid paths. Keep every probe syntactically invalid.
        for name in ("../boot.lua", "/evil.lua", "bad..lua"):
            require(board.call("put", name=name, source="print(1)")["ok"] is False, name)
    case("upload_path_guard", path_safety)

    def dynamic_upload():
        for version in (1, 2):
            source = "print('DYNAMIC_V%d'); assert(6 * 7 == 42)" % version
            expect_ok(board.call("put", name="smoke_dynamic.lua", source=source))
            expect_ok(board.call("run", name="smoke_dynamic.lua"), "DYNAMIC_V%d" % version)
    case("dynamic_replace_without_reflash", dynamic_upload)

    def repeated_vm():
        before = board.call("info")
        for _ in range(20):
            expect_ok(board.call("eval", source="local t={1,2,3}; assert(#t==3); print('ISOLATED_PASS')"), "ISOLATED_PASS")
        after = board.call("info")
        require(before["free_psram_bytes"] - after["free_psram_bytes"] <= 1024, repr(after))
        require(before["free_internal_bytes"] - after["free_internal_bytes"] <= 4096, repr(after))
    case("repeat_20_vm_lifecycles", repeated_vm)

    def reboot_persistence():
        start = len(board.events)
        expect_ok(board.reset())
        checked_boot_diagnostics(board.events[start:])
        expect_ok(board.call("run", name="smoke_dynamic.lua"), "DYNAMIC_V2")
    case("reset_and_file_persistence", reboot_persistence)
    case("healthy_after_all_failures", lambda: expect_ok(board.call("eval", source=hello), "LUA_SMOKE_PASS"))
    return {"ok": True, "tests": cases, "board": info, "responses": board.events}


def call_i2c_scan(board, action, scan_id=None, boot_id=None):
    """Explicit scan control, no raw-I2C fallback or retry after an unknown ACK."""
    if action == "status":
        return board.call("i2c_scan", action="status")
    if action == "start":
        info = board.call("info")
        expect_ok(info)
        current_boot = info.get("boot_id")
        if not isinstance(current_boot, str) or not current_boot:
            raise ValueError("firmware did not provide a boot_id; scan not started")
        return board.call("i2c_scan", action="start", boot_id=current_boot)
    if action == "cancel":
        if (type(scan_id) is not int or not 1 <= scan_id <= 0xFFFFFFFF or
                not isinstance(boot_id, str) or not boot_id):
            raise ValueError("cancel requires the scan_id and boot_id from its start/status reply")
        # Never replace a saved boot_id with the current boot: that could cancel
        # an unrelated scan whose numeric ID was reused after a board reboot.
        return board.call("i2c_scan", action="cancel", scan_id=scan_id,
                          boot_id=boot_id)
    raise ValueError("unknown I2C scan action")


def scan_id_argument(value):
    try:
        number = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("scan ID must be an integer") from error
    if not 1 <= number <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("scan ID must be 1..4294967295")
    return number


def call_i2c_configure(board, sda, scl, hz):
    """Software-only CAS configuration; no fallback/scan/retry on an unknown ACK."""
    if (type(sda) is not int or type(scl) is not int or not 0 <= sda <= 48 or
            not 0 <= scl <= 48 or type(hz) is not int or hz not in (100000, 400000)):
        raise ValueError("I2C configuration requires integer pins 0..48 and 100000/400000 Hz")
    info = board.call("info")
    expect_ok(info)
    boot = info.get("boot_id")
    if not isinstance(boot, str) or not boot:
        raise ValueError("firmware did not provide a boot_id; I2C was not configured")
    status = board.call("i2c_scan", action="status", boot_id=boot)
    expect_ok(status)
    revision = status.get("config_revision")
    if (status.get("schema") != "ryz-i2c-scan/2" or status.get("boot_id") != boot or
            type(revision) is not int or not 1 <= revision <= 0xFFFFFFFF):
        raise ValueError("I2C v2 status boot/revision does not match; configuration was not submitted")
    return board.call("i2c_scan", action="configure", boot_id=boot,
                      sda=sda, scl=scl, hz=hz, expected_revision=revision)


def i2c_pin_argument(value):
    try:
        pin = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("I2C pin must be an integer") from error
    if not 0 <= pin <= 48:
        raise argparse.ArgumentTypeError("I2C pin must be 0..48")
    return pin


def call_rgb(board, action, red=None, green=None, blue=None, *, request_id=None, boot_id=None):
    """Raw RGB8 control; admission is not optical feedback. Never retry a send."""
    channels = (red, green, blue)
    if action == "cleanup":
        if (any(channel is not None for channel in channels) or
                type(request_id) is not int or not 1 <= request_id <= 0xFFFFFFFF or
                not isinstance(boot_id, str) or not boot_id):
            raise ValueError("RGB cleanup requires its original boot_id/request_id and no color")
        # Never discover a fresh token: that could release another controller's
        # resources. Cleanup is not off; it does not transmit a black frame.
        return board.call("rgb", action="cleanup", boot_id=boot_id, request_id=request_id)
    if request_id is not None or boot_id is not None:
        raise ValueError("RGB boot/request tokens are explicit only for cleanup")
    if action == "set":
        if any(type(channel) is not int or not 0 <= channel <= 255 for channel in channels):
            raise ValueError("RGB channels must be integers from 0 to 255")
    elif action not in ("off", "status") or any(channel is not None for channel in channels):
        raise ValueError("unknown RGB action or unrelated color arguments")
    if action == "status":
        return board.call("rgb", action="status")
    info = board.call("info")
    expect_ok(info)
    boot = info.get("boot_id")
    if not isinstance(boot, str) or not boot:
        raise ValueError("firmware did not provide a boot_id; RGB was not submitted")
    fields = {"action": action, "boot_id": boot}
    if action == "set":
        fields.update(red=red, green=green, blue=blue)
    # off is an explicit black-frame request in firmware, not a pin-disable.
    # An exception/unknown ACK propagates: query status before another send.
    return board.call("rgb", **fields)


def call_network(board, action, *, confirm=False):
    """Explicit network intent; acceptance is not completion. Never retry writes.

    On/off/setup preserve saved credentials. Setup opens the local AP portal;
    it is not an ON alias. Reprovision is the separate, confirmed
    forget-and-reopen operation; a lost ACK must be investigated via status.
    """
    if (action not in ("status", "on", "off", "setup", "reprovision") or
            type(confirm) is not bool or confirm != (action == "reprovision")):
        raise ValueError("network reprovision requires --confirm; other actions take no confirmation")
    if action == "status":
        return board.call("network", action="status")
    info = board.call("info")
    expect_ok(info)
    boot = info.get("boot_id")
    if not isinstance(boot, str) or not boot:
        raise ValueError("firmware did not provide a boot_id; network intent was not submitted")
    fields = {"action": action, "boot_id": boot}
    if action == "reprovision":
        fields["confirm"] = True
    return board.call("network", **fields)


def rgb_channel_argument(value):
    try:
        number = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("RGB channel must be an integer") from error
    if not 0 <= number <= 255:
        raise argparse.ArgumentTypeError("RGB channel must be 0..255")
    return number


def rgb_request_argument(value):
    try:
        number = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("RGB request ID must be an integer") from error
    if not 1 <= number <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("RGB request ID must be 1..4294967295")
    return number


def rgb_boot_argument(value):
    if not value:
        raise argparse.ArgumentTypeError("RGB boot ID must not be empty")
    return value


MONITOR_BAUDS = (1200, 2400, 4800, 9600, 19200, 38400, 57600,
                 115200, 230400, 460800, 921600)


def call_monitor(board, action, **fields):
    """Bounded Monitor commands. No implicit takeover, retry, or UART fallback.

    SYSTEM means filtered ESP diagnostics, not all stdout. Pause freezes the
    view while capture continues; read returns at most eight binary-safe records.
    """
    allowed = {"status": set(), "start": set(),
               "configure": {"source", "rx", "tx", "baud"},
               "stop": {"boot_id", "session_id"},
               "pause": {"boot_id", "session_id", "expected_generation", "paused"},
               "clear": {"boot_id", "session_id", "expected_generation"},
               "read": {"boot_id", "session_id", "view_generation", "after_sequence"}}
    if action not in allowed or not fields.keys() <= allowed[action]:
        raise ValueError("unknown Monitor action or unrelated fields")
    if action == "configure":
        if fields.get("source") == "system":
            if set(fields) != {"source"}:
                raise ValueError("system Monitor is filtered diagnostics and takes no UART pins/baud")
        elif fields.get("source") == "uart":
            if (set(fields) != allowed[action] or
                    any(type(fields.get(pin)) is not int or not 0 <= fields[pin] <= 48
                        for pin in ("rx", "tx")) or
                    type(fields.get("baud")) is not int or fields["baud"] not in MONITOR_BAUDS):
                raise ValueError("UART Monitor requires integer RX/TX pins 0..48 and a supported baud")
        else:
            raise ValueError("Monitor source must be system or uart")
    if action not in ("status", "start", "configure"):
        if action == "read":
            fields.setdefault("after_sequence", 0)
        if set(fields) != allowed[action]:
            raise ValueError("Monitor action requires its original boot/session/view tokens")
        if not isinstance(fields["boot_id"], str) or not fields["boot_id"]:
            raise ValueError("Monitor boot ID must not be empty")
        for name in ("session_id", "expected_generation", "view_generation", "after_sequence"):
            if name in fields and (type(fields[name]) is not int or
                    not (0 if name == "after_sequence" else 1) <= fields[name] <= 0xFFFFFFFF):
                raise ValueError("Monitor token/cursor must be a bounded uint32 integer")
        if action == "pause" and type(fields["paused"]) is not bool:
            raise ValueError("Monitor paused must be a boolean")
    if action in ("start", "configure"):
        info = board.call("info")
        expect_ok(info)
        boot = info.get("boot_id")
        if not isinstance(boot, str) or not boot:
            raise ValueError("firmware did not provide a boot_id; Monitor was not submitted")
        status = board.call("monitor", action="status", boot_id=boot)
        expect_ok(status)
        revision, session = status.get("config_revision"), status.get("session_id")
        if (status.get("schema") != "ryz-monitor/1" or status.get("boot_id") != boot or
                type(revision) is not int or not 1 <= revision <= 0xFFFFFFFF or
                type(session) is not int or not 0 <= session <= 0xFFFFFFFF):
            raise ValueError("Monitor status boot/schema/revision/session mismatch; no request submitted")
        fields.update(boot_id=boot, expected_revision=revision)
        if action == "configure":
            fields["session_id"] = session
    # Unknown ACK propagates. Explicit history actions never replace the saved
    # boot/session/view with a newer caller's tokens by fetching fresh status.
    return board.call("monitor", action=action, **fields)


def monitor_cursor_argument(value):
    try:
        number = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("Monitor token must be an integer") from error
    if not 0 <= number <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("Monitor token must be 0..4294967295")
    return number


def monitor_token_argument(value):
    number = monitor_cursor_argument(value)
    if not number:
        raise argparse.ArgumentTypeError("Monitor session/view token must be positive")
    return number


def monitor_pin_argument(value):
    number = monitor_cursor_argument(value)
    if number > 48:
        raise argparse.ArgumentTypeError("Monitor UART pin must be 0..48")
    return number


def monitor_boot_argument(value):
    if not value:
        raise argparse.ArgumentTypeError("Monitor boot ID must not be empty")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("info")
    sub.add_parser("reset")
    ota = sub.add_parser("ota")
    ota.add_argument("action", choices=("status", "start", "cancel"))
    network = sub.add_parser("network", help="Explicit Wi-Fi intent; on/off/setup preserve saved credentials")
    network_actions = network.add_subparsers(dest="action", required=True)
    for action in ("status", "on", "off"):
        network_actions.add_parser(action)
    network_actions.add_parser("setup", help="Open the local AP portal without forgetting saved Wi-Fi")
    reprovision = network_actions.add_parser("reprovision", help="Forget saved Wi-Fi and reopen provisioning")
    reprovision.add_argument("--confirm", action="store_true", required=True,
                             help="Confirm the separate credential deletion operation")
    scan = sub.add_parser("i2c-scan", help="Explicit configured-bus scan; status/configure never starts probing")
    scan_actions = scan.add_subparsers(dest="action", required=True)
    scan_actions.add_parser("start")
    scan_actions.add_parser("status")
    cancel_scan = scan_actions.add_parser("cancel")
    cancel_scan.add_argument("--scan-id", type=scan_id_argument, required=True)
    cancel_scan.add_argument("--boot-id", required=True)
    configure_scan = scan_actions.add_parser("configure")
    configure_scan.add_argument("--sda", type=i2c_pin_argument, required=True)
    configure_scan.add_argument("--scl", type=i2c_pin_argument, required=True)
    configure_scan.add_argument("--hz", type=int, choices=(100000, 400000), required=True)
    rgb = sub.add_parser("rgb", help="Explicit board RGB8 control; status never starts hardware")
    rgb_actions = rgb.add_subparsers(dest="action", required=True)
    set_rgb = rgb_actions.add_parser("set")
    for channel in ("red", "green", "blue"):
        set_rgb.add_argument(channel, type=rgb_channel_argument)
    rgb_actions.add_parser("off")
    rgb_actions.add_parser("status")
    cleanup_rgb = rgb_actions.add_parser("cleanup", help="Release original request resources without sending black")
    cleanup_rgb.add_argument("--boot-id", type=rgb_boot_argument, required=True)
    cleanup_rgb.add_argument("--request-id", type=rgb_request_argument, required=True)
    monitor = sub.add_parser("monitor", help="Bounded filtered diagnostics or receive-only UART1; never UART0")
    monitor_actions = monitor.add_subparsers(dest="action", required=True)
    monitor_actions.add_parser("status")
    monitor_actions.add_parser("start", help="Accept asynchronously; inspect status for actual reception")
    configure_monitor = monitor_actions.add_parser("configure")
    monitor_sources = configure_monitor.add_subparsers(dest="source", required=True)
    monitor_sources.add_parser("system", help="Filtered ESP diagnostics, not all stdout")
    uart_monitor = monitor_sources.add_parser("uart")
    uart_monitor.add_argument("--rx", type=monitor_pin_argument, required=True)
    uart_monitor.add_argument("--tx", type=monitor_pin_argument, required=True,
                              help="Reserved but not driven (receive-only)")
    uart_monitor.add_argument("--baud", type=int, choices=MONITOR_BAUDS, required=True)
    for action in ("stop", "pause", "clear", "read"):
        monitor_action = monitor_actions.add_parser(action)
        monitor_action.add_argument("--boot-id", type=monitor_boot_argument, required=True)
        monitor_action.add_argument("--session-id", type=monitor_token_argument, required=True)
        if action in ("pause", "clear"):
            monitor_action.add_argument("--expected-generation", type=monitor_token_argument, required=True)
        if action == "pause":
            monitor_action.add_argument("--paused", choices=("true", "false"), required=True,
                                        help="Freeze view while bounded capture continues")
        if action == "read":
            monitor_action.add_argument("--view-generation", type=monitor_token_argument, required=True)
            monitor_action.add_argument("--after-sequence", type=monitor_cursor_argument, default=0)
    for name in ("exec", "put", "run"):
        command = sub.add_parser(name)
        command.add_argument("script")
        if name == "put":
            command.add_argument("--name", default=None)
        else:
            command.add_argument("--timeout-ms", type=int, default=1000)
    tests = sub.add_parser("test")
    tests.add_argument("--report", type=Path)
    args = parser.parse_args()
    board = Board(args.port)
    report = None
    try:
        if args.command == "test":
            report = run_tests(board)
            print("LUA_BOARD_TESTS_PASS", len(report["tests"]))
        elif args.command == "info":
            report = board.call("info")
        elif args.command == "reset":
            report = board.reset()
        elif args.command == "ota":
            report = board.call("ota", action=args.action)
        elif args.command == "network":
            report = call_network(board, args.action, confirm=getattr(args, "confirm", False))
        elif args.command == "i2c-scan":
            if args.action == "configure":
                report = call_i2c_configure(board, args.sda, args.scl, args.hz)
            else:
                report = call_i2c_scan(board, args.action,
                                       getattr(args, "scan_id", None),
                                       getattr(args, "boot_id", None))
        elif args.command == "rgb":
            report = call_rgb(board, args.action, getattr(args, "red", None),
                              getattr(args, "green", None), getattr(args, "blue", None),
                              request_id=getattr(args, "request_id", None),
                              boot_id=getattr(args, "boot_id", None))
        elif args.command == "monitor":
            fields = {name: getattr(args, name) for name in (
                "source", "rx", "tx", "baud", "boot_id", "session_id",
                "expected_generation", "view_generation", "after_sequence") if hasattr(args, name)}
            if args.action == "pause":
                fields["paused"] = args.paused == "true"
            report = call_monitor(board, args.action, **fields)
        elif args.command == "run":
            report = board.call("run", name=args.script, timeout_ms=args.timeout_ms)
        else:
            source = Path(args.script).read_text(encoding="utf-8")
            if args.command == "put":
                report = board.call("put", name=args.name or Path(args.script).name, source=source)
            else:
                report = board.call("eval", source=source, timeout_ms=args.timeout_ms)
        if args.command != "test":
            print(json.dumps(report, ensure_ascii=False, indent=2))
        require(report.get("ok") is True, "Board command failed")
        return 0
    except (AssertionError, TimeoutError, ValueError, OSError) as error:
        report = {"ok": False, "error": str(error), "responses": board.events}
        print(json.dumps(report, ensure_ascii=False), file=sys.stderr)
        return 1
    finally:
        board.close()
        if args.command == "test" and args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
