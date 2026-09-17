#!/usr/bin/env python3
"""Run the agreed Lua display tests; software PASS is not optical verification."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import sys

from board_lua import Board, checked_boot_diagnostics, expect_ok, require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    report = {"ok": False, "checked_at": datetime.now(timezone.utc).isoformat(),
              "visual_confirmed": False,
              "verification_scope": "Lua assertions on RAM canvas and completed SPI transfers, not LCD readback",
              "tests": []}
    board = None
    try:
        board = Board(args.port)
        report["board"] = board.call("info")
        expect_ok(report["board"])
        require(report["board"].get("display_driver_ready") is True, "Display driver initialization failed")
        for name in ("display_clear", "display_rect", "display_text"):
            source = (root / "tests" / (name + ".lua")).read_text(encoding="utf-8")
            response = board.call("eval", source=source, timeout_ms=1000)
            report["tests"].append({"name": name, "response": response})
            expect_ok(response, name.upper() + "_PASS")
            print("PASS", name, flush=True)

        start = len(board.events)
        product_boot = board.reset()
        expect_ok(product_boot)
        diagnostics = checked_boot_diagnostics(board.events[start:])
        if diagnostics:
            display_boot = diagnostics.get("boot-display", {})
            report["tests"].append({"name": "display_after_reset", "response": display_boot,
                                    "mode": "boot_diagnostics"})
            expect_ok(display_boot, "DISPLAY_DEMO_READY")
            print("PASS display_after_reset (diagnostic boot)", flush=True)
        else:
            require(product_boot.get("display_driver_ready") is True,
                    repr(product_boot))
            require(product_boot.get("touch_demo_enabled") is False,
                    repr(product_boot))
            report["tests"].append({"name": "display_after_reset",
                                    "mode": "product_boot_without_diagnostics",
                                    "response": product_boot})
            print("PASS display_after_reset (product boot; menu needs visual confirmation)",
                  flush=True)

        # Exercise the legacy demo, then let the product system shell reclaim
        # the display when the Lua job returns.
        demo = (root / "scripts" / "display_demo.lua").read_text(encoding="utf-8")
        report["demo"] = board.call("eval", source=demo, timeout_ms=1000)
        expect_ok(report["demo"], "DISPLAY_DEMO_READY")
        report["ok"] = True
        print("DISPLAY_SOFTWARE_TESTS_PASS", len(report["tests"]), flush=True)
        print("Product shell returned from each Lua test; visually confirm the HOME page.")
        return 0
    except (AssertionError, OSError, TimeoutError, ValueError) as error:
        report["error"] = str(error)
        print("FAIL", error, file=sys.stderr)
        return 1
    finally:
        if board:
            report["responses"] = board.events
            board.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
