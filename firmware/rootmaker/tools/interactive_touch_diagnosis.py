#!/usr/bin/env python3
"""Run a reversible, instrumented copy of the deployed 2048 Lua app."""

import argparse
import base64
import hashlib
import json
import time
from datetime import datetime, timezone
from pathlib import Path

from board_lua import expect_ok
from workbench_tests import WorkbenchBoard


DEFAULT_ORIGINAL = "rb_a6c8bf5de5ff412dafaf.lua"
DEFAULT_ORIGINAL_SHA256 = "6dc3cc014158dd83c70bf71d863b1ca5b0a06773b93502c56f18923de4ab16de"
DIAGNOSTIC_NAME = "diag_2048_touch.lua"
TAG = "[DEBUG-T2048]"


def replace_once(source, old, new, label):
    count = source.count(old)
    if count != 1:
        raise ValueError(f"expected one {label} seam, found {count}")
    return source.replace(old, new, 1)


def instrument_source(source, source_sha):
    if not source.startswith("-- ryz-app/1\n"):
        raise ValueError("expected a ryz-app/1 source")

    source = replace_once(
        source,
        "local interruption_grace_ms = 140\n",
        """local interruption_grace_ms = 140
local dbg_samples, dbg_gap, dbg_miss, dbg_lost, dbg_done = 0, 0, 0, 0, 0
local dbg_previous, dbg_next = nil, 0
local dbg_missing = false
local function dbg(message)
  print('[DEBUG-T2048] ' .. message)
end
dbg('READY source=%s')
""" % source_sha[:12],
        "diagnostic state",
    )

    old_finish = """local function finish_touch(x, y)
  local dx = x - start_x
  local dy = y - start_y
  local short_press = math.abs(dx) < 20 and math.abs(dy) < 20
  if short_press and inside_new(start_x, start_y) and inside_new(x, y) then
    reset_game()
    return
  end
  if math.abs(dx) < 24 and math.abs(dy) < 24 then return end
  if math.abs(dx) > math.abs(dy) then
    apply_move(dx > 0 and 'right' or 'left')
  else
    apply_move(dy > 0 and 'down' or 'up')
  end
end
"""
    new_finish = """local function finish_touch(x, y)
  local dx, dy = x - start_x, y - start_y
  local result, changed = 'short', false
  if math.abs(dx) < 20 and math.abs(dy) < 20 and inside_new(start_x, start_y) and inside_new(x, y) then
    reset_game(); result, changed = 'new', true
  elseif math.abs(dx) >= 24 or math.abs(dy) >= 24 then
    if math.abs(dx) > math.abs(dy) then result = dx > 0 and 'right' or 'left'
    else result = dy > 0 and 'down' or 'up' end
    changed = apply_move(result)
  end
  dbg_done = dbg_done + 1
  dbg(string.format('END n=%d result=%s changed=%d dx=%d dy=%d', dbg_done, result, changed and 1 or 0, dx, dy))
end
"""
    source = replace_once(source, old_finish, new_finish, "finish_touch")

    old_loop = """reset_game()
while running do
  local p = touch.read()
  local now = board.millis()
  if p.event == 'down' and p.has_position then
    if active then
      last_x, last_y = p.x, p.y
      lost_at = nil
    else
      active = true
      start_x, start_y = p.x, p.y
      last_x, last_y = p.x, p.y
      lost_at = nil
    end
  elseif active and p.event == 'move' and p.has_position then
    last_x, last_y = p.x, p.y
    lost_at = nil
  elseif active and p.event == 'up' then
    if p.has_position then last_x, last_y = p.x, p.y end
    complete_active_touch()
  elseif active and p.pressed then
    if p.has_position then last_x, last_y = p.x, p.y end
    lost_at = nil
  elseif active then
    if lost_at == nil then
      lost_at = now
    elseif now - lost_at >= interruption_grace_ms then
      complete_active_touch()
    end
  end
  if dirty then draw_frame() end
  board.sleep_ms(20)
end
"""
    new_loop = """reset_game()
while running do
  local p = touch.read()
  local now = board.millis()
  dbg_samples = dbg_samples + 1
  if dbg_previous then local dt = p.sampled_ms - dbg_previous; if dt > dbg_gap then dbg_gap = dt end end
  dbg_previous = p.sampled_ms
  if p.event == 'down' and p.has_position then
    dbg(string.format('DOWN t=%d x=%d y=%d active=%d', now, p.x, p.y, active and 1 or 0))
    if active then last_x, last_y = p.x, p.y; lost_at = nil
    else active = true; start_x, start_y = p.x, p.y; last_x, last_y = p.x, p.y; lost_at = nil end
  elseif not active and p.pressed and p.has_position then
    if not dbg_missing then
      dbg_missing = true; dbg_miss = dbg_miss + 1
      dbg(string.format('MISS_START t=%d n=%d event=%s x=%d y=%d', now, dbg_miss, p.event, p.x, p.y))
    end
  elseif active and p.event == 'move' and p.has_position then
    last_x, last_y = p.x, p.y; lost_at = nil
  elseif active and p.event == 'up' then
    dbg(string.format('UP t=%d', now))
    if p.has_position then last_x, last_y = p.x, p.y end
    complete_active_touch()
  elseif active and p.pressed then
    if lost_at then dbg(string.format('RECOVER t=%d gap=%d event=%s', now, now - lost_at, p.event)) end
    if p.has_position then last_x, last_y = p.x, p.y end
    lost_at = nil
  elseif active then
    if lost_at == nil then lost_at = now; dbg_lost = dbg_lost + 1; dbg(string.format('LOST t=%d n=%d event=%s', now, dbg_lost, p.event))
    elseif now - lost_at >= interruption_grace_ms then complete_active_touch() end
  end
  if not p.pressed then dbg_missing = false end
  if dirty then local t = board.millis(); draw_frame(); dbg(string.format('DRAW ms=%d', board.millis() - t)) end
  if now >= dbg_next then dbg(string.format('STAT t=%d samples=%d max_gap=%d miss=%d lost=%d done=%d', now, dbg_samples, dbg_gap, dbg_miss, dbg_lost, dbg_done)); dbg_gap = 0; dbg_next = now + 1000 end
  board.sleep_ms(20)
end
"""
    source = replace_once(source, old_loop, new_loop, "main loop")

    # The diagnostic copy is disposable. Removing indentation and blank lines is
    # semantics-preserving here and leaves room below the device's 8192-byte cap.
    source = "\n".join(line.strip() for line in source.splitlines() if line.strip()) + "\n"
    size = len(source.encode("utf-8"))
    if size > 8192:
        raise ValueError(f"instrumented source exceeds device limit: {size}")
    return source


