/*
 * DNS redirection follows Espressif's ESP-IDF captive_portal example, which is
 * offered under CC0-1.0. This adaptation bounds-checks one-question A queries
 * and keeps ownership local to the provisioning component.
 */

#include "ryz_captive_dns.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define RYZ_DNS_PORT 53
#define RYZ_DNS_PACKET_MAX 512
#define RYZ_DNS_HEADER_SIZE 12
#define RYZ_DNS_ANSWER_SIZE 16
#define RYZ_DNS_STOP_ACK_MS 1000U

struct ryz_captive_dns {
    int socket_fd;
    TaskHandle_t task;
    SemaphoreHandle_t stopped;
    esp_netif_t *ap_netif;
    /* Only the serial stop caller accesses these flags. The task may already
     * have deleted itself when stop is retried, so never notify it twice. */
    bool stop_requested;
    bool acknowledged;
};

static const char *TAG = "ryz_captive_dns";

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static size_t find_question_end(const uint8_t *query, size_t query_length)
{
    size_t cursor = RYZ_DNS_HEADER_SIZE;
    while (cursor < query_length) {
        uint8_t label_length = query[cursor++];
        if (label_length == 0) {
            return cursor + 4 <= query_length ? cursor + 4 : 0;
        }
        if (label_length > 63 || cursor + label_length > query_length) {
            return 0;
        }
        cursor += label_length;
    }
    return 0;
}

static size_t build_reply(
    esp_netif_t *ap_netif,
    const uint8_t *query,
    size_t query_length,
    uint8_t *reply,
    size_t reply_capacity)
{
    if (query == NULL || reply == NULL || query_length < RYZ_DNS_HEADER_SIZE ||
        query_length > reply_capacity || read_u16(query + 4) != 1) {
        return 0;
    }

    uint16_t flags = read_u16(query + 2);
    if ((flags & 0x7800U) != 0 || (flags & 0x8000U) != 0) {
        return 0;
    }

    size_t question_end = find_question_end(query, query_length);
    if (question_end == 0) {
        return 0;
    }

    size_t question_type_offset = question_end - 4;
    bool is_ipv4_question = read_u16(query + question_type_offset) == 1 &&
                            read_u16(query + question_type_offset + 2) == 1;
    size_t reply_length = question_end + (is_ipv4_question ? RYZ_DNS_ANSWER_SIZE : 0);
    if (reply_length > reply_capacity) {
        return 0;
    }

    memcpy(reply, query, question_end);
    write_u16(reply + 2, 0x8180U);
    write_u16(reply + 6, is_ipv4_question ? 1 : 0);
    write_u16(reply + 8, 0);
    write_u16(reply + 10, 0);

    if (!is_ipv4_question) {
        return question_end;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(ap_netif, &ip_info) != ESP_OK) {
        return 0;
    }

    uint8_t *answer = reply + question_end;
    write_u16(answer, 0xC00CU);
    write_u16(answer + 2, 1);
    write_u16(answer + 4, 1);
    write_u32(answer + 6, 30);
    write_u16(answer + 10, 4);
    memcpy(answer + 12, &ip_info.ip.addr, 4);
    return reply_length;
}

static void dns_task(void *argument)
{
    struct ryz_captive_dns *server = argument;
    uint8_t query[RYZ_DNS_PACKET_MAX];
    uint8_t reply[RYZ_DNS_PACKET_MAX];
    bool stopping = false;

    while (!stopping) {
        if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            break;
        }
        struct sockaddr_storage source = {0};
        socklen_t source_length = sizeof(source);
        int received = recvfrom(
            server->socket_fd,
            query,
            sizeof(query),
            0,
            (struct sockaddr *)&source,
            &source_length);
        if (received < 0) {
            stopping = ulTaskNotifyTake(pdTRUE, 0) > 0;
            if (!stopping && errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "DNS receive failed: errno=%d", errno);
            }
            continue;
        }

        size_t reply_length = build_reply(
            server->ap_netif,
            query,
            (size_t)received,
            reply,
            sizeof(reply));
        if (reply_length == 0) {
            continue;
        }
        int sent = sendto(
            server->socket_fd,
            reply,
            reply_length,
            0,
            (struct sockaddr *)&source,
            source_length);
        stopping = ulTaskNotifyTake(pdTRUE, 0) > 0;
        if (sent < 0 && !stopping) {
            ESP_LOGW(TAG, "DNS send failed: errno=%d", errno);
        }
    }
    SemaphoreHandle_t stopped = server->stopped;
    xSemaphoreGive(stopped);
    vTaskDelete(NULL);
}

