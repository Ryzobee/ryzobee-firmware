# Configurable I2C scan worker

The exact default SDA41/SCL40/100 kHz borrows the board-owned I2C0 bus. Custom
configurations use two eligible, distinct GPIOs and I2C1 at 100 or 400 kHz.
The shared touch/IMU bus is never deleted, rebuilt or reconfigured. No raw bus
handle or register access is exposed to Lua. A scan is explicit work, not a boot
action or continuous device monitor. The current wire contract is
`ryz-i2c-scan/2`; see [configuration, pins and SDK constraints](../../../../docs/software/firmware-tool-i2c.md).

`configure(config, expected_revision, out_revision)` validates a complete
candidate and atomically saves software settings only while inactive and free
of retained resources. It does not claim pins, create a bus or probe. Current
configuration starts at revision 1 and advances without wrapping. Each start
freezes its own `scan_config/scan_config_revision`, so subsequent configuration
changes cannot relabel historical results. Worker begin rechecks resource
availability and acquires a paired token through `ryz_tool_pins`.

## Request and lifecycle contract

`ryz_i2c_scan_start()` lazily allocates one mutex, one binary wake event and one
boot-lifetime worker (priority 2, 4096-byte stack, pinned CPU1), then publishes
one QUEUED request. It performs no I2C in the caller. Success returns the queued
scan ID, not a completed scan or hardware PASS. A fast worker may already have
advanced the phase when the caller reads its next snapshot.

Startup is atomic. Partial mutex/event allocation rolls back; failed task
creation retains no worker and frees both resources. The first caller keeps
STARTING admission until its first operation commits or rejects, so concurrent
startup is rejected. Configure can also initialize this worker without scanning.
Later requests reuse the same worker. QUEUED/RUNNING/CANCELLING/RELEASING rejects another
start. IDs increase within the boot; UINT32_MAX exhaustion rejects new starts
rather than wrapping or reusing an ID. Outputs are cleared on failure.

Only the worker calls the private bus begin/probe/end Adapter. Default mode uses
`ryz_board_i2c_init()` and `ryz_board_i2c_try_probe()`; custom mode owns a synchronous
I2C1 bus and one address-less device for START/address/STOP operations. Explicit
device timing supplies the requested clock; IDF's fixed-100-kHz probe is not used
for custom mode. Initialization failure retains its real error and zero completed
addresses. Partial custom resources are cleaned before publishing a terminal
state; a later explicit start can retry without rebuilding the shared bus.

`ryz_i2c_scan_cancel(id)` checks the current ID. A valid active request becomes
CANCELLING; the caller does not perform I2C or claim completion. CANCELLED is
published by the worker only after an admitted/in-flight probe has returned and
RELEASING has acknowledged resource cleanup. Queued cancellation performs cleanup
without new board initialization or any probe. A late cancel during RELEASING
does not hide that phase or admit a new scan.
An old ID cannot affect a later scan; repeating cancel for the same CANCELLED
ID succeeds. FAILED with retained resources accepts the same ID for cleanup-only
retry: no begin/probe is issued. Other terminal cancellations are rejected.
Cleanup errors retain `cleanup_error/resources_held`; they never report COMPLETED,
CANCELLED or free pins prematurely. Active resource snapshots are observations,
not permission to bypass lifecycle admission.

The worker rechecks cancellation before admitting every probe and after its
return. The result of a probe cancelled while in flight is discarded. No later
probe from that ID can start after CANCELLED. A cancellation during the fixed
yield waits until the worker resumes; it cannot synchronously claim completion.

## Bounded traversal and result meaning

Addresses 0x08 through 0x77 are visited exactly once as address slots (112 total).
Each slot makes at most four `try_probe` calls. Only NOT_FINISHED, meaning shared
bus contention before physical admission, is retried. There is a 5 ms scheduling
delay request between retries and between addresses, rounded up to FreeRTOS ticks
with a minimum of one tick. It is not a physically exact 5 ms guarantee or a hard
whole-scan deadline; task scheduling and finite board-driver operations add time.
Wake events do not shorten this fairness delay.

| Board result | Address result | Meaning |
| --- | --- | --- |
| OK | ACK | Address acknowledged, not a device-model identification |
| NOT_FOUND | NACK | Physical probe reported no acknowledgement |
| NOT_FINISHED after four attempts | BUSY | All admission attempts contended; presence unknown |
| TIMEOUT | TIMEOUT | Real probe/driver timeout; not absence and not retried |
| Other error | ERROR | Exact error retained; traversal continues |

COMPLETED means traversal finished, including uncertain address outcomes. It does
not mean every device or peripheral passed. Only COMPLETED with 112 NACKs and no
uncertain/unscanned slots is a conclusive empty scan. All unvisited and reserved
addresses remain UNSCANNED; their zero-initialized error field does not mean ACK.

`probe_calls` counts admitted calls to `try_probe`, including contention and an
in-flight call discarded by cancellation; it is not a physical-probe count.
It is bounded by 112 × 4 = 448. `busy_responses` counts observed contention
responses before cancellation, including retries for unfinished slots. A response
discarded after cancellation does not increment it. Result-category counters
count finalized address slots and sum to `completed_addresses`; results and
per-address errors are published together under the snapshot mutex.

`current_address` is cleared in every terminal state. Timestamps are boot-monotonic
milliseconds, not UTC. Snapshots are historical observations, not live presence.

## Scheduling and cache safety

`get_snapshot()` is a bounded-size cached copy with no allocation, I2C or implicit
worker creation. Before startup completes it clears output and returns
INVALID_STATE. The mutex is never held over board initialization, a probe, a
delay or the idle wait. Status and cancellation requests remain available during
a blocked physical probe.

Idle work waits on a binary event, not a polling loop. Wakeups can coalesce and
can arrive before the wait: the worker always checks protected request state
before sleeping. A stale wake may cause one extra state check, but cannot lose a
new request, revive an old scan or publish a previous generation's result.

## Verification boundary

From the workspace root:

```sh
python3 -m unittest firmware/rootmaker/tests/test_i2c_scan.py -v
```

Host cases run the real worker on a pthread with controlled time/yields and a
bus Adapter: startup retries, concurrent startup/configure, configuration CAS,
historical results, 112-address traversal, the 448-call contention bound, distinct
error handling, cancellation at multiple phases, blocked-probe snapshot access,
cleanup-only retry and new-generation isolation. The real ESP Adapter is also
linked with FreeRTOS mutex/event/task allocation failures, confirming rollback
and fixed-core task settings. ASan/UBSan are enabled.
The ID exhaustion branch is statically guarded; tests do not execute billions
of scans or mutate private state to manufacture that condition.

Separate `test_i2c_scan_integration.py` tests the real scanner with the actual
JSON RPC Adapter. `test_i2c_scan_bus.py` executes the production bus and read-only
IDF diagnostic Adapter against controlled SDK boundaries; `test_tool_pins.py`
checks eligibility, atomic leases and contention. Exact counts, target artifact
and remaining work are recorded in the linked current contract. These tests do
not prove physical I2C waveforms, timing, device identity, UI navigation or on-board
hardware PASS. No device operation is part of this implementation batch.
