# Ryzobee provisioning

This component owns the ESP-IDF Wi-Fi driver, default STA/AP netifs, an HTTP
configuration portal, and a captive DNS task. It does not render UI and its
ESP event handler only publishes state into the thread-safe snapshot.

## Integration seam

Call `ryz_provisioning_init()` once, then `ryz_provisioning_start()`. Repeating
`start()` while AP or STA service is already active is an idempotent no-op. Poll
`ryz_provisioning_get_snapshot()` from the display/UI owner. Settings/RX queue
ON/OFF/REPROVISION through `ryz_system_services_request_network()`; only its C
owner calls the potentially blocking provisioning control functions. OFF
preserves credentials, and later ON confirms previous teardown before starting
saved STA or default AP. Explicit ON/OFF first commits the next-boot preference
to NVS key `ryz_prov/wifi_on` (u8, only 0/1); initialization restores it, while a
missing record defaults ON for first provisioning or older firmware. Other
read errors fail initialization without starting the radio. Save failure leaves
the current radio unchanged, invalidates the preference cache and requires an
explicit retry; radio failure after commit does not roll the preference back.
Same-value requests skip only the redundant NVS write, not radio control.
Temporary OPEN_AP and legacy REPROVISION do not change that preference.
Explicit REPROVISION first confirms stop, then erases only the saved STA record
and returns to AP mode. Failed stop cannot erase credentials or report OFF.

The snapshot includes the per-device AP SSID, password, and actual SoftAP IPv4
address so trusted local UI can encode a Wi-Fi QR payload and show the portal
address. During STOPPING or failed, unconfirmed teardown, retained portal
fields describe last-observed resources, not verified reachability. `ipv4`
remains the station address and is populated only after validated got-IP. The AP
password is generated from device entropy on first initialization and persisted
separately from STA credentials; it is not a product-wide constant and is not
returned by the HTTP API.

## Portal contract

- `GET /` serves the self-contained setup page.
- `GET /api/status` returns state, revision, AP SSID, STA SSID, IPv4 address,
  scan records, scan timing, session age, connection progress, timeout, and
  error code. Passwords are never included.
- `POST /api/scan` queues a bounded 2.4 GHz scan on the provisioning worker.
- `POST /api/provision` accepts
  `{"ssid":"network","password":"passphrase"}` and returns HTTP 202.
- `POST /api/cancel` invalidates the current STA attempt on target APSTA builds,
  disconnects the station from the network worker, and returns the browser to
  the still-running local portal. It does not erase the persisted credential
  record.
- Other HTTP paths redirect to `/`; the DNS server resolves IPv4 A queries to
  the SoftAP address. DHCP option 114 is also advertised when supported.

The target portal starts the radio in APSTA mode so the browser can observe
SELECT / JOINING / READY / FAILED without the HTTP server disappearing as soon
as credentials are submitted. ESP32-S3 APSTA shares one 2.4 GHz channel: when
the selected router is on another channel, the phone may temporarily lose the
SoftAP connection. The page treats that as a transport interruption and the
device continues the real station attempt.

SSID length is 1-32 bytes. Password length is either zero for an open network
or 8-63 printable ASCII bytes. Credentials are stored as one versioned NVS
blob in namespace `ryz_prov`. NVS encryption remains a project-level security
choice; this component does not erase or reformat the shared NVS partition when
initialization fails.

## Station recovery

The first saved-STA boot attempt now performs a directed active scan for the
exact saved SSID before calling `esp_wifi_connect()`. It does not use the
portal's strongest-16 cache. Only a matching 1–32-byte SSID on 2.4 GHz admits
the attempt; an empty, truncated, or case-different result is not a match.
Hidden APs can respond to the directed probe, but a missing response skips
the attempt too. A completed miss returns NOT_FOUND; scan/record errors retain
their distinct error. Driver scan records are released on success and error.

This scan runs on the service owner under the existing network mutex, with
no snapshot lock held. The UI reads `boot_phase` directly while the aggregate
service snapshot may still be waiting for the scan. Missing/failed scans
immediately enter the existing one-shot, password-protected AP fallback;
saved credentials and ON/OFF remain unchanged. Matching SSIDs retain the
existing ten-second boot connection window, measured after the scan returns.
Saved OFF does not scan or connect. Explicit ON and HTTP submissions retain
their existing policy; this change is scoped to automatic boot restoration.
The boot phase retains the original missing/failed result across AP fallback.

Transient station disconnects receive at most three reconnect attempts with
500 ms, 1000 ms, and 1500 ms backoff. Authentication, handshake, cipher, and
configured-security incompatibility reasons fail immediately so the user can
choose **REPROVISION** from Settings. Exhausting transient retries also enters
`FAILED`; saved credentials are retained until that explicit reprovision
action.

The mailbox/retry carries the generation current when software captures it;
the SDK event itself has no original attempt epoch. AP/STA changes, reconnect
attempts and terminal failures advance the generation. Got-IP additionally
requires the owned netif, actual desired-SSID association, netif-up and live
IPv4. A delayed same-SSID disconnect while actually unassociated cannot be
attributed to its original attempt; it is handled as current disconnection.
No software generation claim replaces that explicit SDK limitation.

The ESP event callback only replaces a small, generation-stamped mailbox with
the latest physical STA state and wakes the worker. The mailbox also preserves
whether the same batch observed got-IP. Backoff deadlines are held by that
worker rather than blocking sleeps. Raw got-IP observations cannot consume a
retry without current association validation; ignored foreign/coalesced events
also preserve the existing retry ticket.

`AP_READY` is published after the SoftAP address is available and the portal
services have started. When DHCP can be paused, a successful restart is
mandatory; an unexpected pause error is treated as a degraded, already-running
service and logged. Captive-portal option 114 remains best effort. The DNS task
owns its self-deletion; stop requests shutdown and waits at most 1000 ms for its
exit acknowledgement, then closes/frees confirmed resources. Timeout or close
failure retains a cleanup-only handle for retry. This bounds only the ACK wait,
not lwIP close/httpd_stop. Failed HTTP stop likewise retains its handle, and all
new AP/STA starts must confirm previous teardown first. AP_READY publication
checks the starting revision so a later HTTP/worker event is never overwritten.

Platform initialization publishes its event sink only after all resources are
ready. On failure it unregisters only handlers it registered, deletes its task,
queue and mutex, deinitializes its Wi-Fi instance, destroys only its default
netifs, and deletes the default event loop only when this component created it;
a subsequent initialization attempt is therefore permitted. Default netifs are
assembled with error-returning ESP-IDF primitives so allocation or attachment
failure follows this rollback path instead of aborting the firmware. If AP
identity preparation fails in the narrow interval after platform commit, the
core invokes the same retained ownership plan before allowing initialization
to be retried.

## Verification

Run the state-machine contract tests with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
python3 -m unittest components/ryz_provisioning/test_host/test_provisioning.py
```

An ESP-IDF build compiles the network adapter. A real board is still required
to validate RF association, captive-portal behavior across phones, NVS reboot
persistence, and recovery from an incorrect Wi-Fi password.

The current native ESP/DNS/OTA coordination harness and evidence boundaries are
recorded in [network control](../../../../docs/software/firmware-network-control.md).
Persistent-switch tests, UI error feedback and the current target artifact are
recorded in [network preference](../../../../docs/software/firmware-network-preference.md).
The existing form still persists credentials before connecting; cancellation
keeps that record so a later explicit retry can reuse the same policy.