def output_text(response):
    return base64.b64decode(response["data_b64"]).decode("utf-8", errors="replace")


def stop_job(board, job_id):
    expect_ok(board.command("lua --stop " + job_id))
    job = board.wait_job(job_id)
    if job["state"] != "stopped":
        raise RuntimeError(f"job did not stop: {job}")
    return job


def artifact_directory():
    path = Path(__file__).resolve().parents[3] / "artifacts" / "touch-diagnostic"
    path.mkdir(parents=True, exist_ok=True)
    return path


def prepare(board, original_name, expected_original_sha):
    info = board.call("info")
    expect_ok(info)
    if info.get("job"):
        raise RuntimeError(f"device is busy: {info['job']}")
    listing = board.call("list")
    expect_ok(listing)
    if any(entry["name"] == DIAGNOSTIC_NAME for entry in listing["files"]):
        raise RuntimeError(f"refusing to overwrite existing diagnostic file: {DIAGNOSTIC_NAME}")
    original = board.call("get", name=original_name)
    expect_ok(original)
    source = original["source"]
    if hashlib.sha256(source.encode()).hexdigest() != original["sha256"]:
        raise RuntimeError("device source checksum mismatch")
    if expected_original_sha and original["sha256"] != expected_original_sha:
        raise RuntimeError(
            f"deployed source changed: expected {expected_original_sha}, got {original['sha256']}"
        )
    diagnostic = instrument_source(source, original["sha256"])
    diagnostic_sha = hashlib.sha256(diagnostic.encode()).hexdigest()
    directory = artifact_directory()
    (directory / "original.lua").write_text(source, encoding="utf-8")
    (directory / "diagnostic.lua").write_text(diagnostic, encoding="utf-8")
    manifest = {
        "created_at": datetime.now(timezone.utc).isoformat(),
        "boot_id": info["boot_id"],
        "firmware": info["firmware"],
        "original_name": original_name,
        "original_sha256": original["sha256"],
        "original_bytes": original["bytes"],
        "diagnostic_name": DIAGNOSTIC_NAME,
        "diagnostic_sha256": diagnostic_sha,
        "diagnostic_bytes": len(diagnostic.encode()),
        "touch_reads": info["touch_reads"],
        "touch_errors": info["touch_errors"],
        "touch_presses": info["touch_presses"],
        "touch_releases": info["touch_releases"],
        "device_files": [
            {"name": entry["name"], "sha256": entry.get("sha256"), "bytes": entry.get("bytes")}
            for entry in listing["files"]
        ],
        "recent_job": info.get("recent_job"),
    }
    (directory / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return info, diagnostic, diagnostic_sha, manifest, directory


def start(board, original_name, expected_original_sha, dry_run):
    info, diagnostic, diagnostic_sha, manifest, directory = prepare(
        board, original_name, expected_original_sha
    )
    print(json.dumps(manifest, ensure_ascii=False, indent=2), flush=True)
    if dry_run:
        print("DIAGNOSTIC_DRY_RUN_OK", flush=True)
        return
    uploaded = board.call("put", name=DIAGNOSTIC_NAME, source=diagnostic, sha256=diagnostic_sha)
    expect_ok(uploaded)
    fetched = board.call("get", name=DIAGNOSTIC_NAME)
    expect_ok(fetched)
    if fetched["source"] != diagnostic or fetched["sha256"] != diagnostic_sha:
        raise RuntimeError("diagnostic readback mismatch")
    response = board.command("lua --run-async --path " + DIAGNOSTIC_NAME)
    expect_ok(response)
    job_id = response["job"]["job_id"]
    if response["job"].get("name") != DIAGNOSTIC_NAME or response["job"].get("sha256") != diagnostic_sha:
        raise RuntimeError(f"unexpected diagnostic job identity: {response['job']}")
    log_path = directory / "live.log"
    print(f"DIAGNOSTIC_RUNNING job={job_id} log={log_path}", flush=True)
    with log_path.open("w", encoding="utf-8", buffering=1) as log:
        log.write(f"HOST start={datetime.now(timezone.utc).isoformat()} job={job_id} boot={info['boot_id']}\n")
        pending = list(board.events)
        board.events.clear()
        while True:
            event = pending.pop(0) if pending else board.read_response()
            if not event:
                continue
            if event.get("event") == "output" and event.get("job_id") == job_id:
                text = output_text(event)
                if TAG in text:
                    stamp = datetime.now(timezone.utc).isoformat()
                    for line in text.splitlines():
                        if TAG in line:
                            record = f"{stamp} {line}"
                            print(record, flush=True)
                            log.write(record + "\n")
            elif event.get("event") == "job" and event.get("job", {}).get("job_id") == job_id:
                record = f"{datetime.now(timezone.utc).isoformat()} JOB {json.dumps(event['job'], ensure_ascii=False)}"
                print(record, flush=True)
                log.write(record + "\n")
                if event["job"].get("state") != "running":
                    return


def restore(board, original_name, expected_original_sha, restart_original):
    info = board.call("info")
    expect_ok(info)
    job = info.get("job")
    if job:
        if job.get("name") != DIAGNOSTIC_NAME:
            raise RuntimeError(f"refusing to stop an unrelated job: {job}")
        stop_job(board, job["job_id"])
    removed = board.call("remove", name=DIAGNOSTIC_NAME)
    if not removed.get("ok") and "not found" not in str(removed).lower():
        raise RuntimeError(f"cannot remove diagnostic file: {removed}")
    result = {"removed": DIAGNOSTIC_NAME, "original": original_name, "restarted": False}
    if restart_original:
        original = board.call("get", name=original_name)
        expect_ok(original)
        if original.get("sha256") != expected_original_sha:
            raise RuntimeError(
                f"refusing to restart changed original: expected {expected_original_sha}, got {original.get('sha256')}"
            )
        response = board.command("lua --run-async --path " + original_name)
        expect_ok(response)
        result["restarted"] = True
        result["job"] = response["job"]
        time.sleep(0.5)
        verified = board.call("info")
        expect_ok(verified)
        running = verified.get("job") or {}
        if (
            verified.get("boot_id") != info.get("boot_id")
            or running.get("job_id") != response["job"].get("job_id")
            or running.get("state") != "running"
            or running.get("sha256") != expected_original_sha
        ):
            raise RuntimeError(f"restarted original did not remain active: {verified}")
        result["verified"] = {
            "boot_id": verified["boot_id"],
            "job_id": running["job_id"],
            "state": running["state"],
            "sha256": running["sha256"],
            "touch_errors": verified["touch_errors"],
        }
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--original", default=DEFAULT_ORIGINAL)
    parser.add_argument("--expected-original-sha", default=DEFAULT_ORIGINAL_SHA256)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--dry-run", action="store_true")
    action.add_argument("--start", action="store_true")
    action.add_argument("--restore", action="store_true")
    parser.add_argument("--restart-original", action="store_true")
    args = parser.parse_args()
    board = WorkbenchBoard(args.port)
    try:
        if args.restore:
            restore(board, args.original, args.expected_original_sha, args.restart_original)
        else:
            start(board, args.original, args.expected_original_sha, args.dry_run)
    finally:
        board.close()


if __name__ == "__main__":
    main()
