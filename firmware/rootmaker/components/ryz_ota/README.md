# Ryzobee HTTPS OTA

This module keeps the deployment URL and ESP-IDF transfer details behind a
small firmware interface (`include/ryz_ota.h`). It is compiled even when the
URL is empty; that default produces the explicit `RYZ_OTA_DISABLED` state.

## Integration contract

1. Call `ryz_ota_init()` to inspect the running image and configure the local
   state. Initialization itself does not start a transfer or require Wi-Fi.
2. On every orchestration tick, publish both provisioning ONLINE state and the
   time module's `clock_valid` flag with `ryz_ota_set_prerequisites()`. Starting
   an update is rejected until both are true.
3. Trigger `ryz_ota_start_update()` from the trusted local serial RPC or a
   future UI action. The URL is fixed by `CONFIG_RYZ_OTA_URL`; callers cannot
   inject one, and this module does not copy it into snapshots or its own logs.
4. After the display/UI and other required boot services are known healthy,
   call `ryz_ota_confirm_running_image_healthy()` once. Do not wait for Wi-Fi or
   NTP: a newly updated device must also survive its first unconfigured boot.
5. A successful transfer stops at `RYZ_OTA_READY_TO_REBOOT`. Reboot only through
   an explicit, authorized `ryz_ota_reboot_to_updated_image(attempt, guard)` call.
   Both wider-system guard callbacks are mandatory. The exact nonzero attempt
   must have finished its worker and successful cleanup, with no running-image
   confirmation pending. A separate `reboot_pending` reservation excludes
   duplicate restarts and new updates; network hold cannot revoke it. Callbacks
   and reboot run outside the snapshot mutex. If reboot unexpectedly returns,
   the guard is released exactly once and the API returns `ESP_FAIL`, retaining
   the candidate for explicit retry. A real reboot does not return `ESP_OK`.

Workbench connects this guard to the shared Job/file writer reservation and a
copy-only clean-Store check. Original V5 REBOOT binds the press to its presented
attempt. See [restart implementation and evidence](../../../../docs/software/firmware-v5-ota-restart.md).

`running_info` holds boot-cached RUNNING partition identity, separately typed
image state/error and observed A/B partition structure. Candidate/download
events do not mutate these facts. Confirmation rechecks the same partition and
reads back VALID before clearing pending; a successful SDK no-op alone is not
confirmation evidence. Missing/unsupported records are unknown, not VALID.
Workbench consumes this cache and boot-sampled chip revision without per-frame
Flash queries; it does not infer installation time, release channel or system
health from these fields. See [Version Info evidence](../../../../docs/software/firmware-v5-version-info.md).

The worker accepts only a lowercase `https://` URL, verifies the server with
ESP-IDF's CA bundle, disables redirects, rejects a candidate whose project name
differs from the running app or whose version is empty/unchanged, and checks
that the complete image was received. Cancellation is cooperative while
STARTING/DOWNLOADING. VERIFYING is the commit boundary because
`esp_https_ota_finish()` may switch the boot partition.

## Coordinating network shutdown

The single trusted network controller calls `ryz_ota_network_hold(true)` before
publishing a Wi-Fi OFF/forget/reprovision command. This accepts intent even
before OTA initialization and always returns `ESP_OK`; it is not a cancellation
or cleanup acknowledgement. The hold immediately blocks new starts and requests
cooperative cancellation in STARTING/DOWNLOADING. VERIFYING must finish normally;
the hold cannot undo a boot-partition commit. The VERIFY admission also checks
the atomic hold intent, including a caller still waiting for the snapshot mutex.

Keep the network alive until a successful snapshot reports both
`worker_active == false` and `cleanup_confirmed == true`. A candidate can be
rejected with state FAILED while its HTTPS handle is still live; FAILED alone
is never permission to stop networking or accept another update. Cancellation
dispatch failures retain `worker_active` and expose `last_error`. The controller
must fail closed if a snapshot cannot be read or cleanup is unconfirmed.

After publishing the new connectivity prerequisites, the controller may call
`ryz_ota_network_hold(false)`. Release does not start OTA or reconnect Wi-Fi.
Serialize release with the next network command so an old command cannot undo
a newer hold. OTA cancellation remains requested even if the hold is released
before the worker finishes aborting. Each accepted attempt has a boot-local,
non-wrapping `attempt_id`; stale events cannot release or mutate a newer worker.

### Cleanup evidence and SDK boundary

The ESP Adapter is compile-locked to ESP-IDF 5.5.4. In that SDK, successful
`esp_https_ota_begin()` produces a BEGIN/RESUME handle, and this Adapter's later
calls only advance it to IN_PROGRESS/SUCCESS. In those valid states, both
`esp_https_ota_abort()` and `esp_https_ota_finish()` consume the HTTPS/HTTP handles,
including their error returns. The Adapter therefore calls cleanup once only;
it never aborts after a failed finish or retries a freed abort handle.

`cleanup_error` records an abort error separately from the original transfer or
candidate error. It can be nonzero while `cleanup_confirmed` is true: handle
consumption and operation success are different facts. Finish validation or
boot-selection errors remain `last_error`, with confirmed consumption. A begin
failure normally leaves a NULL handle; an unexpected non-NULL output is instead
reported as unconfirmed cleanup and blocks further starts until reboot.

Audited SDK sources (relative to the ESP-IDF 5.5.4 checkout):

- `components/esp_https_ota/src/esp_https_ota.c`: begin cleanup 538–543;
  finish 857–902; abort 905–939.
- `components/app_update/esp_ota_ops.c`: abort 438–448; end 478–523.
- `components/esp_http_client/esp_http_client.c`: cleanup 1020–1059.

Terminal publication occurs after the worker's last SDK/resource access; only
task self-deletion follows. It does not claim that the RTOS idle task has already
reclaimed the task stack. SDK transport close errors are not all propagated by
SDK cleanup; the confirmation describes the audited API ownership contract,
not an independent physical socket/flash verification. Re-audit these branches
before changing the SDK version.

Rollback confirmation becomes effective when
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is set by the application. The module
does not enable that project-wide setting itself.

Do not put credentials, user-info, or query secrets in `CONFIG_RYZ_OTA_URL`.
The configured string remains discoverable in the firmware binary, and lower
ESP-IDF layers may include it in diagnostic logs on parsing failures.

## Host verification

From `firmware/rootmaker`, run without an ESP-IDF toolchain:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  components/ryz_ota/test_host/test_ota.py \
  components/ryz_ota/test_host/test_ota_esp.py -v
```

The suite runs 29 public-core scenarios and 59 real-core + real-ESP-Adapter
scenarios under AddressSanitizer/UndefinedBehaviorSanitizer. It includes the
candidate-rejected/abort-before-free second-start regression; pre-init and
in-flight holds; both sides of the VERIFY admission boundary; stale attempts;
cleanup uncertainty; consumed-on-error SDK handles; and mutex/task allocation
failure recovery. The VERIFY race uses a real pthread hold caller paused at the
Adapter's snapshot mutex, not a rewritten copy of the OTA state machine.

SDK HTTPS/flash and FreeRTOS allocation/task calls are controlled substitutes.
These Host results do not execute a real SDK hardware allocator, download an
image, switch a physical boot partition, or prove OTA/rollback on a device.
