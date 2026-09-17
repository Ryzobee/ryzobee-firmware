#include "ryz_net_traffic.h"

#include <string.h>
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"

#ifdef ESP_PLATFORM
#include "esp_idf_version.h"
#include "sdkconfig.h"
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 4) || !CONFIG_ESP_NETIF_TCPIP_LWIP
#error "Re-audit ESP-NETIF traffic call/relocation coverage for this SDK or stack"
#endif
#endif

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uintptr_t s_sta_key;
static uint8_t s_bssid[6];
static ryz_net_traffic_snapshot_t s_snapshot;
static esp_err_t s_error;

typedef struct {
    uintptr_t netif_key;
    uint32_t epoch;
} traffic_ticket_t;

_Static_assert(sizeof(size_t) <= sizeof(uint64_t), "traffic length must fit uint64");

esp_err_t __real_esp_netif_transmit(esp_netif_t *, void *, size_t);
esp_err_t __real_esp_netif_transmit_wrap(esp_netif_t *, void *, size_t, void *);
esp_err_t __real_esp_netif_receive(esp_netif_t *, void *, size_t, void *);

esp_err_t ryz_net_traffic_observe(esp_netif_t *sta, const uint8_t bssid[6],
                                ryz_net_traffic_snapshot_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!sta || !bssid || !out) return ESP_ERR_INVALID_ARG;
    uintptr_t key = (uintptr_t)sta;
    portENTER_CRITICAL(&s_mux);
    if (s_error != ESP_OK) {
        esp_err_t error = s_error;
        portEXIT_CRITICAL(&s_mux);
        return error;
    }
    if (!s_snapshot.valid || s_sta_key != key || memcmp(s_bssid, bssid, 6) != 0) {
        if (s_snapshot.epoch == UINT32_MAX) {
            s_snapshot.valid = false;
            s_error = ESP_ERR_INVALID_STATE;
            portEXIT_CRITICAL(&s_mux);
            return ESP_ERR_INVALID_STATE;
        }
        s_sta_key = key;
        memcpy(s_bssid, bssid, 6);
        ++s_snapshot.epoch;
        s_snapshot.valid = true;
        s_snapshot.rx_bytes = 0;
        s_snapshot.tx_bytes = 0;
    }
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

void ryz_net_traffic_revoke(void)
{
    portENTER_CRITICAL(&s_mux);
    s_sta_key = 0;
    memset(s_bssid, 0, sizeof(s_bssid));
    s_snapshot.valid = false;
    s_snapshot.rx_bytes = 0;
    s_snapshot.tx_bytes = 0;
    s_error = ESP_OK;
    portEXIT_CRITICAL(&s_mux);
}

static traffic_ticket_t capture_epoch(esp_netif_t *netif)
{
    traffic_ticket_t ticket = {.netif_key = (uintptr_t)netif};
    portENTER_CRITICAL(&s_mux);
    if (s_snapshot.valid && s_sta_key == ticket.netif_key)
        ticket.epoch = s_snapshot.epoch;
    portEXIT_CRITICAL(&s_mux);
    return ticket;
}

static void record_bytes(traffic_ticket_t ticket, size_t length, bool tx)
{
    if (!ticket.epoch) return;
    portENTER_CRITICAL(&s_mux);
    if (s_snapshot.valid && s_sta_key == ticket.netif_key &&
        s_snapshot.epoch == ticket.epoch) {
        uint64_t *counter = tx ? &s_snapshot.tx_bytes : &s_snapshot.rx_bytes;
        if ((uint64_t)length > UINT64_MAX - *counter) {
            s_snapshot.valid = false;
            s_error = ESP_ERR_INVALID_SIZE;
        } else {
            *counter += (uint64_t)length;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t __wrap_esp_netif_transmit(esp_netif_t *netif, void *data, size_t length)
{
    traffic_ticket_t ticket = capture_epoch(netif);
    esp_err_t error = __real_esp_netif_transmit(netif, data, length);
    if (error == ESP_OK) record_bytes(ticket, length, true);
    return error;
}

esp_err_t __wrap_esp_netif_transmit_wrap(esp_netif_t *netif, void *data,
                                      size_t length, void *pbuf)
{
    traffic_ticket_t ticket = capture_epoch(netif);
    esp_err_t error = __real_esp_netif_transmit_wrap(netif, data, length, pbuf);
    if (error == ESP_OK) record_bytes(ticket, length, true);
    return error;
}

esp_err_t __wrap_esp_netif_receive(esp_netif_t *netif, void *data,
                                size_t length, void *buffer)
{
    traffic_ticket_t ticket = capture_epoch(netif);
    esp_err_t error = __real_esp_netif_receive(netif, data, length, buffer);
    /* Neither RX buffer nor the opaque handle is inspected after __real: the
     * SDK may already have consumed buffers. The ticket is only an identity
     * value, not a retained resource or permission to prolong netif lifetime. */
    if (error == ESP_OK) record_bytes(ticket, length, false);
    return error;
}
