#!/usr/bin/env python3
"""Check the real touch device/Lua API; optionally record human touch samples.

This does not inject touches or certify physical screen alignment. The existing
uploaded script files are not modified. The board is reset once.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
import time

from board_lua import Board, checked_boot_diagnostics, expect_ok, require


def observe(board, seconds):
    result = {"seconds_requested": seconds, "visual_confirmed": False,
              "pressed_samples": [], "transitions": []}
    start = board.call("info")
    expect_ok(start)
    result["before"] = start
    source = ("local p=require('touch').read(); "
              "print(p.pressed,p.event,p.x or -1,p.y or -1,p.gesture,p.sampled_ms)")
    deadline = time.monotonic() + seconds
    previous_pressed = False
    print("TOUCH_OBSERVATION_START: tap, drag and lift on the physical screen", flush=True)
    while time.monotonic() < deadline:
        response = board.call("eval", source=source)
        expect_ok(response)
        fields = response["output"].strip().split("\t")
        require(len(fields) == 6, repr(response))
        require(fields[0] in ("true", "false"), repr(response))
        sample = {"pressed": fields[0] == "true", "event": fields[1],
                  "x": int(fields[2]), "y": int(fields[3]),
                  "gesture": int(fields[4]), "sampled_ms": int(fields[5])}
        if sample["pressed"]:
            require(0 <= sample["x"] < 240 and 0 <= sample["y"] < 240, repr(sample))
            result["pressed_samples"].append(sample)
        if sample["pressed"] != previous_pressed:
            result["transitions"].append(sample)
            print("TOUCH_TRANSITION", json.dumps(sample), flush=True)
        previous_pressed = sample["pressed"]
        time.sleep(0.08)  # Leave the native idle UI time to process and draw.
    finish = board.call("info")
    expect_ok(finish)
    result["after"] = finish
    result["physical_press_observed"] = bool(result["pressed_samples"]) or finish["touch_presses"] > start["touch_presses"]
    result["physical_release_observed"] = finish["touch_releases"] > start["touch_releases"]
    require(finish["touch_errors"] == start["touch_errors"], "I2C errors during observation")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--observe-seconds", type=int, default=0)
    args = parser.parse_args()
    if not 0 <= args.observe_seconds <= 60:
        parser.error("--observe-seconds must be 0..60")
    root = Path(__file__).resolve().parents[1]
    report = {"ok": False, "checked_at": datetime.now(timezone.utc).isoformat(),
              "verification_scope": "real I2C reads, Lua assertions and demo state; not optical verification",
              "visual_confirmed": False, "tests": []}
    board = None
    try:
        board = Board(args.port)

        def case(name, check):
            result = check()
            report["tests"].append({"name": name, "ok": True, "result": result})
            print("PASS", name, flush=True)

        def evaluate(source, marker, timeout=1500):
            result = board.call("eval", source=source, timeout_ms=timeout)
            expect_ok(result, marker)
            return result

        report["board"] = board.call("info")
        expect_ok(report["board"])
        require(report["board"].get("touch_driver_ready") is True, "Touch initialization failed")
        require(report["board"].get("display_driver_ready") is True, "Display initialization failed")
        case("chip_identity_and_awake_register", lambda: evaluate(
            "local t=require('touch').info(); local names={[0xB5]='CST816T',[0xB6]='CST816D'}; "
            "assert(t.ready and names[t.chip_id] and t.controller==names[t.chip_id] and t.address==0x15); "
            "assert(t.disable_auto_sleep==1); print('IDENTITY_PASS',t.chip_id,t.project_id,t.firmware_version,t.factory_id)",
            "IDENTITY_PASS"))
        case("100_real_touch_reads", lambda: evaluate(
            (root / "tests/touch_read.lua").read_text(encoding="utf-8"), "TOUCH_READ_PASS"))

        def repeated_handoff():
            # Separate VMs reproduce the high-speed DMA handoff failure seen
            # after Lua drawing relinquishes the canvas to the native demo.
            source = (root / "scripts/display_demo.lua").read_text(encoding="utf-8")
            for _ in range(20):
                evaluate(source, "DISPLAY_DEMO_READY")
                evaluate("assert(require('touch').demo(true)); print('HANDOFF_PASS')", "HANDOFF_PASS")
            return {"handoffs": 20}
        case("20_display_touch_vm_handoffs", repeated_handoff)

        def system_menu_reclaims_demo():
            evaluate(
                "local t=require('touch'); assert(t.demo(true)); "
                "assert(t.info().demo_enabled); print('DEMO_ARMED')",
                "DEMO_ARMED")
            evaluate(
                "assert(not require('touch').info().demo_enabled); "
                "print('SYSTEM_MENU_RECLAIM_PASS')",
                "SYSTEM_MENU_RECLAIM_PASS")
            result = board.call("info")
            expect_ok(result)
            require(result["touch_demo_enabled"] is False, repr(result))
            return result
        case("system_menu_reclaims_native_demo_after_vm", system_menu_reclaims_demo)
        case("lua_drawing_takes_canvas_ownership", lambda: evaluate(
            "local t=require('touch'); local d=require('display'); "
            "local draw={function() d.clear(0) end,function() d.rect(0,0,1,1,1) end,"
            "function() d.text(0,0,'A',1) end,function() d.show() end}; "
            "for _,f in ipairs(draw) do assert(t.demo(true)); d.read_pixel(0,0); "
            "assert(t.info().demo_enabled); f(); assert(not t.info().demo_enabled) end; "
            "print('OWNERSHIP_PASS')", "OWNERSHIP_PASS"))

        def bad_demo_arg():
            result = board.call("eval", source="require('touch').demo('yes')")
            require(result.get("ok") is False and result.get("phase") == "runtime", repr(result))
            require("boolean expected" in result.get("error", ""), repr(result))
            return result
        case("demo_argument_guard", bad_demo_arg)

        def timeout():
            result = board.call("eval", source="while true do require('touch').read() end", timeout_ms=100)
            require(result.get("ok") is False and result.get("phase") == "timeout", repr(result))
            require(100 <= result["elapsed_ms"] < 600, repr(result))
            return result
        case("touch_read_loop_timeout", timeout)

        def repeated_vm():
            before = board.call("info")
            expect_ok(before)
            for _ in range(20):
                evaluate("require('touch').read(); print('VM_PASS')", "VM_PASS")
            after = board.call("info")
            expect_ok(after)
            require(before["free_psram_bytes"] - after["free_psram_bytes"] <= 1024, repr(after))
            require(before["free_internal_bytes"] - after["free_internal_bytes"] <= 4096, repr(after))
            return {"before": before, "after": after}
        case("20_touch_vm_lifecycles", repeated_vm)

        def reboot():
            start = len(board.events)
            product_boot = board.reset()
            expect_ok(product_boot)
            diagnostics = checked_boot_diagnostics(board.events[start:])
            if diagnostics:
                touch = diagnostics.get("boot-touch", {})
                display = diagnostics.get("boot-display", {})
                expect_ok(touch, "TOUCH_DEMO_READY")
                expect_ok(display, "DISPLAY_DEMO_READY")
                return {"mode": "boot_diagnostics", "touch": touch,
                        "display": display}
            require(product_boot.get("display_driver_ready") is True and
                    product_boot.get("touch_driver_ready") is True,
                    repr(product_boot))
            require(product_boot.get("touch_demo_enabled") is False,
                    repr(product_boot))
            return {"mode": "product_boot_without_diagnostics",
                    "touch": None, "display": None,
                    "response": product_boot}
        case("touch_display_and_runtime_ready_after_reset", reboot)

        def idle():
            before = board.call("info")
            expect_ok(before)
            time.sleep(10)
            after = board.call("info")
            expect_ok(after)
            require(after["touch_reads"] > before["touch_reads"] + 30, repr(after))
            require(after["touch_errors"] == before["touch_errors"] == 0, repr(after))
            return {"before": before, "after": after}
        case("10_second_idle_polling_without_sleep_nacks", idle)

        # Exercise the legacy touch demo. The product system shell must reclaim
        # the display and touch owner when this Lua job returns.
        report["demo"] = evaluate((root / "scripts/touch_demo.lua").read_text(encoding="utf-8"), "TOUCH_DEMO_READY")
        print("TOUCH_SOFTWARE_TESTS_PASS", len(report["tests"]), flush=True)
        if args.observe_seconds:
            report["observation"] = observe(board, args.observe_seconds)
            print("TOUCH_OBSERVATION_DONE", report["observation"]["physical_press_observed"], flush=True)
        report["final_board"] = board.call("info")
        expect_ok(report["final_board"])
        require(report["final_board"]["touch_demo_enabled"] is False,
                repr(report["final_board"]))
        report["ok"] = True
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
