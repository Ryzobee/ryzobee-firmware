#pragma once

#include <stdbool.h>

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True only for the already-running password-protected AP and HTTP server.
 * Does not create a network or imply that STA-facing diagnostics are allowed. */
bool ryz_provisioning_diagnostics_ready(void);

#define RYZ_PROVISIONING_SSID_MAX_LENGTH 32
#define RYZ_PROVISIONING_PASSWORD_MAX_LENGTH 63
#define RYZ_PROVISIONING_IPV4_MAX_LENGTH 15

/** Last sampled diagnostics for the verified current STA connection. Flags
 * are independent of the numerical values (RSSI may be slightly positive).
 * All fields are cleared on connection/stop transitions. Readers must also
 * bound age using sampled_at_us; getters perform no SDK sampling. IPv6 is an
 * actually assigned preferred global address, otherwise preferred link-local;
 * it does not imply Internet reachability or enable IPv6 on the interface. */
typedef struct {
    bool mac_valid;
    bool rssi_valid;
    bool ipv6_valid;
    char mac[18];
    char ipv6[40];
    int8_t rssi_dbm;
    /* STA L2 boundary bytes in one verified pointer/BSSID session. RX is
     * ingress handed to ESP-NETIF, TX is accepted by its driver; neither is
     * application goodput or a radio ACK. Counters and epoch never wrap.
     * First observation is only a baseline; consumers calculate elapsed rate.
     * Invalid traffic does not invalidate independent RSSI/MAC observations. */
    bool traffic_valid;
    uint32_t traffic_epoch;
    uint64_t traffic_rx_bytes;
    uint64_t traffic_tx_bytes;
    uint64_t sampled_at_us;
    esp_err_t last_error;
} ryz_provisioning_link_info_t;

typedef enum {
    RYZ_PROVISIONING_UNCONFIGURED = 0,
    RYZ_PROVISIONING_AP_STARTING,
    RYZ_PROVISIONING_AP_READY,
    RYZ_PROVISIONING_CREDENTIALS_RECEIVED,
    RYZ_PROVISIONING_CONNECTING,
    RYZ_PROVISIONING_ONLINE,
    RYZ_PROVISIONING_FAILED,
    RYZ_PROVISIONING_STOPPING,
    RYZ_PROVISIONING_OFF,
} ryz_provisioning_state_t;

/** Boot-only saved-network progress. Retained across AP fallback so the
 * splash can show why STA was skipped/failed, not falsely report connected. */
typedef enum {
    RYZ_PROVISIONING_BOOT_WAITING = 0,
    RYZ_PROVISIONING_BOOT_OFF,
    RYZ_PROVISIONING_BOOT_NO_CREDENTIALS,
    RYZ_PROVISIONING_BOOT_SCANNING,
    RYZ_PROVISIONING_BOOT_NOT_FOUND,
    RYZ_PROVISIONING_BOOT_SCAN_FAILED,
    RYZ_PROVISIONING_BOOT_CONNECTING,
    RYZ_PROVISIONING_BOOT_CONNECTED,
    RYZ_PROVISIONING_BOOT_CONNECT_FAILED,
} ryz_provisioning_boot_phase_t;

/**
 * Copy-only view of provisioning state. Event callbacks update this snapshot;
 * UI/display code may poll it without participating in network callbacks.
 *
 * ap_password is intentionally available to trusted firmware so the local
 * screen can render a Wi-Fi QR code. The HTTP status endpoint never exposes it.
 */
