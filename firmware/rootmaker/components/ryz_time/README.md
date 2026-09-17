# Ryzobee time synchronization

This module consumes the provisioning module's ONLINE/OFFLINE state and owns
the single ESP-NETIF SNTP client. It never initializes Wi-Fi, NVS, netifs, or
the default event loop.

## Interface and failure behavior

The sole SystemServices owner initializes this module and periodically calls
`ryz_time_set_network_ready(bool)`. An online transition starts SNTP asynchronously
and a fixed **60-second monotonic initial-sync window**. Re-publishing ONLINE
does not restart or extend a live window. Expiry publishes `FAILED`, returns
`ESP_ERR_TIMEOUT`, and does not immediately restart. The existing owner's
**five-second retry backoff** then applies; its later ONLINE call begins a new
window. No extra worker, timer, synchronous-wait semaphore or public tick API is
introduced. `get_snapshot` remains a coherent, read-only copy with no clock or
network I/O.

Disconnecting ends local waiting immediately, including during retry backoff.
It does not invalidate an already calibrated clock and does not stop the SDK
client. Reconnecting starts a fresh window. Timeout also preserves a previously
valid clock; lack of a new response is not proof that the previous calibration
was lost. Unix time remains UTC, and the unchanged POSIX timezone only affects
local-time conversion.

The SDK's callback publishes a thread-safe snapshot. A value before 2024-01-01
clears `clock_valid` and `last_sync_unix`; the next ONLINE call reports this
asynchronous error once so the owner waits before retrying. It must not restart
every 200ms after an invalid response. A valid late callback restores the current
clock observation even after timeout/offline; offline remains `WAITING_NETWORK`.
The normal start return cannot overwrite a newer callback with an older error.

Snapshot locking never spans the synchronous SDK start/IPC call, because a
TCP/IP callback also takes that mutex. Initialization and readiness publication
are single-owner operations, not concurrent/reentrant controls. The existing
callback/snapshot seam remains thread-safe.

## Current UTC sampling

`ryz_time_sample_utc(ryz_time_utc_sample_t *)` is the nonwaiting timestamp
interface. It returns `unix_seconds` plus `valid`; `last_sync_unix` remains only
the historical sync point and must not be used as current time. The new call
performs one mutex try-lock and one local monotonic-clock read, with no network
operation, allocation, timezone conversion or wait. Non-NULL output is cleared
before any failure: missing initialization returns `ESP_ERR_INVALID_STATE`,
contention returns `ESP_ERR_TIMEOUT`, and no usable calibration returns
`ESP_OK` with `valid=false`. Unknown is not Unix epoch zero.

The immediate-mode SDK callback captures the monotonic observation before it
can wait for the snapshot lock. The core atomically retains its UTC seconds,
microsecond remainder and monotonic anchor under that lock. Sampling acquires
the same lock before reading the monotonic clock, advances the anchor with
checked arithmetic, and rounds down only the resulting UTC value. Thus a
delayed callback does not make its old UTC observation appear newer, and a
reader cannot combine one callback's seconds with another callback's anchor.

Calibration remains useful offline and after a synchronization timeout. An
unsynchronized boot, NULL/invalid SDK time value, monotonic regression, or
arithmetic overflow leaves the sample unknown until a fresh valid sync. A
read high-water mark also detects regression that remains after the original
anchor. Derived regression/overflow invalidates this private UTC sample only;
it does not rewrite the existing public sync snapshot or its owner retry
policy. Invalid callbacks still clear `clock_valid` and `last_sync_unix` as
documented above. Consumers must use the sample's own `valid` flag, not infer
it from `snapshot.clock_valid`.

A fresh calibration can legitimately move UTC backwards. This is calibrated
UTC, not authenticated time, a monotonic transaction identifier, a timezone
policy, or a claim of long-term oscillator accuracy. The int64 arithmetic
bounds do not extend the locked SNTP protocol's supported date range.

## SDK and evidence boundaries

The locked ESP-IDF 5.5.4 defaults to immediate sync; this Adapter now also sets
`smooth_sync=false` explicitly. The SDK starts the client during
`esp_netif_sntp_init`. Repeated `esp_netif_sntp_start` executes stop/init on TCP/IP;
success is not receipt of a time response. There is no asynchronous failure
callback. Default immediate mode calls `settimeofday` before notifying the
application; this module does not turn notifications into authenticated time
or claim a real network verification.

The 60-second window bounds local waiting when the owner next runs (normally
every 200ms). It is not a hard deadline for a blocked SDK call, an SDK shutdown
acknowledgement, or tracking of each hourly background refresh. SDK stop does not
cancel every pending DNS callback, and sync notifications contain no attempt ID.
The revision guard only prevents a stale synchronous return from overwriting a
newer observation; it is not a network-request epoch.

## 2026-09-06 verification

The original no-response regression failed twice against the real core in
0.764s / 0.631s: after advancing the Host monotonic clock to 60s, ONLINE did not
return `ESP_ERR_TIMEOUT`. The same test passes after the fix. The missing seam
was elapsed-time observation, not another network retry worker: the owner
already had error backoff, while the core treated a successful start as an
unbounded wait for the SDK's success-only callback.

