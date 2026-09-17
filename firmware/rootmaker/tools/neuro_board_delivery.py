#!/usr/bin/env python3
"""Scoped neural delivery checks. Backup is read-only; run writes a fresh test file only."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import time
import uuid
from urllib.request import Request, urlopen

from board_lua import Board, expect_ok, require
from workbench_tests import WorkbenchBoard


def digest(source):
    return hashlib.sha256(source.encode("utf-8")).hexdigest()


def backup(board, directory):
    directory.mkdir(parents=True, exist_ok=False, mode=0o700)
    info = board.call("info")
    expect_ok(info)
    require(not info.get("job") or info["job"]["state"] != "running", "Existing job running; not stopping it")
    listing = board.call("list")
    expect_ok(listing)
    files = []
    for entry in listing["files"]:
        name = entry["name"]
        require(re.fullmatch(r"[A-Za-z0-9_-]{1,36}\.lua", name), "Unexpected device filename")
        content = board.call("get", name=name)
        expect_ok(content)
        require(digest(content["source"]) == content["sha256"], "Readback SHA mismatch")
        target = directory / name
        target.write_text(content["source"], encoding="utf-8")
        target.chmod(0o600)
        files.append({"name": name, "sha256": content["sha256"], "bytes": len(content["source"].encode())})
    manifest = {"at": datetime.now(timezone.utc).isoformat(), "board": info, "files": files}
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("BACKUP_PASS", len(files), "files; contents not printed", flush=True)


def verify(board, directory):
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    verified = []
    for entry in manifest["files"]:
        content = board.call("get", name=entry["name"])
        expect_ok(content)
        require(content["sha256"] == entry["sha256"] == digest(content["source"]), "Existing file changed")
        verified.append({"name": entry["name"], "sha256": content["sha256"]})
    info = board.call("info"); expect_ok(info)
    (directory / "preservation.json").write_text(json.dumps({"at": datetime.now(timezone.utc).isoformat(), "ok": True, "verified": verified, "board": info}, indent=2) + "\n", encoding="utf-8")
    print("PRESERVATION_PASS", len(manifest["files"]), "files unchanged", flush=True)


def receipt(proof):
    base = "http://127.0.0.1:5174"
    with urlopen(base + "/api/simulator/status", timeout=10) as response:
        token = json.load(response)["token"]
    keys = ("id", "sourceSHA", "modelSHA", "scenarioSHA", "coreSHA", "runtimeSHA")
    request = Request(base + "/api/simulator/verify", data=json.dumps({key: proof[key] for key in keys}).encode(),
                      headers={"Origin": base, "Content-Type": "application/json", "X-Ryzobee-Session": token})
    with urlopen(request, timeout=10) as response:
        require(json.load(response)["valid"], "Simulation receipt is no longer valid")


def run(board, directory, seconds):
    source = (directory / "main.lua").read_text(encoding="utf-8")
    proof = json.loads((directory / "simulation.json").read_text(encoding="utf-8"))
    require(proof["passed"] and digest(source) == proof["sourceSHA"], "Source not tied to passed simulation")
    receipt(proof)
    before = board.call("info"); expect_ok(before)
    require(before.get("neuro_core") == proof["runtimeSHA"], "Board/simulation core mismatch")
    require(before.get("neuro_runtime") == proof["runtime"] and before.get("neuro_catalog") == proof["catalog"], "ABI mismatch")
    require(not before.get("job") or before["job"]["state"] != "running", "Existing job running; will not stop it")
    name = "neuro_" + uuid.uuid4().hex[:12] + ".lua"
    listing = board.call("list"); expect_ok(listing)
    require(not any(f["name"] == name for f in listing["files"]), "Refusing to overwrite existing file")
    written = board.call("put", name=name, source=source, sha256=proof["sourceSHA"]); expect_ok(written)
    readback = board.call("get", name=name); expect_ok(readback)
    require(readback["source"] == source and readback["sha256"] == proof["sourceSHA"], "Readback mismatch")
    receipt(proof)
    started = board.command("lua --run-async --path " + name); expect_ok(started)
    job_id = started["job"]["job_id"]
    require(started["job"]["sha256"] == proof["sourceSHA"], "Running source differs")
    report = {"at": datetime.now(timezone.utc).isoformat(), "requested_seconds": seconds, "board_before": before,
              "proof": proof, "name": name, "started": started, "snapshots": [], "ok": False,
              "upload": {"sha256": written["sha256"], "bytes": written.get("bytes")},
              "readback": {"sha256": readback["sha256"], "bytes": len(readback["source"].encode())},
              "scope": "actual device, public serial protocol, no injected touch or LCD optical verification"}
    report["running_touch_counters"] = "cached; only post-stop board_after counters are authoritative"
    report_path = directory / (name + ".report.json")
    def save():
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    save()
    print("NEURAL_BOARD_STARTED", job_id, "source=" + proof["sourceSHA"], flush=True)
    clock = time.monotonic()
    next_check = clock
    try:
        # Close/reopen the serial link without a stop or reset; do not repeat the run command.
        board.close()
        time.sleep(1)
        board = WorkbenchBoard(board.serial.port)
        reconnected = board.call("info"); expect_ok(reconnected)
        require(reconnected["boot_id"] == before["boot_id"] and reconnected["job"]["job_id"] == job_id, "Reconnection changed task identity")
        report["reconnected"] = reconnected
        print("PASS disconnect/reconnect kept same boot and job; no run resent", flush=True)
        while time.monotonic() - clock < seconds:
            if time.monotonic() >= next_check:
                current = board.call("info"); expect_ok(current)
                require(current["boot_id"] == before["boot_id"] and current["job"]["job_id"] == job_id and current["job"]["state"] == "running", "Task ended unexpectedly")
                require(current["touch_errors"] == before["touch_errors"], "I2C errors")
                report["snapshots"].append(current); save()
                print("SOAK", round(time.monotonic() - clock), "s", "touches=" + str(current["touch_presses"] - before["touch_presses"]), flush=True)
                next_check = time.monotonic() + 30
            time.sleep(0.2)
        stop_clock = time.monotonic()
        stop = board.command("lua --stop " + job_id); expect_ok(stop)
        final = board.wait_job(job_id)
        elapsed = round((time.monotonic() - stop_clock) * 1000, 1)
        require(final["state"] == "stopped" and elapsed < 500, "Stop exceeded 500 ms or wrong state")
        report.update(final=final, stop_ms=elapsed, elapsed_seconds=round(time.monotonic() - clock, 1))
        # Normal regression query after the task stopped, not a second application or touch injection.
        report["pixels"] = board.call("eval", source="local d=require('display'); print(d.read_pixel(0,0),d.read_pixel(239,239))")
        expect_ok(report["pixels"])
        after = board.call("info"); expect_ok(after); report["board_after"] = after
        require(after["touch_errors"] == before["touch_errors"] and after["boot_id"] == before["boot_id"], "Device error/reset")
        require(after["free_psram_bytes"] >= before["free_psram_bytes"] - 1024, "PSRAM not recovered")
        report["ok"] = True; save()
        print("NEURAL_BOARD_PASS", "stop_ms=" + str(elapsed), "report=" + str(report_path), flush=True)
    finally:
        # Stop only this owned job on a failed check, never an unrelated application.
        if not report["ok"]:
            try:
                info = board.call("info")
                if (info.get("job") or {}).get("job_id") == job_id and info["job"]["state"] == "running":
                    board.command("lua --stop " + job_id)
            except (OSError, TimeoutError):
                pass
            save()
        board.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("command", choices=["backup", "verify", "run"])
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=5)
    args = parser.parse_args()
    require(1 <= args.seconds <= 1800, "Duration must be 1..1800 seconds")
    board = WorkbenchBoard(args.port)
    try:
        if args.command == "run":
            run(board, args.directory, args.seconds)
        else:
            (backup if args.command == "backup" else verify)(board, args.directory)
    finally:
        board.close()


if __name__ == "__main__":
    main()