typedef struct {
    ryz_provisioning_state_t state;
    uint32_t revision;
    esp_err_t last_error;
    int32_t detail;
    /** Current-boot intent, initialized from the saved ON/OFF preference.
     * Temporary portal/reprovision operations can differ from that preference.
     * stop_confirmed is true only before first start or after platform stop
     * confirms all owned portal/radio services have stopped. FAILED is not OFF. */
    bool enabled;
    bool stop_confirmed;
    bool credentials_stored;
    /** Only the first actual saved-STA boot attempt may fall back to AP.
     * False for an initial portal/HTTP submission, restored OFF, after the
     * first verified ONLINE event, any explicit control, or fallback claim.
     * FAILED can remain pending after a synchronous saved-STA start failure. */
    bool boot_restore_pending;
    ryz_provisioning_boot_phase_t boot_phase;
    /** Last observed portal state. During STOPPING or an unconfirmed failed
     * stop this may describe retained resources, not a reachable portal. */
    bool portal_active;
    char ap_ssid[RYZ_PROVISIONING_SSID_MAX_LENGTH + 1];
    char ap_password[RYZ_PROVISIONING_PASSWORD_MAX_LENGTH + 1];
    /** Last observed SoftAP address, cleared after confirmed stop. Do not
     * advertise reachability while STOPPING or after an unconfirmed failure. */
    char portal_ip[RYZ_PROVISIONING_IPV4_MAX_LENGTH + 1];
    char sta_ssid[RYZ_PROVISIONING_SSID_MAX_LENGTH + 1];
    /** Station IPv4 address; empty unless the station is online. */
    char ipv4[RYZ_PROVISIONING_IPV4_MAX_LENGTH + 1];
    ryz_provisioning_link_info_t link;
} ryz_provisioning_snapshot_t;

/** Initialize NVS/network ownership, read ON/OFF and create the AP identity.
 * Missing preference defaults ON; read/type/value errors fail initialization
 * without starting the radio. Saved OFF starts with confirmed OFF state. */
esp_err_t ryz_provisioning_init(void);

/** On the first boot start, scan for the exact saved SSID before starting
 * STA. A missing SSID/scan error skips connect and permits the existing boot
 * AP fallback. Later explicit controls and browser submissions retain their
 * own connection policy. Otherwise start the captive portal if unconfigured.
 * No-op while disabled, including a restored persistent OFF preference. */
esp_err_t ryz_provisioning_start(void);

/** Settle the first enabled saved-STA boot attempt without changing persistent
 * ON/OFF, STA credentials, or the AP password. Owner only, after its boot wait:
 * preserve ONLINE/OFF/AP and any explicitly controlled session; otherwise
 * verify live STA association/IP under the network lock before choosing AP.
 * A live connection is synchronously published ONLINE even if GOT_IP is still
 * queued. Only confirmed shutdown can start the password-protected AP.
 * ESP_OK acknowledges settlement/no-op, not necessarily AP_READY (a browser
 * may already submit credentials). Read/stop/start errors are returned and
 * published as FAILED; no automatic retry. A later explicit control revokes
 * this boot-only operation, even when that control reports an error. */
esp_err_t ryz_provisioning_boot_fallback(void);

/** Trusted owner operation; may wait for platform service shutdown, never call
 * from UI, HTTP, or the ESP event task. OFF preserves STA/AP credentials. ON
 * retries any unconfirmed prior stop before selecting saved STA or default AP.
 * Repeating start() while disabled never overrides the explicit OFF intent.
 * Save/commit the explicit choice first. Storage failure leaves current radio
 * state unchanged and returns the error; its storage outcome can be unknown.
 * Radio failure after a successful save does not undo the saved choice.
 * Repeating a known saved choice skips only the NVS write, not radio control.
 * No periodic persistence retry occurs; the trusted caller requests retry. */
esp_err_t ryz_provisioning_set_enabled(bool enabled);

/** Open the captive portal without erasing or saving STA credentials. Trusted
 * control owner only: confirms old shutdown, reads saved identity, then starts
 * AP. A reported healthy AP_READY is idempotent. Errors stop this sequence;
 * failed shutdown cannot start AP and failed credential reads are not NONE.
 * This explicitly enables Wi-Fi for the current boot only. Success establishes
 * portal startup, not STA connectivity: a newer HTTP/STA event may already have
 * advanced the snapshot. A concurrent HTTP submit retains its existing policy.
 * Never call from UI, HTTP, or the ESP event task; competing controls fail fast. */
esp_err_t ryz_provisioning_open_portal(void);

/** Return one coherent copy of the latest state. */
esp_err_t ryz_provisioning_get_snapshot(ryz_provisioning_snapshot_t *out_snapshot);

/** Forget only Ryzobee STA credentials and reopen the captive portal for this
 * boot. Does not alter the independently saved ON/OFF preference. */
esp_err_t ryz_provisioning_request_reprovision(void);

#ifdef __cplusplus
}
#endif