Final root run: **45 unittest items PASS, 14.362s**. Counts are time core 4,
real time/SystemServices combination 4, SystemServices 19, OTA core 1, OTA ESP
Adapter 4, network RPC 5 and network CLI 8. The time tests use strict C11,
pthread and ASan/UBSan. They cover exact deadline edges, start at zero/nonzero,
large monotonic values, getter purity, retained clock, both late-callback orders,
start-return interleaving, invalid clock reporting and actual owner backoff.

The combination links real `ryz_time.c` and the real SystemServices owner;
controlled SNTP, pthread scheduling and other dependency Adapters remain Host
substitutes. Its OTA observations prove prerequisite propagation, not a full
time-to-HTTPS/OTA or device test. No host wall clock was modified, and no network
server, serial port or device was accessed by these tests.

Run the final checks from the repository root:

```sh
IDF_PATH=/path/to/espvm/esp-idf/versions/v5.5.4/esp-idf \
python3 -m unittest \
  firmware/rootmaker/components/ryz_time/test_host/test_time.py \
  firmware/rootmaker/tests/test_system_time_integration.py \
  firmware/rootmaker/tests/test_system_services.py \
  firmware/rootmaker/components/ryz_ota/test_host/test_ota.py \
  firmware/rootmaker/components/ryz_ota/test_host/test_ota_esp.py \
  firmware/rootmaker/tests/test_workbench_network_rpc.py \
  firmware/rootmaker/tests/test_network_cli.py -q
cmake --build /private/tmp/ryz-v5-native.ASZc65 --parallel 8
ctest --test-dir /private/tmp/ryz-v5-native.ASZc65 --output-on-failure \
  -R 'v5_(network_ui|network_setup|telemetry|password_ui|version_ui)$'
sh firmware/rootmaker/tools/check_lua_freeze.sh
```

Selected native LVGL/FreeType regressions: **5/5 PASS, 8.52s**, after rebuilding
their actual source. No new rendering or screenshot review is claimed; V5
geometry, fonts, BOE/TP initialization and LCD SPI 80MHz were not changed.
Lua/dependency/config/partition freeze and modified-file whitespace checks pass.

ESP-IDF 5.5.4 / ESP32-S3 build and link pass using the existing isolated build
and SDK config. Run from `firmware/rootmaker`:

```zsh
export IDF_TOOLS_PATH=/path/to/espvm/esp-idf/versions/v5.5.4/tools
source /path/to/espvm/esp-idf/versions/v5.5.4/esp-idf/export.sh
idf.py -B /private/tmp/ryzobee-p3-catalog.cjYAiI/target \
  -D SDKCONFIG=/private/tmp/ryzobee-p3-catalog.cjYAiI/sdkconfig build
```

`target/ryzobee_rootmaker.bin` is **1,808,976 bytes**, leaving **1,336,752 bytes**
in the 3MiB app slot. SHA-256:
`5ef511fa2d6ec491b25b011a1d43bed54ac7da5c63a23a7fedf26ea3cc87d7ec`.
The linked map contains both the deadline-enabled readiness function and the
ESP monotonic clock Adapter. Generated config still uses `pool.ntp.org`, `UTC0`
and the SDK's 3,600,000ms refresh interval. `scripts.bin` remains unchanged:
`300f85876ce59b5c961516d4af92bd70c68b6fd6e42634eb245ed91f3a7ea12a`.

These temporary artifacts are not release delivery. The full V5 goal, timezone
product choice, other network workflows, BLE and complete OTA are still open.
No Figma edits, device enumeration, serial access, reset or flash occurred;
physical acceptance remains with the user after full-scope implementation.

## Current UTC increment verification

The checks below exercise the current UTC interface separately from the
historical target-build record above. They link the actual time core, the
actual ESP callback Adapter where named, and the actual SystemServices/time
combination. SDK, OS scheduling and monotonic observations are controlled Host
substitutes; no network or host wall-clock modification is performed.
The combined run passed **14/14 unittest items in 8.144s**: time core 5,
real core/ESP Adapter 5, and real time/SystemServices 4.

```sh
python3 -m unittest \
  firmware/rootmaker/components/ryz_time/test_host/test_time.py \
  firmware/rootmaker/components/ryz_time/test_host/test_time_esp.py \
  firmware/rootmaker/tests/test_system_time_integration.py -v
```

The core suite includes subsecond carry, offline/timeout retention, invalid
observations, signed/unsigned arithmetic boundaries, sticky faults and fresh
sync recovery. Real pthread tests check immediate lock contention and coherent
concurrent callback/sample reads. The real ESP Adapter suite invokes its
registered SDK callback, checks explicit IMMED configuration, and holds the
snapshot mutex while advancing the monotonic fixture to prove that callback
time is captured before lock waiting. Strict C11 warnings and ASan/UBSan are
enabled. These checks do not replace the parent task's final ESP32-S3 build or
physical time/network acceptance.
