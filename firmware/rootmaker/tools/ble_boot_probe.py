#!/usr/bin/env python3
"""Bounded, read-only BLE startup probe; --reset explicitly pulses RTS once.

No radio commands, Lua execution, NVS writes, addresses or pairing codes.
An init result is meaningful only when its probe says returned=true.
"""
import argparse
import json
import re
import time

import serial

REQUEST_ID = "ble-boot-probe"
INFO_KEYS = ("ok", "firmware", "app_core", "idf", "uptime_ms",
             "free_internal_bytes", "free_psram_bytes", "runtime_gateway_ready",
             "filesystem_ready", "display_driver_ready", "ui_fonts_ready",
             "system_services_running", "system_services_cycles", "system_network_valid",
             "system_provisioning_started", "time_service_ready", "ota_service_ready",
             "ui_owner_stack_bytes", "ui_owner_stack_free_min_bytes",
             "touch_errors", "ui_input_dropped", "ble")
BLE_KEYS = ("snapshot_error", "available", "phase", "failure", "last_error",
            "sdk_error", "enabled", "saved_enabled", "bond_known", "bonded",
            "linked", "authenticated", "boot_settled", "boot_error", "checking_state",
            "hid_ready", "hid_busy", "hid_sdk_error")
PROBE_KEYS = ("entered", "returned", "result", "internal_free_before",
              "internal_largest_before", "internal_free_after", "internal_largest_after")
MAX_REPLY_AGE_S = 3.0


def safe_info(reply):
    out = {key: reply[key] for key in INFO_KEYS if key != "ble" and key in reply}
    source = reply.get("ble")
    if isinstance(source, dict):
        out["ble"] = {key: source[key] for key in BLE_KEYS if key in source}
        init = source.get("init")
        if isinstance(init, dict):
            safe_init = {"finished": init.get("finished")}
            for name in ("port", "buffers", "vhci"):
                probe = init.get(name)
                if isinstance(probe, dict):
                    safe_init[name] = {key: probe[key] for key in PROBE_KEYS if key in probe}
            out["ble"]["init"] = safe_init
    return out


def available(reply):
    ble = reply.get("ble", {})
    phase = ble.get("phase")
    return (reply.get("ok") is True and ble.get("snapshot_error") == 0
            and ble.get("available") is True and ble.get("boot_settled") is True
            and type(phase) is int and 0 <= phase < 13
            and ble.get("checking_state") is False)


class ProbeWindow:
    """Evidence from one bounded observation window, not a cached health claim."""
    def __init__(self):
        self.latest = {}
        self.last_reply_at = None
        self.fatal_seen = False

    def observe_reply(self, reply, now):
        if isinstance(reply, dict) and reply.get("id") == REQUEST_ID:
            self.latest = safe_info(reply)
            self.last_reply_at = now

    def reply_is_fresh(self, now):
        return (self.last_reply_at is not None
                and 0 <= now - self.last_reply_at <= MAX_REPLY_AGE_S)

    def observe_log(self, line):
        if re.search(r"assert failed|Guru Meditation|Backtrace", line, re.IGNORECASE):
            self.fatal_seen = True
        if re.search(r"ESP-ROM|rst:0x", line, re.IGNORECASE):
            # The requested initial reset has no snapshot to invalidate.
            # Any later boot must supply its own fresh matching response.
            self.latest = {}
            self.last_reply_at = None

    def passed(self, now):
        return not self.fatal_seen and self.reply_is_fresh(now) and available(self.latest)


def log_signal(line):
    line = re.sub(r"\x1b\[[0-9;]*m", "", line)
    if not re.search(r"BLE (?:service start|service snapshot|boot restore).*failed|"
                     r"BLE_INIT:|ryz_ble: startup|assert failed|Guru Meditation|Backtrace|"
                     r"ESP-ROM|rst:0x|running image health gate", line):
        return None
    line = re.sub(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", "<REDACTED_ADDR>", line)
    return re.sub(r"\b[0-9a-fA-F]{32,}\b", "<REDACTED_HEX>", line)[:500]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--expect-available", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.timeout <= 45:
        parser.error("timeout must be between 1 and 45 seconds")
    link = serial.Serial(port=None, baudrate=115200, timeout=.15, write_timeout=2, exclusive=True)
    link.dtr = True
    link.rts = False
    link.port = args.port
    window = ProbeWindow()
    count = 0
    try:
        link.open()
        link.dtr = False
        link.reset_input_buffer()
        if args.reset:
            link.rts = True
            time.sleep(.1)
            link.rts = False
        print(json.dumps({"port": args.port, "reset_requested": args.reset}), flush=True)
        deadline = time.monotonic() + args.timeout
        next_query = time.monotonic() + .3
        request = (json.dumps({"id": REQUEST_ID, "op": "info"}) + "\n").encode()
        while time.monotonic() < deadline:
            if time.monotonic() >= next_query:
                link.write(request)
                link.flush()
                next_query = time.monotonic() + 1
            line = link.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            count += 1
            if "RYZOBEE_RPC " in line:
                try:
                    reply = json.loads(line.split("RYZOBEE_RPC ", 1)[1])
                except ValueError:
                    continue
                window.observe_reply(reply, time.monotonic())
            else:
                window.observe_log(line)
                signal = log_signal(line)
                if signal:
                    print(signal, flush=True)
        completed_at = time.monotonic()
        passed = window.passed(completed_at)
        print(json.dumps({"lines_received": count, "ble_available": passed,
                          "reply_fresh": window.reply_is_fresh(completed_at),
                          "fatal_seen": window.fatal_seen,
                          "info": window.latest}, ensure_ascii=False), flush=True)
        return 0 if window.latest and (passed or not args.expect_available) else 2
    finally:
        link.rts = False
        link.dtr = False
        link.close()


if __name__ == "__main__":
    raise SystemExit(main())
