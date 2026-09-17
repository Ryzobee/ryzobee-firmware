#!/usr/bin/env python3
"""Reset a real Ryzobee and assert the product-menu boot path."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--expect-version", default="0.8.0")
    parser.add_argument("--seconds", type=float, default=6.0)
    parser.add_argument(
        "--require-portal",
        action="store_true",
        help="also require the unprovisioned SoftAP portal-ready log",
    )
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if not 2.0 <= args.seconds <= 30.0:
        parser.error("--seconds must be 2..30")

    report = {
        "ok": False,
        "checked_at": datetime.now(timezone.utc).isoformat(),
        "port": args.port,
        "expected_version": args.expect_version,
        "require_portal": args.require_portal,
    }
    board = serial.Serial(port=None, baudrate=115200, timeout=0.2,
                          write_timeout=5, exclusive=True)
    board.dtr = True
    board.rts = False
    board.port = args.port
    lines = []
    try:
        board.open()
        board.dtr = False
        board.reset_input_buffer()
        board.rts = True
        time.sleep(0.1)
        board.rts = False
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            line = board.readline().decode("utf-8", errors="replace").strip()
            if line:
                lines.append(line)

        text = "\n".join(lines)
        checks = {
            "version": f"App version:      {args.expect_version}" in text,
            "rpc_ready": "RYZOBEE_READY" in text,
            "display_ready": '"display_driver_ready":true' in text,
            "font_ready": ('"ui_fonts_ready":true' in text or
                           ('"ui_fonts_ready":' not in text and '"font_engine_ready":true' in text)),
            "touch_ready": '"touch_driver_ready":true' in text,
            "time_service_ready": "time service: initialized" in text,
            "ota_service_ready": "OTA service: initialized" in text,
            "boot_health_gate": "running image health gate: passed" in text,
            "portal_ready": bool(re.search(
                r"provisioning portal ready on SSID RYZOBEE-[0-9A-F]{4}", text)),
            "render_error_absent": "system UI render failed" not in text,
            "fatal_error_absent": not any(marker in text for marker in (
                "Guru Meditation Error", "abort() was called", "assert failed")),
        }
        report.update({"checks": checks, "log": lines})
        required_checks = (
            "version",
            "rpc_ready",
            "display_ready",
            "font_ready",
            "touch_ready",
            "time_service_ready",
            "ota_service_ready",
            "boot_health_gate",
            "render_error_absent",
            "fatal_error_absent",
        )
        failures = [name for name in required_checks if not checks[name]]
        if args.require_portal and not checks["portal_ready"]:
            failures.append("portal_ready")
        if failures:
            raise AssertionError("failed checks: " + ", ".join(failures))
        report["ok"] = True
        print("SYSTEM_UI_BOOT_SMOKE_PASS", args.expect_version)
        return 0
    except (AssertionError, OSError, serial.SerialException) as error:
        report["error"] = str(error)
        print("SYSTEM_UI_BOOT_SMOKE_FAIL", error, file=sys.stderr)
        return 1
    finally:
        board.close()
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(
                json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