static esp_err_t cleanup_failed_start(
    struct ryz_captive_dns *server,
    ryz_captive_dns_handle_t *out_handle,
    esp_err_t error)
{
    /* No task was successfully created. Keep an unclosed socket reachable
     * even when initialization failed before bind or task creation. */
    server->stop_requested = true;
    server->acknowledged = true;
    server->task = NULL;
    if (ryz_captive_dns_stop(server) != ESP_OK) {
        *out_handle = server;
    }
    return error;
}

esp_err_t ryz_captive_dns_start(
    esp_netif_t *ap_netif,
    ryz_captive_dns_handle_t *out_handle)
{
    if (ap_netif == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    struct ryz_captive_dns *server = calloc(1, sizeof(*server));
    if (server == NULL) {
        return ESP_ERR_NO_MEM;
    }

    server->stopped = xSemaphoreCreateBinary();
    if (server->stopped == NULL) {
        free(server);
        return ESP_ERR_NO_MEM;
    }

    server->socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (server->socket_fd < 0) {
        vSemaphoreDelete(server->stopped);
        free(server);
        return ESP_FAIL;
    }

    struct timeval receive_timeout = {
        .tv_sec = 0,
        .tv_usec = 250000,
    };
    if (setsockopt(
            server->socket_fd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &receive_timeout,
            sizeof(receive_timeout)) < 0) {
        return cleanup_failed_start(server, out_handle, ESP_FAIL);
    }
    if (setsockopt(
            server->socket_fd,
            SOL_SOCKET,
            SO_SNDTIMEO,
            &receive_timeout,
            sizeof(receive_timeout)) < 0) {
        return cleanup_failed_start(server, out_handle, ESP_FAIL);
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(RYZ_DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(server->socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        return cleanup_failed_start(server, out_handle, ESP_FAIL);
    }

    server->ap_netif = ap_netif;
    if (xTaskCreate(dns_task, "ryz_dns", 3072, server, 4, &server->task) != pdPASS) {
        return cleanup_failed_start(server, out_handle, ESP_ERR_NO_MEM);
    }

    *out_handle = server;
    return ESP_OK;
}

esp_err_t ryz_captive_dns_stop(ryz_captive_dns_handle_t handle)
{
    if (handle == NULL) {
        return ESP_OK;
    }
    if (!handle->acknowledged && !handle->stop_requested &&
        xTaskGetCurrentTaskHandle() == handle->task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!handle->stop_requested) {
        handle->stop_requested = true;
        xTaskNotifyGive(handle->task);
        /* IDF 5.5.4 UDP shutdown returns EOPNOTSUPP; stopping relies on the
         * notification plus receive timeout, not on this best-effort call.
         * A failed shutdown never justifies freeing a still-running task. */
        shutdown(handle->socket_fd, SHUT_RDWR);
    }
    if (!handle->acknowledged) {
        if (xSemaphoreTake(handle->stopped,
                           pdMS_TO_TICKS(RYZ_DNS_STOP_ACK_MS)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        handle->acknowledged = true;
        handle->task = NULL;
    }
    /* IDF 5.5.4 lwip_close leaves the socket allocated when
     * netconn_prepare_delete fails; the permanently registered socket FD
     * range also survives a VFS close error. Retain it for a serial retry. */
    if (close(handle->socket_fd) != 0) {
        return ESP_FAIL;
    }
    vSemaphoreDelete(handle->stopped);
    free(handle);
    return ESP_OK;
}
