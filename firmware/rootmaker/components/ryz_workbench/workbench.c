/* Application-owned Console, script jobs and file transport. No Lua VM patches. */
#include "workbench.h"
#include "workbench_lifecycle.h"
#include "workbench_boot.h"
#include "workbench_ble.h"
#include "workbench_boot_key.h"
#include "workbench_fault.h"
#include "ryz_display_settings.h"
#include "ryz_v5_display.h"
#include "workbench_io.h"
#include "workbench_input.h"
#include "workbench_file_rpc.h"
#include "workbench_i2c_rpc.h"
#include "workbench_rgb_rpc.h"
#include "workbench_monitor_rpc.h"
#include "workbench_job_start.h"
#include "workbench_tools.h"
#include "workbench_network_rpc.h"
#include "workbench_apps.h"
#include "workbench_scripts.h"
#include "workbench_telemetry.h"
#include "workbench_network.h"
#include "workbench_write_guard.h"
#include "workbench_restart.h"
#include "workbench_version.h"
#include "app_runtime.h"
#include "display.h"
#include "boot_button.h"
#include "lua_runtime.h"
#include "ryz_runtime_io.h"
#include "ryz_lvgl.h"
#include "nervous_runtime.h"
#include "ryz_ota.h"
#include "ryz_apps.h"
#include "ryz_sensors.h"
#include "ryz_font.h"
#include "ryz_provisioning.h"
#include "ryz_provisioning_local_secret.h"
#include "ryz_script_store.h"
#include "ryz_system_services.h"
#include "ryz_system_ui.h"
#include "ryz_v5_boot.h"
#include "ryz_v5_apps.h"
#include "ryz_v5_status.h"
#include "ryz_v5_render_dirty.h"
#include "ryz_v5_password.h"
#include "ryz_time.h"
#include "touch.h"
#include "touch_demo.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "argtable3/argtable3.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lua.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

#define RX_MAX (24 * 1024)
#define LOG_BYTES RYZ_WORKBENCH_JOB_LOG_BYTES
#define LOG_CHUNK 128
#define SCRIPT_ROOT "/scripts"
#define SYSTEM_UI_TOUCH_INTERVAL_US 20000
#define SYSTEM_UI_TOUCH_RETRY_US 500000
#define SYSTEM_UI_BOOT_BUTTON_INTERVAL_US 20000
#define SYSTEM_UI_NETWORK_INTERVAL_US 200000
#define SYSTEM_UI_STORAGE_INTERVAL_US 1000000
#define SYSTEM_UI_ERROR_LOG_INTERVAL_US 5000000
/* The old 40 KiB budget included FreeType's 16,672-byte cold-glyph frame.
 * Static-font firmware has no such frame: audited deep native touch/render
 * frames total about 17 KiB. Keep 24 KiB plus on-device watermark reporting;
 * opt-in Lua FreeType retains the original 40 KiB budget. This does not change
 * Lua worker, BLE, Wi-Fi, UART or DMA stack/buffer ownership. */
#if CONFIG_RYZ_LUA_FREETYPE
#define SYSTEM_UI_TASK_STACK_BYTES (40 * 1024)
#else
#define SYSTEM_UI_TASK_STACK_BYTES (24 * 1024)
#endif

/* The early UI task uses only these atomics + the LCD until setup publication.
 * Release/acquire publishes every ordinary workbench field/queue as one unit. */
static atomic_int boot_first_frame = ATOMIC_VAR_INIT(ESP_ERR_NOT_FINISHED);
static atomic_bool boot_setup_complete = ATOMIC_VAR_INIT(false);
static bool boot_animation_active; /* UI Owner only */
static bool boot_timeout_reported;
static int64_t boot_animation_started_us;

static void pump_boot_screen(void)
{
    (void)ryz_display_settings_owner_tick((uint64_t)esp_timer_get_time()/1000U,false,true);
    if (!boot_animation_active) return;
    const int64_t elapsed = esp_timer_get_time() - boot_animation_started_us;
    esp_err_t error;
    if (elapsed >= INT64_C(15000000) && !boot_timeout_reported) {
        boot_timeout_reported = true;
        ESP_LOGE("ryzobee", "startup pending after 15 s; keeping boot screen, runtime not ready");
        error = ryz_v5_boot_timeout();
    } else {
        error = ryz_v5_boot_tick((uint32_t)(elapsed / 1000));
    }
    if (error != ESP_OK) {
        ESP_LOGE("ryzobee", "boot display update failed: %s", esp_err_to_name(error));
        /* Release synchronously on the same Owner; later normal rendering can
         * recover. Never introduce another task to write the physical panel. */
        ESP_ERROR_CHECK(ryz_v5_boot_end());
        boot_animation_active = false;
    }
}

_Static_assert(RYZ_LUA_SOURCE_MAX == RYZ_SCRIPT_STORE_SOURCE_MAX,
               "Store and runtime source budgets must match");
_Static_assert(RYZ_LUA_SOURCE_MAX == RYZ_SCRIPT_METADATA_SOURCE_MAX,
               "Metadata and runtime source budgets must match");

_Static_assert(RYZ_PROVISIONING_SSID_MAX_LENGTH ==
                   RYZ_SYSTEM_UI_SSID_MAX_LENGTH,
               "Provisioning and system UI SSID limits must match");
_Static_assert(RYZ_PROVISIONING_PASSWORD_MAX_LENGTH ==
                   RYZ_SYSTEM_UI_PASSWORD_MAX_LENGTH,
               "Provisioning and system UI password limits must match");
_Static_assert(RYZ_PROVISIONING_IPV4_MAX_LENGTH ==
                   RYZ_SYSTEM_UI_IPV4_MAX_LENGTH,
               "Provisioning and system UI IPv4 limits must match");

typedef struct { char *text; const char *prefix; } packet_t;
typedef struct {
    char job_id[32];
    uint32_t sequence;
    size_t length;
    unsigned char bytes[LOG_CHUNK];
} output_t;

typedef enum {
    SYSTEM_SHELL_ERROR_SERVICES_START,
    SYSTEM_SHELL_ERROR_SERVICES_SNAPSHOT,
    SYSTEM_SHELL_ERROR_OTA_CONFIRM,
    SYSTEM_SHELL_ERROR_OTA_ACTION,
    SYSTEM_SHELL_ERROR_STORAGE_SNAPSHOT,
    SYSTEM_SHELL_ERROR_RENDER,
    SYSTEM_SHELL_ERROR_TOUCH_HANDLER,
    SYSTEM_SHELL_ERROR_REPROVISION,
    SYSTEM_SHELL_ERROR_APPS,
    SYSTEM_SHELL_ERROR_SENSORS_START,
    SYSTEM_SHELL_ERROR_BLE_START,
    SYSTEM_SHELL_ERROR_BLE_SNAPSHOT,
    SYSTEM_SHELL_ERROR_BLE_BOOT,
    SYSTEM_SHELL_ERROR_COUNT,
} system_shell_error_t;

typedef struct {
    int64_t next_log_us;
    uint32_t suppressed;
} system_shell_error_limit_t;

typedef struct {
    bool provisioning_ready;
    bool provisioning_started;
    bool time_ready;
    bool ota_ready;
    bool network_valid;
    ryz_workbench_boot_t boot_health;
    bool ui_active;
    bool render_dirty;
    uint32_t network_revision;
    uint32_t reprovision_completed;
    uint32_t ui_network_seen_operation;
    esp_err_t ui_network_admission_error;
    uint32_t ui_restart_attempt;
    esp_err_t ui_restart_error;
    int64_t next_services_start_attempt_us;
    esp_err_t ble_start_error;
    ryz_system_ui_snapshot_t snapshot;
    ryz_workbench_telemetry_t telemetry;
    esp_chip_info_t chip; /* Boot-local hardware identity; no per-frame SDK query. */
    system_shell_error_limit_t errors[SYSTEM_SHELL_ERROR_COUNT];
} system_shell_t;

static bool filesystem_ready, display_ready, touch_ready, font_ready;
static bool boot_button_ready;
static bool transport_ready;
static char boot_id[24], device_id[65];
static unsigned next_job;
static QueueHandle_t control_queue, output_queue, job_queue;
static QueueHandle_t worker_queue, completion_queue;
static SemaphoreHandle_t job_lock;
/* Same admission lock as every Job start; no filesystem lock is held here. */
static bool file_write_active; /* Guarded by the same lock as final Job admission. */
static const ryz_workbench_write_guard_t file_write_guard;
static TaskHandle_t transmitter, ui_owner, lua_worker;
static atomic_bool runtime_gateway_ready;
static ryz_workbench_io_t runtime_channel;
static workbench_input_t runtime_input;
/* Diagnostics copied under job_lock; input state itself is UI-owner-only. */
static uint64_t ui_owner_cycles;
static uint32_t ui_input_dropped;
/* Owner writes, read-only info observes individual atomic counters. Durations
 * measure software calls, not physical finger-to-photon latency. No logs,
 * coordinates, secrets or allocations are added to the hot path. */
typedef struct {
    _Atomic uint32_t calls;
    _Atomic uint32_t last_us;
    _Atomic uint32_t max_us;
} ui_call_timing_t;
static ui_call_timing_t ui_touch_read_timing, ui_touch_handle_timing;
static ui_call_timing_t ui_render_timing, ui_tick_timing;

static void ui_record_timing(ui_call_timing_t *timing, int64_t started_us)
{
    const int64_t elapsed = esp_timer_get_time() - started_us;
    const uint32_t duration = elapsed <= 0 ? 0 :
        (uint64_t)elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    atomic_store_explicit(&timing->last_us, duration, memory_order_relaxed);
    if (duration > atomic_load_explicit(&timing->max_us, memory_order_relaxed))
        atomic_store_explicit(&timing->max_us, duration, memory_order_relaxed);
    atomic_fetch_add_explicit(&timing->calls, 1, memory_order_relaxed);
}
static script_job_t *current, *recent;
/* Prepared by the startup/RX owner before boot_setup_complete is published,
 * then consumed exactly once by the UI Owner. A valid boot.lua is the
 * persisted opt-in autostart contract; absence means normal HOME startup. */
static char *boot_autostart_source;
static ryz_touch_info_t touch_snapshot;
static bool demo_snapshot;
static const char *TAG = "ryz_workbench";
static const char *ACTIVE_OWNER_ERROR = "busy: runtime owner has an active job";
static const char *console_id;
static cJSON *console_reply;
static void launch_system_app(void *unused, const char *name, char *source,
                              ryz_workbench_apps_launch_result_t *result);
static cJSON *start_job(const char *id, const char *name, char *source,
                        uint32_t timeout, bool legacy);
static struct {
    struct arg_lit *run, *jobs;
    struct arg_str *path, *job, *stop;
    struct arg_end *end;
} lua_args;

static cJSON *response(const char *id, bool ok, const char *error)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) return NULL;
    cJSON_AddStringToObject(r, "id", id ? id : "");
    cJSON_AddBoolToObject(r, "ok", ok);
    if (error) cJSON_AddStringToObject(r, "error", error);
    return r;
}

static void boot_emit(cJSON *reply)
{
    char *text = reply ? cJSON_PrintUnformatted(reply) : NULL;
    printf("\nRYZOBEE_RPC %s\n",
           text ? text : "{\"id\":\"\",\"ok\":false,\"error\":\"response allocation failed\"}");
    fflush(stdout);
    free(text);
    cJSON_Delete(reply);
}

static const char *field(cJSON *r, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(r, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void send_json(const char *prefix, cJSON *r)
{
    packet_t packet = { .text = r ? cJSON_PrintUnformatted(r) : NULL, .prefix = prefix };
    cJSON_Delete(r);
    if (!packet.text) return;
    /* Bounded queue: a broken/flooding peer cannot exhaust memory or block Lua. */
    if (xQueueSend(control_queue, &packet, pdMS_TO_TICKS(20)) != pdTRUE) free(packet.text);
    else xTaskNotifyGive(transmitter);
}

static void add_base64(cJSON *r, const char *key, const void *bytes, size_t length)
{
    size_t size = 4 * ((length + 2) / 3) + 1, written = 0;
    unsigned char *encoded = malloc(size);
    if (!encoded) return;
    if (mbedtls_base64_encode(encoded, size, &written, bytes, length) == 0) {
        encoded[written] = 0;
        cJSON_AddStringToObject(r, key, (const char *)encoded);
    }
    free(encoded);
}

static void write_packet(const char *prefix, const char *text)
{
    /* Keep a complete frame inside one stdio lock, including driver logging. */
    flockfile(stdout);
    printf("\n%s%s\n", prefix, text);
    fflush(stdout);
    funlockfile(stdout);
}

static void transmit_task(void *unused)
{
    (void)unused;
    packet_t packet;
    output_t *chunk = malloc(sizeof(*chunk));
    configASSERT(chunk);
    for (;;) {
        if (xQueueReceive(control_queue, &packet, 0) == pdTRUE) {
            write_packet(packet.prefix, packet.text);
            free(packet.text);
        } else if (xQueueReceive(output_queue, chunk, 0) == pdTRUE) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "event", "output");
            cJSON_AddStringToObject(r, "boot_id", boot_id);
            cJSON_AddStringToObject(r, "job_id", chunk->job_id);
            cJSON_AddNumberToObject(r, "seq", chunk->sequence);
            add_base64(r, "data_b64", chunk->bytes, chunk->length);
            char *text = cJSON_PrintUnformatted(r);
            if (text) { write_packet("RYZOBEE_EVENT ", text); free(text); }
            cJSON_Delete(r);
            /* stdout can busy-wait on UART. Let the receiver process a stop
             * even while the bounded output queue remains permanently full. */
            vTaskDelay(1);
        } else ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
}

static void digest(const char *bytes, size_t length, char hex[65])
{
    unsigned char sum[32];
    if (mbedtls_sha256((const unsigned char *)bytes, length, sum, 0)) { hex[0] = 0; return; }
    for (size_t i = 0; i < sizeof(sum); ++i) snprintf(hex + 2 * i, 3, "%02x", sum[i]);
}

static char *read_script(const char *name)
{
    if (!filesystem_ready) return NULL;
    ryz_script_store_status_t status;
    if (ryz_script_store_status(&status) != ESP_OK || !status.ready ||
        status.recovery_required) return NULL;
    ryz_script_store_snapshot_t snapshot;
    if (ryz_script_store_get(name, &snapshot) != ESP_OK) return NULL;
    /* Diagnostic downloads remain available during recovery, but execution
     * never adopts a snapshot while a transaction outcome is unresolved. */
    if (ryz_script_store_status(&status) != ESP_OK || status.recovery_required) {
        ryz_script_store_snapshot_free(&snapshot);
        return NULL;
    }
    /* Keep an independent job-owned snapshot; immutable source is CPU-only
     * and must not consume the internal RAM reserved for task/driver state. */
    char *source = ryz_workbench_source_copy(snapshot.source, snapshot.entry.bytes);
    ryz_script_store_snapshot_free(&snapshot);
    return source;
}

static bool busy(void)
{
    xSemaphoreTake(job_lock, portMAX_DELAY);
    bool active = current != NULL;
    xSemaphoreGive(job_lock);
    return active;
}

static void refresh_touch(void)
{
    ryz_touch_info_t snapshot;
    ryz_touch_get_info(&snapshot);
    bool enabled = ryz_touch_demo_enabled();
    xSemaphoreTake(job_lock, portMAX_DELAY);
    touch_snapshot = snapshot;
    demo_snapshot = enabled;
    xSemaphoreGive(job_lock);
}

static void copy_network_field(char *destination, size_t destination_size,
                               const char *source)
{
    if (destination_size == 0) return;
    size_t length = source ? strnlen(source, destination_size - 1) : 0;
    if (length) memcpy(destination, source, length);
    destination[length] = '\0';
}

static const char *system_shell_error_name(system_shell_error_t error)
{
    switch (error) {
    case SYSTEM_SHELL_ERROR_SERVICES_START:
        return "system services start";
    case SYSTEM_SHELL_ERROR_SERVICES_SNAPSHOT:
        return "system services snapshot";
    case SYSTEM_SHELL_ERROR_OTA_CONFIRM:
        return "OTA image confirmation";
    case SYSTEM_SHELL_ERROR_OTA_ACTION:
        return "OTA action";
    case SYSTEM_SHELL_ERROR_STORAGE_SNAPSHOT:
        return "storage snapshot";
    case SYSTEM_SHELL_ERROR_RENDER:
        return "system UI render";
    case SYSTEM_SHELL_ERROR_TOUCH_HANDLER:
        return "system UI touch";
    case SYSTEM_SHELL_ERROR_REPROVISION:
        return "network reprovision";
    case SYSTEM_SHELL_ERROR_APPS:
        return "application catalog";
    case SYSTEM_SHELL_ERROR_SENSORS_START:
        return "sensor worker startup";
    case SYSTEM_SHELL_ERROR_BLE_START:
        return "BLE service start";
    case SYSTEM_SHELL_ERROR_BLE_SNAPSHOT:
        return "BLE service snapshot";
    case SYSTEM_SHELL_ERROR_BLE_BOOT:
        return "BLE boot restore";
    default:
        return "system shell";
    }
}

static void report_system_shell_error(system_shell_t *shell,
                                      system_shell_error_t error,
                                      esp_err_t status)
{
    /* Log only a fixed operation label and status. The network view contains
     * the AP password and must never be formatted into diagnostics. */
    if (error >= SYSTEM_SHELL_ERROR_COUNT) return;
    system_shell_error_limit_t *limit = &shell->errors[error];
    int64_t now = esp_timer_get_time();
    if (now < limit->next_log_us) {
        if (limit->suppressed != UINT32_MAX) ++limit->suppressed;
        return;
    }
    if (limit->suppressed) {
        ESP_LOGW(TAG, "%s failed: %s (0x%x); %lu repeats suppressed",
                 system_shell_error_name(error), esp_err_to_name(status),
                 (unsigned int)status, (unsigned long)limit->suppressed);
    } else {
        ESP_LOGW(TAG, "%s failed: %s (0x%x)",
                 system_shell_error_name(error), esp_err_to_name(status),
                 (unsigned int)status);
    }
    limit->suppressed = 0;
    limit->next_log_us = now + SYSTEM_UI_ERROR_LOG_INTERVAL_US;
}

static ryz_system_ui_network_state_t system_ui_network_state(
    ryz_provisioning_state_t state)
{
    switch (state) {
    case RYZ_PROVISIONING_UNCONFIGURED:
        return RYZ_SYSTEM_UI_NETWORK_UNCONFIGURED;
    case RYZ_PROVISIONING_AP_STARTING:
        return RYZ_SYSTEM_UI_NETWORK_AP_STARTING;
    case RYZ_PROVISIONING_AP_READY:
        return RYZ_SYSTEM_UI_NETWORK_AP_READY;
    case RYZ_PROVISIONING_CREDENTIALS_RECEIVED:
        return RYZ_SYSTEM_UI_NETWORK_CREDENTIALS_RECEIVED;
    case RYZ_PROVISIONING_CONNECTING:
        return RYZ_SYSTEM_UI_NETWORK_CONNECTING;
    case RYZ_PROVISIONING_ONLINE:
        return RYZ_SYSTEM_UI_NETWORK_ONLINE;
    case RYZ_PROVISIONING_STOPPING:
        return RYZ_SYSTEM_UI_NETWORK_STOPPING;
    case RYZ_PROVISIONING_OFF:
        return RYZ_SYSTEM_UI_NETWORK_OFF;
    case RYZ_PROVISIONING_FAILED:
    default:
        return RYZ_SYSTEM_UI_NETWORK_FAILED;
    }
}

static void map_network_snapshot(
    const ryz_provisioning_snapshot_t *source,
    ryz_system_ui_snapshot_t *destination)
{
    destination->configured = source->credentials_stored;
    destination->connected = source->state == RYZ_PROVISIONING_ONLINE;
    /* Retained handles after a failed stop do not prove a reachable portal. */
    destination->ap_active = source->enabled && source->portal_active &&
        source->state == RYZ_PROVISIONING_AP_READY && source->portal_ip[0];
    destination->state = system_ui_network_state(source->state);
    memset(destination->ap_ssid, 0, sizeof(destination->ap_ssid));
    memset(destination->ap_password, 0, sizeof(destination->ap_password));
    memset(destination->portal_ip, 0, sizeof(destination->portal_ip));
    copy_network_field(destination->ap_ssid,
                       sizeof(destination->ap_ssid), source->ap_ssid);
    copy_network_field(destination->ap_password,
                       sizeof(destination->ap_password), source->ap_password);
    if (destination->ap_active) {
        copy_network_field(destination->portal_ip,
                           sizeof(destination->portal_ip),
                           source->portal_ip);
    }
}

static bool refresh_system_ble(system_shell_t *shell)
{
    ryz_ble_snapshot_t snapshot = {0};
    esp_err_t error = shell->ble_start_error;
    if (error == ESP_OK) error = ryz_ble_get_snapshot(&snapshot);
    if (error != ESP_OK)
        report_system_shell_error(shell, SYSTEM_SHELL_ERROR_BLE_SNAPSHOT, error);
    ryz_v5_status_t view;
    ryz_v5_status_get(&view);
    ryz_workbench_ble_model(&snapshot, error, &view.ble);
    const bool changed = ryz_v5_status_publish_visible(ryz_system_ui_current_route(), &view);
    if (changed) shell->render_dirty = true;
    return changed;
}

static void refresh_home_telemetry(system_shell_t *shell,
    const ryz_system_services_snapshot_t *services, ryz_v5_network_model_t *network)
{
    const uint64_t now_us=(uint64_t)esp_timer_get_time();
    (void)ryz_workbench_telemetry_select(&shell->telemetry,
        ryz_system_ui_home_selection(), &shell->snapshot);
    ryz_workbench_telemetry_update(&shell->telemetry, services, now_us, NULL,
        &shell->snapshot, network);
    ryz_workbench_telemetry_psram(&shell->telemetry, now_us,
        esp_psram_get_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        &shell->snapshot);
    network->system=shell->snapshot;
}

static bool refresh_system_network(system_shell_t *shell, bool force)
{
    /* BLE owns a separate service and must remain observable even if Wi-Fi,
     * OTA or time-owner startup failed. No radio wait or navigation here. */
    const bool ble_changed = refresh_system_ble(shell);
    ryz_system_services_snapshot_t services;
    esp_err_t error = ryz_system_services_get_snapshot(&services);
    if (error != ESP_OK) {
        /* Only OS task creation is retried here. Dependency initialization,
         * NTP retries and network/OTA propagation belong to the C owner. */
        const int64_t now = esp_timer_get_time();
        if (now >= shell->next_services_start_attempt_us) {
            esp_err_t start_error = ryz_system_services_start();
            shell->next_services_start_attempt_us =
                now + SYSTEM_UI_ERROR_LOG_INTERVAL_US;
            if (start_error != ESP_OK) {
                report_system_shell_error(
                    shell, SYSTEM_SHELL_ERROR_SERVICES_START, start_error);
            }
        }
        report_system_shell_error(shell,
                                  SYSTEM_SHELL_ERROR_SERVICES_SNAPSHOT, error);
        /* HOME selection and heap observations are independent of the service
         * task. NULL revokes NET/TEMP/CPU instead of reading the failed output;
         * the selected PSRAM recording can still start and remain observable. */
        const ryz_system_ui_snapshot_t previous=shell->snapshot;
        ryz_v5_status_t view;
        ryz_v5_status_get(&view);
        refresh_home_telemetry(shell,NULL,&view.network);
        const ryz_system_ui_route_t route=ryz_system_ui_current_route();
        const bool changed=ryz_v5_status_publish_visible(route,&view) || ble_changed ||
            ryz_v5_system_visible_changed(route,&previous,&shell->snapshot);
        if(changed) shell->render_dirty=true;
        return changed;
    }
    shell->provisioning_ready = services.provisioning_ready;
    shell->provisioning_started = services.provisioning_started;
    shell->time_ready = services.time_ready;
    shell->ota_ready = services.ota_ready;
    if (services.reprovision_completed != shell->reprovision_completed) {
        shell->reprovision_completed = services.reprovision_completed;
        if (services.reprovision_result != ESP_OK) {
            report_system_shell_error(shell, SYSTEM_SHELL_ERROR_REPROVISION,
                                      services.reprovision_result);
        }
    }
    const ryz_system_ui_snapshot_t previous = shell->snapshot;
    /* Link samples also advance provisioning revision. Compare mapped values
     * and validity below, not raw sample timestamps. The shared view still
     * includes other routes (for example Version uptime), so this is not a
     * current-route-only repaint gate. */
    const bool network_changed = force || services.network_valid != shell->network_valid;
    /* revision means mapped, not painted; render_dirty is cleared only after
     * the display driver confirms the complete system UI render. */
    shell->network_revision = services.network.revision;
    shell->network_valid = services.network_valid;
    map_network_snapshot(&services.network, &shell->snapshot);
    shell->snapshot.time_hhmm[0]='\0';
    if(services.time_valid && services.time.clock_valid) {
        const time_t now=time(NULL);
        struct tm local;
        /* Use the configured device timezone (currently UTC0), never infer
         * the product timezone from the developer's computer. */
        if(localtime_r(&now,&local))
            (void)strftime(shell->snapshot.time_hhmm,sizeof(shell->snapshot.time_hhmm),"%H:%M",&local);
    }
    const esp_app_desc_t *app=esp_app_get_description();
    copy_network_field(shell->snapshot.firmware_version,sizeof(shell->snapshot.firmware_version),app->version);
    ryz_v5_status_t view;
    ryz_v5_status_get(&view);
    refresh_home_telemetry(shell,&services,&view.network);
    ryz_workbench_network_controls(&services, &view.network);
    if(shell->ui_network_admission_error!=ESP_OK &&
       shell->ui_network_seen_operation==services.network_operation.requested) {
        view.network.operation_error=shell->ui_network_admission_error;
        copy_network_field(view.network.operation_label,sizeof(view.network.operation_label),"REQUEST FAILED");
    }
    copy_network_field(view.network.sta_ssid,sizeof(view.network.sta_ssid),services.network.sta_ssid);
    copy_network_field(view.network.ipv4,sizeof(view.network.ipv4),services.network.ipv4);
    copy_network_field(view.version.running,sizeof(view.version.running),app->version);
    copy_network_field(view.version.idf,sizeof(view.version.idf),app->idf_ver);
    snprintf(view.version.build,sizeof(view.version.build),"%.11s %.8s",app->date,app->time);
    uint64_t uptime=(uint64_t)esp_timer_get_time()/1000000;
    snprintf(view.version.uptime,sizeof(view.version.uptime),"%llu:%02u:%02u",
        (unsigned long long)(uptime/3600),(unsigned)(uptime/60%60),(unsigned)(uptime%60));
    copy_network_field(view.version.device_id,sizeof(view.version.device_id),device_id);
    ryz_ota_snapshot_t ota;
    /* A failed snapshot must not leave a stale enabled restart action. */
    view.version.image_staged=false;
    view.version.restart_ready=false;
    view.version.restart_pending=false;
    view.version.attempt_id=0;
    view.version.restart_error=ESP_OK;
    ryz_workbench_version_hardware(&view.version,NULL,
        shell->chip.model==CHIP_ESP32S3 && shell->chip.cores!=0,shell->chip.revision);
    if(ryz_ota_get_snapshot(&ota)==ESP_OK) {
        ryz_workbench_version_hardware(&view.version,&ota.running_info,
            shell->chip.model==CHIP_ESP32S3 && shell->chip.cores!=0,shell->chip.revision);
        view.version.wifi_ready=ota.network_ready;
        /* No pending confirmation may also mean an unknown OTA partition
         * state. Only the actual system boot gate proves health. */
        view.version.healthy_known=shell->boot_health.confirmed || ota.pending_confirmation;
        view.version.healthy=shell->boot_health.confirmed && !ota.pending_confirmation;
        view.version.downloaded_bytes=ota.downloaded_bytes;
        view.version.total_bytes=ota.total_bytes;
        view.version.verifying=ota.state==RYZ_OTA_VERIFYING;
        view.version.cancelling=ota.state==RYZ_OTA_CANCELLING;
        view.version.cleanup_pending=ota.worker_active;
        view.version.cancel_ready=ota.worker_active &&
            (ota.state==RYZ_OTA_STARTING || ota.state==RYZ_OTA_DOWNLOADING);
        view.version.attempt_id=ota.attempt_id;
        view.version.image_staged=ota.state==RYZ_OTA_READY_TO_REBOOT && ota.reboot_required;
        view.version.restart_pending=ota.reboot_pending;
        /* This is display eligibility only. The C restart operation atomically
         * rechecks this exact attempt, then reserves the real Job/file gate. */
        view.version.restart_ready=view.version.image_staged && ota.attempt_id &&
            !ota.reboot_pending && !ota.pending_confirmation && !ota.worker_active &&
            ota.cleanup_confirmed && ota.cleanup_error==ESP_OK;
        if(shell->ui_restart_attempt!=ota.attempt_id) {
            shell->ui_restart_attempt=ota.attempt_id;
            shell->ui_restart_error=ESP_OK;
        }
        view.version.restart_error=shell->ui_restart_error;
        view.version.error=ota.last_error;
        copy_network_field(view.version.candidate,sizeof(view.version.candidate),ota.candidate_version);
    }
    const ryz_system_ui_route_t route=ryz_system_ui_current_route();
    /* Always publish the entire service model. Only visible changes request
     * a repaint; Version uptime must not repaint Settings or Apps. */
    bool changed=ryz_v5_status_publish_visible(route,&view) || ble_changed || network_changed ||
        ryz_v5_system_visible_changed(route,&previous,&shell->snapshot);
    if(changed) shell->render_dirty=true;
    return changed;
}

static bool refresh_system_storage(system_shell_t *shell, bool force)
{
    bool ready = false;
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    if (filesystem_ready) {
        ryz_script_store_status_t status;
        esp_err_t error = ryz_script_store_status(&status);
        /* The Store publishes cached capacity after non-UI file operations.
         * A writer/GC holding its lock must never block this UI Owner. */
        if (error == ESP_ERR_TIMEOUT) return false;
        if (error == ESP_OK && status.ready && status.capacity_valid) {
            ready = true;
            total_bytes = status.total_bytes;
            used_bytes = status.used_bytes;
        } else {
            report_system_shell_error(
                shell, SYSTEM_SHELL_ERROR_STORAGE_SNAPSHOT,
                error != ESP_OK ? error : status.capacity_error != ESP_OK
                    ? status.capacity_error : ESP_ERR_INVALID_STATE);
        }
    }
    const bool changed =
        shell->snapshot.storage_ready != ready ||
        shell->snapshot.storage_total_bytes != total_bytes ||
        shell->snapshot.storage_used_bytes != used_bytes;
    if (!force && !changed) return false;
    shell->snapshot.storage_ready = ready;
    shell->snapshot.storage_total_bytes = total_bytes;
    shell->snapshot.storage_used_bytes = used_bytes;
    shell->render_dirty = true;
    return true;
}

static bool system_ui_render_has_pending_job(void *unused)
{
    (void)unused;
    xSemaphoreTake(job_lock, portMAX_DELAY);
    const bool pending = current != NULL;
    xSemaphoreGive(job_lock);
    return pending;
}

/* Return true only when a newly admitted job deliberately interrupted the
 * deferred HOME transfer. A driver timeout without that observation remains a
 * real render failure. */
static bool render_system_ui(system_shell_t *shell)
{
    if (!shell->ui_active || !shell->render_dirty) return false;
    ryz_workbench_render_guard_t guard = {
        .pending_job = system_ui_render_has_pending_job,
    };
    bool cancelled_out = false;
    const int64_t render_started_us = esp_timer_get_time();
    esp_err_t error = ryz_system_ui_render_checked(
        &shell->snapshot, ryz_workbench_cancel_render, &guard,
        &cancelled_out);
    ui_record_timing(&ui_render_timing, render_started_us);
    if (error == ESP_OK) {
        shell->render_dirty = false;
        return false;
    }
    if (error == ESP_ERR_TIMEOUT &&
        ryz_workbench_render_was_cancelled(&guard, cancelled_out)) {
        return true;
    }
    report_system_shell_error(shell, SYSTEM_SHELL_ERROR_RENDER, error);
    /* A failed show may leave a pressed target armed against pixels the user
     * never saw. Recover through a clean HOME render and a fresh gesture. */
    ryz_system_ui_reset();
    shell->render_dirty = true;
    return false;
}

static esp_err_t confirm_boot_image(void *unused)
{
    (void)unused;
    return ryz_ota_confirm_running_image_healthy();
}

static void confirm_running_image_if_healthy(system_shell_t *shell)
{
    ryz_workbench_boot_inputs_t inputs = {
        .font_ready = font_ready,
        .touch_ready = touch_ready,
        .ui_active = shell->ui_active,
        .render_dirty = shell->render_dirty,
    };
    if (!shell->boot_health.confirmed) {
        ryz_system_services_snapshot_t services;
        /* Task creation alone is not completed C initialization. Read the
         * current service milestone, not the UI's last rendered cache. OFF
         * and an unsynchronized clock are valid initialized states. */
        inputs.services_ready =
            ryz_system_services_get_snapshot(&services) == ESP_OK &&
            services.cycles != 0 && services.provisioning_started &&
            services.time_ready && services.ota_ready;
    }
    const ryz_workbench_boot_result_t result = ryz_workbench_boot_step(
        &shell->boot_health, &inputs, esp_timer_get_time(), confirm_boot_image, NULL);
    /* Publish only after confirmation returns, outside job_lock. No user Job
     * can steal the first successful system frame or defer confirmation by
     * occupying the display. Success stays latched throughout Lua execution. */
    atomic_store(&runtime_gateway_ready, result.runtime_ready);
    if (result.attempted) {
        if (result.error == ESP_OK) {
            ESP_LOGI(TAG, "running image health gate: passed");
        } else {
            report_system_shell_error(shell, SYSTEM_SHELL_ERROR_OTA_CONFIRM, result.error);
        }
    }
}

static void start_system_shell(system_shell_t *shell, bool defer_home)
{
    memset(shell, 0, sizeof(*shell));
    esp_chip_info(&shell->chip);
    shell->snapshot.state = RYZ_SYSTEM_UI_NETWORK_FAILED;
    esp_err_t error = ryz_system_services_start();
    if (error != ESP_OK) {
        shell->next_services_start_attempt_us =
            esp_timer_get_time() + SYSTEM_UI_ERROR_LOG_INTERVAL_US;
        report_system_shell_error(
            shell, SYSTEM_SHELL_ERROR_SERVICES_START, error);
    }
    const ryz_v5_ble_binding_t ble_binding = {.submit = ryz_workbench_ble_submit};
    ryz_v5_ble_bind_services(&ble_binding);
    /* Both APIs only create their respective owners. Wi-Fi and BLE restore
     * concurrently while this single display Owner continues the animation. */
    shell->ble_start_error = ryz_ble_start();
    if (shell->ble_start_error != ESP_OK)
        report_system_shell_error(shell, SYSTEM_SHELL_ERROR_BLE_START, shell->ble_start_error);
    /* IMU is opt-in per Lua job, not a boot worker. Wait for the one-shot
     * Wi-Fi/BLE restoration decisions while this same Owner keeps animating.
     * Saved OFF stays OFF; saved STA is scanned first, then gets a bounded
     * connect window only on an exact match, otherwise immediate AP fallback.
     * Offline/unsynced and reported failures are settled results, not reasons
     * to hide the recovery UI. This is not the stricter OTA health gate.
     * A stuck initializer keeps the boot page (static timeout after 15 s).
     * OTA health confirmation remains a separate, stricter success gate. */
    int64_t radios_settled_at_us = -1;
    for (;;) {
        ryz_system_services_snapshot_t services;
        const bool services_settled = error != ESP_OK ||
            (ryz_system_services_get_snapshot(&services) == ESP_OK && services.boot_network_settled);
        ryz_ble_snapshot_t ble = {0};
        const esp_err_t ble_error = shell->ble_start_error == ESP_OK
            ? ryz_ble_get_snapshot(&ble) : shell->ble_start_error;
        const bool ble_settled = ryz_workbench_ble_boot_settled(shell->ble_start_error, ble_error, &ble);
        if (boot_animation_active) {
            /* Read the provisioning copy directly: the service owner may be
             * inside a blocking SDK scan and has not published its next full
             * aggregate yet. This getter takes only the short snapshot lock. */
            ryz_provisioning_snapshot_t network;
            esp_err_t read = ryz_provisioning_get_snapshot(&network);
            if (read == ESP_OK || services_settled) {
                esp_err_t shown = ryz_v5_boot_network(read == ESP_OK ? &network : NULL);
                if (shown != ESP_OK) {
                    ESP_LOGE(TAG, "boot network footer failed: %s", esp_err_to_name(shown));
                    ESP_ERROR_CHECK(ryz_v5_boot_end());
                    boot_animation_active = false;
                }
            }
        }
        if (services_settled && ble_settled) {
            const int64_t now = esp_timer_get_time();
            if (radios_settled_at_us < 0) radios_settled_at_us = now;
            /* Give the real terminal result one readable beat before HOME
             * or autostart. Do not sleep the display Owner or invent percent. */
            if (boot_animation_active && now - radios_settled_at_us < INT64_C(800000)) {
                pump_boot_screen();
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (shell->ble_start_error == ESP_OK && ble.boot_error != ESP_OK)
                report_system_shell_error(shell, SYSTEM_SHELL_ERROR_BLE_BOOT, ble.boot_error);
            break;
        }
        pump_boot_screen();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    /* Bootstrap already loaded the tuple before LCD initialization. Start the
     * runtime persistence worker and publish its fresh readback here; applying
     * the same panel tuple is a no-op, so the splash never starts at 0 degrees
     * only to rotate after wireless restoration. Later NVS IO stays off Owner. */
    esp_err_t display_settings=ryz_display_settings_start();
    if(display_settings==ESP_OK) {
        for(;;) {
            (void)ryz_display_settings_owner_tick((uint64_t)esp_timer_get_time()/1000U,false,true);
            ryz_display_settings_snapshot_t state;
            if(ryz_display_settings_get_snapshot(&state)==ESP_OK &&
               state.phase!=RYZ_DISPLAY_SETTINGS_LOADING && state.phase!=RYZ_DISPLAY_SETTINGS_APPLYING) break;
            pump_boot_screen();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    } else ESP_LOGE(TAG,"display settings unavailable: %s",esp_err_to_name(display_settings));
    (void)refresh_system_network(shell, true);
    (void)refresh_system_storage(shell, true);
    (void)ryz_touch_demo_enable(false);
    /* No panel clear/fade: retained boot pixels stay until either HOME or the
     * selected Lua scene commits. Autostart initializes navigation state but
     * deliberately does not render the menu underneath the splash. */
    ESP_ERROR_CHECK(ryz_v5_boot_end());
    boot_animation_active = false;
    ryz_display_show_abort();
    ryz_system_ui_reset();
    shell->ui_active = display_ready;
    shell->render_dirty = !defer_home;
    if (!defer_home) (void)render_system_ui(shell);
    confirm_running_image_if_healthy(shell);
}

static esp_err_t suspend_system_ui(system_shell_t *shell)
{
    /* Revoke the reader before the Lua job can claim the screen. A storage
     * call already in flight may finish, but cannot publish into that view. */
    ryz_workbench_apps_owner_tick(false, (ryz_ui_nav_token_t){0}, NULL, NULL);
    ryz_workbench_scripts_owner_tick(false, (ryz_ui_nav_token_t){0}, RYZ_V5_SCRIPTS_SETTINGS);
    shell->ui_active = false;
    esp_err_t error=ryz_system_ui_reset_checked();
    shell->render_dirty = true;
    (void)ryz_touch_demo_enable(false);
    ryz_display_show_abort();
    return error;
}

static void resume_system_ui(system_shell_t *shell)
{
    (void)ryz_touch_demo_enable(false);
    ryz_display_show_abort();
    (void)refresh_system_network(shell, true);
    (void)refresh_system_storage(shell, true);
    ryz_system_ui_reset();
    shell->ui_active = display_ready;
    shell->render_dirty = true;
}

static void consume_system_ui_action(system_shell_t *shell,
                                     ryz_system_ui_action_t action)
{
    if(action==RYZ_SYSTEM_UI_ACTION_NONE) return;
    ryz_system_services_snapshot_t before;
    if(ryz_system_services_get_snapshot(&before)==ESP_OK)
        shell->ui_network_seen_operation=before.network_operation.requested;
    uint32_t operation_id=0;
    esp_err_t error=ESP_ERR_NOT_SUPPORTED;
    const ryz_ui_nav_token_t issued_from=ryz_system_ui_page_token();
    if(action>=RYZ_SYSTEM_UI_ACTION_REPROVISION && action<=RYZ_SYSTEM_UI_ACTION_NETWORK_RETRY) {
        error=ryz_workbench_network_request(action,&operation_id);
        if(error==ESP_OK && action==RYZ_SYSTEM_UI_ACTION_REPROVISION)
            (void)ryz_system_ui_navigate(issued_from,RYZ_SYSTEM_UI_ROUTE_AP_SETUP);
    } else if(action==RYZ_SYSTEM_UI_ACTION_OTA_CANCEL) {
        error=ryz_ota_cancel_update();
    } else if(action==RYZ_SYSTEM_UI_ACTION_OTA_RESTART) {
        ryz_v5_status_t displayed;
        ryz_v5_status_get(&displayed);
        /* Input and this consumer run synchronously on the same Owner, with
         * no status publication between them. The UI has already matched DOWN
         * and UP against the actually presented nonzero attempt. The OTA core
         * rejects a newer backend attempt even if the view has not caught up. */
        shell->ui_restart_attempt=displayed.version.attempt_id;
        error=ryz_workbench_restart_updated(displayed.version.attempt_id,&file_write_guard);
        shell->ui_restart_error=error;
    }
    if(action<=RYZ_SYSTEM_UI_ACTION_NETWORK_RETRY)
        shell->ui_network_admission_error=error;
    if (error != ESP_OK) {
        report_system_shell_error(
            shell, action==RYZ_SYSTEM_UI_ACTION_OTA_CANCEL || action==RYZ_SYSTEM_UI_ACTION_OTA_RESTART ?
                SYSTEM_SHELL_ERROR_OTA_ACTION : SYSTEM_SHELL_ERROR_REPROVISION, error);
    }
    if (refresh_system_network(shell, true)) (void)render_system_ui(shell);
}

static esp_err_t password_ui_inspect(void *unused,ryz_v5_password_metadata_t *out)
{
    (void)unused;
    if(!out) return ESP_ERR_INVALID_ARG;
    *out=(ryz_v5_password_metadata_t){0};
    ryz_provisioning_local_secret_t source={0};
    esp_err_t error=ryz_provisioning_local_secret_try_get(&source);
    if(error==ESP_OK) *out=(ryz_v5_password_metadata_t){source.available,source.epoch};
    return error;
}

static esp_err_t password_ui_copy(void *unused,uint64_t epoch,char *out,size_t capacity)
{
    (void)unused;
    return ryz_provisioning_local_secret_try_copy(epoch,out,capacity);
}

static void sync_apps_view(system_shell_t *shell)
{
    const ryz_ui_nav_token_t page = ryz_system_ui_page_token();
    const ryz_system_ui_route_t route = ryz_system_ui_current_route();
    const bool scripts = shell->ui_active && page.generation != 0 &&
        route >= RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS && route <= RYZ_SYSTEM_UI_ROUTE_SCRIPTS_DELETE_ALL;
    const bool active = shell->ui_active && !page.modal && page.generation != 0 &&
        route == RYZ_SYSTEM_UI_ROUTE_APPS;
    /* One ryz_apps reader: close the previous consumer before opening the next.
     * An inactive controller must not repeatedly close another's live view. */
    if (scripts) {
        ryz_workbench_apps_owner_tick(false, page, launch_system_app, NULL);
        ryz_workbench_scripts_owner_tick(true, page,
            (ryz_v5_scripts_page_t)(route - RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SETTINGS));
    } else {
        ryz_workbench_scripts_owner_tick(false, page, RYZ_V5_SCRIPTS_SETTINGS);
        ryz_workbench_apps_owner_tick(active, page, launch_system_app, NULL);
    }
    ryz_v5_status_t view;
    ryz_v5_status_get(&view);
    if (ryz_workbench_scripts_get_snapshot(&view.scripts) == ESP_OK) {
        if(ryz_v5_status_set(&view)) shell->render_dirty=true;
        if(scripts && route==RYZ_SYSTEM_UI_ROUTE_SCRIPTS_PICKER && view.scripts.show_saved &&
           ryz_system_ui_navigate(page,RYZ_SYSTEM_UI_ROUTE_SCRIPTS_SAVED)==ESP_OK)
            shell->render_dirty=true;
    }
}

static esp_err_t scripts_ui_submit(void *unused, const ryz_v5_scripts_intent_t *intent)
{ (void)unused; return ryz_workbench_scripts_submit(intent); }

static esp_err_t display_ui_get(void *unused,ryz_display_settings_snapshot_t *out)
{ (void)unused; return ryz_display_settings_get_snapshot(out); }
static esp_err_t display_ui_submit(void *unused,uint64_t revision,ryz_display_settings_value_t value)
{ (void)unused; return ryz_display_settings_submit(revision,value); }
static esp_err_t display_ui_preview(void *unused,uint64_t revision,uint8_t brightness)
{ (void)unused; return ryz_display_settings_preview(revision,brightness); }
static void display_ui_end_preview(void *unused)
{ (void)unused; ryz_display_settings_end_preview(); }

static bool file_try_begin_write(void *unused)
{
    (void)unused;
    if (!job_lock || xSemaphoreTake(job_lock, 0) != pdTRUE) return false;
    bool accepted = current == NULL && !file_write_active;
    if (accepted) file_write_active = true;
    xSemaphoreGive(job_lock);
    return accepted;
}

static void file_end_write(void *unused)
{
    (void)unused;
    xSemaphoreTake(job_lock, portMAX_DELAY);
    file_write_active = false;
    xSemaphoreGive(job_lock);
}

static const ryz_workbench_write_guard_t file_write_guard = {
    .try_begin_write=file_try_begin_write, .end_write=file_end_write,
};

/* UI owns only a bounded copy model. It does not depend on Workbench or call
 * the Store; this adapter is invoked only from the existing display Owner. */
static EXT_RAM_BSS_ATTR ryz_workbench_apps_snapshot_t apps_ui_copy;
static esp_err_t apps_ui_snapshot(void *unused, ryz_v5_apps_snapshot_t *out)
{
    (void)unused;
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    esp_err_t error = ryz_workbench_apps_get_snapshot(&apps_ui_copy);
    if (error != ESP_OK) return error;
    out->open = apps_ui_copy.open;
    out->token.page = apps_ui_copy.token.page;
    out->token.reader_request_id = apps_ui_copy.token.reader_request_id;
    out->reader = apps_ui_copy.reader;
    out->page_limit = apps_ui_copy.page_limit;
    switch (apps_ui_copy.run_state) {
    case RYZ_WB_APPS_RUN_IDLE: out->run_state = RYZ_V5_APPS_IDLE; break;
    case RYZ_WB_APPS_RUN_PREPARING: out->run_state = RYZ_V5_APPS_PREPARING; break;
    case RYZ_WB_APPS_RUN_STARTED: out->run_state = RYZ_V5_APPS_STARTED; break;
    case RYZ_WB_APPS_RUN_FAILED: out->run_state = RYZ_V5_APPS_FAILED; break;
    default: out->run_state = RYZ_V5_APPS_UNKNOWN; break;
    }
    switch (apps_ui_copy.deletion.state) {
    case RYZ_WB_APPS_DELETE_IDLE: out->delete_state = RYZ_V5_APPS_DELETE_IDLE; break;
    case RYZ_WB_APPS_DELETE_PENDING: out->delete_state = RYZ_V5_APPS_DELETE_PENDING; break;
    case RYZ_WB_APPS_DELETE_COMMITTED: out->delete_state = RYZ_V5_APPS_DELETE_COMMITTED; break;
    case RYZ_WB_APPS_DELETE_FAILED: out->delete_state = RYZ_V5_APPS_DELETE_FAILED; break;
    default: out->delete_state = RYZ_V5_APPS_DELETE_UNKNOWN; break;
    }
    out->delete_error = apps_ui_copy.deletion.error;
    out->delete_cleanup_error = apps_ui_copy.deletion.cleanup_error;
    out->delete_recovery_required = apps_ui_copy.deletion.recovery_required;
    out->action_error = apps_ui_copy.action_error;
    snprintf(out->message, sizeof(out->message), "%s", apps_ui_copy.message);
    return ESP_OK;
}

static esp_err_t apps_ui_submit(void *unused, const ryz_v5_apps_intent_t *intent)
{
    (void)unused;
    if (!intent) return ESP_ERR_INVALID_ARG;
    ryz_workbench_apps_intent_t request = {
        .expected = {.page = intent->expected.page,
                     .reader_request_id = intent->expected.reader_request_id},
        .offset = intent->offset, .limit = intent->limit, .index = intent->index,
    };
    switch (intent->action) {
    case RYZ_V5_APPS_PAGE: request.action = RYZ_WB_APPS_PAGE; break;
    case RYZ_V5_APPS_SELECT: request.action = RYZ_WB_APPS_SELECT; break;
    case RYZ_V5_APPS_BACK: request.action = RYZ_WB_APPS_BACK; break;
    case RYZ_V5_APPS_RUN: request.action = RYZ_WB_APPS_RUN; break;
    case RYZ_V5_APPS_DELETE:
        request.action = RYZ_WB_APPS_DELETE;
        memcpy(request.delete_name, intent->delete_name, sizeof(request.delete_name));
        memcpy(request.delete_sha256, intent->delete_sha256, sizeof(request.delete_sha256));
        break;
    default: return ESP_ERR_INVALID_ARG;
    }
    return ryz_workbench_apps_submit(&request);
}

static cJSON *job_json(const script_job_t *job, bool include_log)
{
    if (!job) return cJSON_CreateNull();
    ryz_workbench_tools_report_t tools;
    esp_err_t observed = ryz_workbench_tools_report(job->id,
        job->active ? NULL : &job->tools.final, &tools);
    if (observed != ESP_OK) {
        tools = (ryz_workbench_tools_report_t){.phase = RYZ_WB_TOOLS_PENDING, .error = observed};
    }
    const bool clean = observed == ESP_OK && ryz_workbench_tools_clean(&tools);
    cJSON *tool_json = ryz_workbench_tools_report_json(&tools);
    cJSON *r = cJSON_CreateObject();
    if (!r || !tool_json) { cJSON_Delete(r); cJSON_Delete(tool_json); return NULL; }
    if (!cJSON_AddItemToObject(r, "tools", tool_json)) {
        cJSON_Delete(tool_json); cJSON_Delete(r); return NULL;
    }
    cJSON_AddStringToObject(r, "job_id", job->id);
    cJSON_AddStringToObject(r, "name", job->name);
    cJSON_AddStringToObject(r, "sha256", job->sha256);
    cJSON_AddStringToObject(r, "state", job->active ? "running" :
        job->payload->result.ok && clean ? "done" :
        !strcmp(job->payload->result.phase, "stopped") ? "stopped" :
        !strcmp(job->payload->result.phase, "timeout") ? "timeout" : "failed");
    cJSON_AddBoolToObject(r, "stop_requested", atomic_load(&job->cancel));
    cJSON_AddNumberToObject(r, "elapsed_ms", job->active ?
        (esp_timer_get_time() - job->started_us) / 1000 : job->payload->result.elapsed_ms);
    cJSON_AddNumberToObject(r, "log_next_seq", job->sequence);
    cJSON_AddNumberToObject(r, "dropped_bytes", job->dropped_bytes);
    if (include_log) {
        size_t length = job->log_length > 512 ? 512 : job->log_length;
        add_base64(r, "log_b64", job->payload->log + job->log_length - length, length);
        cJSON_AddBoolToObject(r, "log_truncated", length < job->log_length);
    }
    if (!job->active) {
        if (!cJSON_AddBoolToObject(r, "ok", job->payload->result.ok && clean) ||
            !cJSON_AddBoolToObject(r, "lua_ok", job->payload->result.ok)) {
            cJSON_Delete(r); return NULL;
        }
        cJSON_AddStringToObject(r, "phase", job->payload->result.phase);
        cJSON_AddStringToObject(r, "error", job->payload->result.error);
        cJSON_AddNumberToObject(r, "lua_peak_bytes", job->payload->result.peak_bytes);
    }
    return r;
}

static const char *ota_state_name(ryz_ota_state_t state)
{
    switch (state) {
    case RYZ_OTA_DISABLED:
        return "disabled";
    case RYZ_OTA_WAITING_PREREQUISITES:
        return "waiting_prerequisites";
    case RYZ_OTA_IDLE:
        return "idle";
    case RYZ_OTA_STARTING:
        return "starting";
    case RYZ_OTA_DOWNLOADING:
        return "downloading";
    case RYZ_OTA_VERIFYING:
        return "verifying";
    case RYZ_OTA_CANCELLING:
        return "cancelling";
    case RYZ_OTA_CANCELLED:
        return "cancelled";
    case RYZ_OTA_READY_TO_REBOOT:
        return "ready_to_reboot";
    case RYZ_OTA_FAILED:
    default:
        return "failed";
    }
}

static cJSON *ota_snapshot_response(const char *id, bool ok,
                                    const char *error)
{
    ryz_ota_snapshot_t snapshot;
    esp_err_t snapshot_error = ryz_ota_get_snapshot(&snapshot);
    if (snapshot_error != ESP_OK) {
        return response(id, false,
                        error != NULL ? error : esp_err_to_name(snapshot_error));
    }
    cJSON *result = response(id, ok, error);
    cJSON_AddStringToObject(result, "state", ota_state_name(snapshot.state));
    cJSON_AddNumberToObject(result, "revision", snapshot.revision);
    cJSON_AddBoolToObject(result, "configured", snapshot.configured);
    cJSON_AddBoolToObject(result, "network_ready", snapshot.network_ready);
    cJSON_AddBoolToObject(result, "clock_valid", snapshot.clock_valid);
    cJSON_AddBoolToObject(result, "network_held", snapshot.network_held);
    cJSON_AddBoolToObject(result, "worker_active", snapshot.worker_active);
    cJSON_AddBoolToObject(result, "cleanup_confirmed", snapshot.cleanup_confirmed);
    cJSON_AddNumberToObject(result, "cleanup_error", snapshot.cleanup_error);
    cJSON_AddNumberToObject(result, "attempt_id", snapshot.attempt_id);
    cJSON_AddBoolToObject(result, "reboot_required",
                          snapshot.reboot_required);
    cJSON_AddBoolToObject(result, "pending_confirmation",
                          snapshot.pending_confirmation);
    cJSON_AddNumberToObject(result, "downloaded_bytes",
                            snapshot.downloaded_bytes);
    cJSON_AddNumberToObject(result, "total_bytes", snapshot.total_bytes);
    cJSON_AddStringToObject(result, "running_version",
                            snapshot.running_version);
    if (snapshot.candidate_version[0] != '\0') {
        cJSON_AddStringToObject(result, "candidate_version",
                                snapshot.candidate_version);
    }
    if (snapshot.last_error != ESP_OK) {
        cJSON_AddStringToObject(result, "last_error",
                                esp_err_to_name(snapshot.last_error));
    }
    return result;
}

static cJSON *ota_control(const char *id, cJSON *request)
{
    const char *action = field(request, "action");
    if (action == NULL || !strcmp(action, "status")) {
        return ota_snapshot_response(id, true, NULL);
    }

    esp_err_t error;
    if (!strcmp(action, "start")) {
        error = ryz_ota_start_update();
    } else if (!strcmp(action, "cancel")) {
        error = ryz_ota_cancel_update();
    } else {
        return ota_snapshot_response(
            id, false, "OTA action must be status, start or cancel");
    }
    return ota_snapshot_response(
        id, error == ESP_OK, error == ESP_OK ? NULL : esp_err_to_name(error));
}

static cJSON *apps_diagnostics(void)
{
    ryz_apps_snapshot_t snapshot;
    const esp_err_t error = ryz_apps_get_snapshot(&snapshot);
    cJSON *value = cJSON_CreateObject();
    if (!value) return NULL;
    if (!cJSON_AddBoolToObject(value, "reader_running", error == ESP_OK) ||
        !cJSON_AddNumberToObject(value, "snapshot_error", error)) goto failed;
    if (error != ESP_OK) return value;
    static const char *const views[] = {"none", "catalog", "detail"};
    static const char *const states[] = {
        "idle", "loading", "ready", "empty", "failed", "stale",
    };
    char requested[21], completed[21];
    snprintf(requested, sizeof(requested), "%" PRIu64, snapshot.request_id);
    snprintf(completed, sizeof(completed), "%" PRIu64, snapshot.completed_id);
    if (!cJSON_AddStringToObject(value, "view", views[snapshot.view]) ||
        !cJSON_AddStringToObject(value, "state", states[snapshot.state]) ||
        !cJSON_AddStringToObject(value, "request_id", requested) ||
        !cJSON_AddStringToObject(value, "completed_id", completed) ||
        !cJSON_AddNumberToObject(value, "error", snapshot.error) ||
        !cJSON_AddBoolToObject(value, "store_status_valid", snapshot.store_status_valid) ||
        !cJSON_AddBoolToObject(value, "store_ready", snapshot.store_ready) ||
        !cJSON_AddBoolToObject(value, "recovery_required", snapshot.recovery_required)) goto failed;
    if (snapshot.view == RYZ_APPS_VIEW_CATALOG &&
        (snapshot.state == RYZ_APPS_READY || snapshot.state == RYZ_APPS_EMPTY)) {
        if (!cJSON_AddNumberToObject(value, "revision", snapshot.page.revision) ||
            !cJSON_AddNumberToObject(value, "total", snapshot.page.total) ||
            !cJSON_AddNumberToObject(value, "offset", snapshot.page.offset) ||
            !cJSON_AddNumberToObject(value, "count", snapshot.page.count)) goto failed;
    }
    return value;
failed:
    cJSON_Delete(value);
    return NULL;
}

static cJSON *sensors_diagnostics(void)
{
    ryz_sensors_snapshot_t snapshot;
    const esp_err_t error = ryz_sensors_get_snapshot(&snapshot);
    cJSON *value = cJSON_CreateObject();
    if (!value) return NULL;
    if (!cJSON_AddBoolToObject(value, "worker_running", error == ESP_OK) ||
        !cJSON_AddNumberToObject(value, "snapshot_error", error)) goto failed;
    if (error != ESP_OK) return value;
    if (!cJSON_AddBoolToObject(value, "initialized", snapshot.initialized) ||
        !cJSON_AddBoolToObject(value, "ever_sampled", snapshot.ever_sampled) ||
        !cJSON_AddBoolToObject(value, "sample_valid", snapshot.sample_valid) ||
        !cJSON_AddNumberToObject(value, "last_error", snapshot.last_error) ||
        !cJSON_AddNumberToObject(value, "last_error_ms", (double)snapshot.last_error_ms) ||
        !cJSON_AddNumberToObject(value, "init_attempts", snapshot.init_attempts) ||
        !cJSON_AddNumberToObject(value, "read_attempts", snapshot.read_attempts) ||
        !cJSON_AddNumberToObject(value, "sample_count", snapshot.sample_count) ||
        !cJSON_AddNumberToObject(value, "no_data_count", snapshot.no_data_count) ||
        !cJSON_AddNumberToObject(value, "error_count", snapshot.error_count)) goto failed;
    if (snapshot.ever_sampled &&
        (!cJSON_AddNumberToObject(value, "last_sampled_ms", (double)snapshot.last_sampled_ms) ||
         !cJSON_AddNumberToObject(value, "sample_age_ms", (double)snapshot.sample_age_ms))) goto failed;
    if (snapshot.initialized &&
        (!cJSON_AddStringToObject(value, "model", "LIS2DW12") ||
         !cJSON_AddNumberToObject(value, "address", snapshot.imu.address) ||
         !cJSON_AddNumberToObject(value, "who_am_i", snapshot.imu.who_am_i) ||
         !cJSON_AddNumberToObject(value, "odr_hz", snapshot.imu.odr_hz) ||
         !cJSON_AddNumberToObject(value, "full_scale_g", snapshot.imu.full_scale_g))) goto failed;
    /* Do not send old/zero axes as a new observation on init/read failure.
     * Even a valid cache keeps its acquisition time and an explicit age. */
    if (snapshot.sample_valid) {
        char acquired[21];
        snprintf(acquired, sizeof(acquired), "%" PRIu64, snapshot.sample.timestamp_us);
        if (!cJSON_AddStringToObject(value, "axes", "sensor") ||
            !cJSON_AddStringToObject(value, "sampled_us", acquired) ||
            !cJSON_AddNumberToObject(value, "sequence", snapshot.sample.sequence) ||
            !cJSON_AddNumberToObject(value, "x_mg", snapshot.sample.x_mg) ||
            !cJSON_AddNumberToObject(value, "y_mg", snapshot.sample.y_mg) ||
            !cJSON_AddNumberToObject(value, "z_mg", snapshot.sample.z_mg)) goto failed;
    }
    return value;
failed:
    cJSON_Delete(value);
    return NULL;
}

static void add_ui_timing(cJSON *reply, const char *name,
                          const ui_call_timing_t *timing)
{
    cJSON *value = cJSON_AddObjectToObject(reply, name);
    if (!value) return;
    cJSON_AddNumberToObject(value, "calls",
        atomic_load_explicit(&timing->calls, memory_order_relaxed));
    cJSON_AddNumberToObject(value, "last_us",
        atomic_load_explicit(&timing->last_us, memory_order_relaxed));
    cJSON_AddNumberToObject(value, "max_us",
        atomic_load_explicit(&timing->max_us, memory_order_relaxed));
}

static cJSON *device_info(const char *id)
{
    cJSON *r = response(id, true, NULL);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    cJSON_AddStringToObject(r, "chip", "ESP32-S3");
    cJSON_AddNumberToObject(r, "chip_revision", chip.revision);
    cJSON_AddStringToObject(r, "lua", LUA_VERSION);
    cJSON_AddStringToObject(r, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(r, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(r, "boot_id", boot_id);
    if (device_id[0]) cJSON_AddStringToObject(r, "device_id", device_id);
    cJSON_AddNumberToObject(r, "protocol_version", 2);
    cJSON_AddStringToObject(r, "neuro_runtime", RYZ_NEURO_ABI);
    cJSON_AddStringToObject(r, "neuro_catalog", RYZ_NEURO_CATALOG);
    cJSON_AddStringToObject(r, "neuro_core", RYZ_NEURO_CORE_SHA);
    cJSON_AddNumberToObject(r, "neuro_node_limit", RYZ_NEURO_NODES);
    cJSON_AddStringToObject(r, "app_runtime", RYZ_APP_ABI);
    cJSON_AddStringToObject(r, "app_catalog", RYZ_APP_CATALOG);
    cJSON_AddStringToObject(r, "app_core", RYZ_APP_CORE_SHA);
    cJSON_AddNumberToObject(r, "flash_bytes", flash);
    cJSON_AddNumberToObject(r, "psram_bytes", esp_psram_get_size());
    cJSON_AddNumberToObject(r, "free_internal_bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(r, "free_internal_largest_block_bytes",
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(r, "minimum_free_internal_bytes",
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(r, "free_psram_bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    /* Whitelisted C-owned radio diagnostics only. Do not serialize the full
     * snapshot: it also carries identities and the active comparison code. */
    ryz_ble_snapshot_t ble_state = {0};
    const esp_err_t ble_snapshot_error = ryz_ble_get_snapshot(&ble_state);
    cJSON *ble = cJSON_AddObjectToObject(r, "ble");
    if (ble) {
        cJSON_AddNumberToObject(ble, "snapshot_error", ble_snapshot_error);
        if (ble_snapshot_error == ESP_OK) {
            cJSON_AddBoolToObject(ble, "available", ble_state.available);
            cJSON_AddNumberToObject(ble, "phase", ble_state.phase);
            cJSON_AddNumberToObject(ble, "failure", ble_state.failure);
            cJSON_AddNumberToObject(ble, "last_error", ble_state.last_error);
            cJSON_AddNumberToObject(ble, "sdk_error", ble_state.sdk_error);
            cJSON_AddBoolToObject(ble, "enabled", ble_state.enabled);
            cJSON_AddBoolToObject(ble, "saved_enabled", ble_state.saved_enabled);
            cJSON_AddBoolToObject(ble, "bond_known", ble_state.bond_known);
            cJSON_AddBoolToObject(ble, "bonded", ble_state.bonded);
            cJSON_AddBoolToObject(ble, "linked", ble_state.linked);
            cJSON_AddBoolToObject(ble, "authenticated", ble_state.authenticated);
            cJSON_AddNumberToObject(ble, "ancs_state", ble_state.ancs_state);
            cJSON_AddNumberToObject(ble, "ancs_sdk_error", ble_state.ancs_sdk_error);
            cJSON_AddBoolToObject(ble, "ancs_service_changed_subscribed", ble_state.ancs_service_changed_subscribed);
            cJSON_AddBoolToObject(ble, "hid_ready", ble_state.hid_ready);
            cJSON_AddBoolToObject(ble, "hid_busy", ble_state.hid_busy);
            cJSON_AddNumberToObject(ble, "hid_sdk_error", ble_state.hid_sdk_error);
            cJSON_AddBoolToObject(ble, "boot_settled", ble_state.boot_settled);
            cJSON_AddNumberToObject(ble, "boot_error", ble_state.boot_error);
            cJSON_AddBoolToObject(ble, "checking_state", ble_state.checking_state);
        }
        ryz_ble_init_diagnostics_t init = {0};
        if (ryz_ble_get_init_diagnostics(&init) == ESP_OK) {
            cJSON *diagnostics = cJSON_AddObjectToObject(ble, "init");
            if (diagnostics) {
                cJSON_AddBoolToObject(diagnostics, "finished", init.finished);
                const char *const names[] = {"port", "buffers", "vhci"};
                const ryz_ble_init_probe_t *const probes[] = {&init.port, &init.buffers, &init.vhci};
                for (size_t i = 0; i < 3; ++i) {
                    const ryz_ble_init_probe_t *probe = probes[i];
                    cJSON *entry = cJSON_AddObjectToObject(diagnostics, names[i]);
                    if (!entry) continue;
                    cJSON_AddBoolToObject(entry, "entered", probe->entered);
                    cJSON_AddBoolToObject(entry, "returned", probe->returned);
                    if (probe->returned) cJSON_AddNumberToObject(entry, "result", probe->result);
                    if (probe->entered) {
                        cJSON_AddNumberToObject(entry, "internal_free_before", probe->internal_free_before);
                        cJSON_AddNumberToObject(entry, "internal_largest_before", probe->internal_largest_before);
                    }
                    if (probe->returned) {
                        cJSON_AddNumberToObject(entry, "internal_free_after", probe->internal_free_after);
                        cJSON_AddNumberToObject(entry, "internal_largest_after", probe->internal_largest_after);
                    }
                }
            }
        }
    }
    cJSON_AddNumberToObject(r, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(r, "source_limit_bytes", RYZ_LUA_SOURCE_MAX);
    cJSON_AddNumberToObject(r, "lua_heap_limit_bytes", RYZ_LUA_HEAP_LIMIT);
    cJSON_AddNumberToObject(r, "request_limit_bytes", RX_MAX - 1);
    cJSON_AddBoolToObject(r, "filesystem_ready", filesystem_ready);
    size_t filesystem_total_bytes = 0;
    size_t filesystem_used_bytes = 0;
    ryz_script_store_status_t store_status = {0};
    const bool store_state_ready = filesystem_ready &&
        ryz_script_store_status(&store_status) == ESP_OK;
    const bool filesystem_info_ready =
        store_state_ready && store_status.ready && store_status.capacity_valid;
    if (filesystem_info_ready) {
        filesystem_total_bytes = store_status.total_bytes;
        filesystem_used_bytes = store_status.used_bytes;
    }
    cJSON_AddBoolToObject(r, "script_store_state_ready", store_state_ready);
    if (store_state_ready) {
        cJSON_AddBoolToObject(r, "script_store_ready", store_status.ready);
        cJSON_AddBoolToObject(r, "script_store_recovery_required", store_status.recovery_required);
        cJSON_AddNumberToObject(r, "script_store_revision", store_status.revision);
    }
    cJSON_AddBoolToObject(r, "filesystem_info_ready", filesystem_info_ready);
    cJSON_AddNumberToObject(r, "filesystem_total_bytes",
                            filesystem_total_bytes);
    cJSON_AddNumberToObject(r, "filesystem_used_bytes",
                            filesystem_used_bytes);
    cJSON_AddBoolToObject(r, "display_driver_ready", display_ready);
    cJSON_AddBoolToObject(r, "font_engine_ready", ryz_font_ready());
    cJSON_AddBoolToObject(r, "ui_fonts_ready", font_ready);
    add_ui_timing(r, "ui_touch_read_timing", &ui_touch_read_timing);
    add_ui_timing(r, "ui_touch_handle_timing", &ui_touch_handle_timing);
    add_ui_timing(r, "ui_render_timing", &ui_render_timing);
    add_ui_timing(r, "ui_tick_timing", &ui_tick_timing);
    /* On-demand, byte-based IDF watermark of the boot-lifetime UI owner.
     * This is observed headroom, not proof of every future page's peak. */
    cJSON_AddNumberToObject(r, "ui_owner_stack_bytes", SYSTEM_UI_TASK_STACK_BYTES);
    if (ui_owner != NULL) {
        cJSON_AddNumberToObject(r, "ui_owner_stack_free_min_bytes",
                                uxTaskGetStackHighWaterMark(ui_owner));
    }
    /* This is cached, read-only diagnostics, NOT a second controller of the
     * screen's request slot and NOT a claim that the V5 list is rendered. */
    cJSON *apps = apps_diagnostics();
    if (!apps || !cJSON_AddItemToObject(r, "apps", apps)) {
        cJSON_Delete(apps);
        cJSON_Delete(r);
        return NULL;
    }
    cJSON *sensors = sensors_diagnostics();
    if (!sensors || !cJSON_AddItemToObject(r, "sensors", sensors)) {
        cJSON_Delete(sensors);
        cJSON_Delete(r);
        return NULL;
    }
    /* Expose only non-sensitive scheduling diagnostics, not the trusted
     * system snapshot (which includes the local AP QR password). */
    ryz_system_services_snapshot_t services;
    const bool services_running =
        ryz_system_services_get_snapshot(&services) == ESP_OK;
    cJSON_AddBoolToObject(r, "system_services_running", services_running);
    if (services_running) {
        cJSON_AddNumberToObject(r, "system_services_cycles", (double)services.cycles);
        cJSON_AddNumberToObject(r, "system_services_updated_us",
                                (double)services.updated_at_us);
        cJSON_AddBoolToObject(r, "system_network_valid", services.network_valid);
        cJSON_AddBoolToObject(r, "system_provisioning_started",
                              services.provisioning_started);
        cJSON_AddNumberToObject(r, "system_reprovision_requested",
                                services.reprovision_requested);
        cJSON_AddNumberToObject(r, "system_reprovision_completed",
                                services.reprovision_completed);
        if (services.reprovision_completed != 0) {
            cJSON_AddNumberToObject(r, "system_reprovision_result",
                                    services.reprovision_result);
        }
    }
    ryz_time_snapshot_t time_state;
    const bool time_state_ready =
        ryz_time_get_snapshot(&time_state) == ESP_OK;
    cJSON_AddBoolToObject(r, "time_service_ready", time_state_ready);
    if (time_state_ready) {
        cJSON_AddNumberToObject(r, "time_state", time_state.state);
        cJSON_AddBoolToObject(r, "clock_valid", time_state.clock_valid);
        cJSON_AddNumberToObject(r, "last_sync_unix",
                                (double)time_state.last_sync_unix);
    }
    ryz_ota_snapshot_t ota_state;
    const bool ota_state_ready = ryz_ota_get_snapshot(&ota_state) == ESP_OK;
    cJSON_AddBoolToObject(r, "ota_service_ready", ota_state_ready);
    if (ota_state_ready) {
        cJSON_AddNumberToObject(r, "ota_state", ota_state.state);
        cJSON_AddBoolToObject(r, "ota_configured", ota_state.configured);
        cJSON_AddBoolToObject(r, "ota_network_held", ota_state.network_held);
        cJSON_AddBoolToObject(r, "ota_worker_active", ota_state.worker_active);
        cJSON_AddBoolToObject(r, "ota_cleanup_confirmed", ota_state.cleanup_confirmed);
        cJSON_AddNumberToObject(r, "ota_cleanup_error", ota_state.cleanup_error);
        cJSON_AddNumberToObject(r, "ota_attempt_id", ota_state.attempt_id);
        cJSON_AddBoolToObject(r, "ota_pending_confirmation",
                              ota_state.pending_confirmation);
        cJSON_AddNumberToObject(r, "ota_downloaded_bytes",
                                ota_state.downloaded_bytes);
        cJSON_AddNumberToObject(r, "ota_total_bytes", ota_state.total_bytes);
    }
    xSemaphoreTake(job_lock, portMAX_DELAY);
    cJSON_AddBoolToObject(r, "touch_driver_ready", touch_snapshot.ready);
    cJSON_AddNumberToObject(r, "touch_chip_id", touch_snapshot.chip_id);
    cJSON_AddNumberToObject(r, "touch_firmware_version", touch_snapshot.firmware_version);
    cJSON_AddNumberToObject(r, "touch_reads", touch_snapshot.reads);
    cJSON_AddNumberToObject(r, "touch_errors", touch_snapshot.errors);
    cJSON_AddNumberToObject(r, "touch_presses", touch_snapshot.presses);
    cJSON_AddNumberToObject(r, "touch_releases", touch_snapshot.releases);
    cJSON_AddBoolToObject(r, "touch_demo_enabled", demo_snapshot);
    cJSON_AddBoolToObject(r, "runtime_gateway_ready",
                          atomic_load(&runtime_gateway_ready));
    cJSON_AddNumberToObject(r, "ui_owner_cycles", (double)ui_owner_cycles);
    cJSON_AddNumberToObject(r, "ui_input_dropped", ui_input_dropped);
    cJSON_AddItemToObject(r, "job", job_json(current, false));
    cJSON_AddItemToObject(r, "recent_job", job_json(recent, false));
    xSemaphoreGive(job_lock);
    return r;
}

static cJSON *boot_info(const char *id)
{
    cJSON *r = response(id, true, NULL);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_err_t flash_status = esp_flash_get_size(NULL, &flash_size);
    cJSON_AddStringToObject(r, "chip", "ESP32-S3");
    cJSON_AddNumberToObject(r, "chip_revision", chip.revision);
    cJSON_AddStringToObject(r, "lua", LUA_VERSION);
    cJSON_AddStringToObject(r, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(r, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(r, "app_runtime", RYZ_APP_ABI);
    cJSON_AddStringToObject(r, "app_catalog", RYZ_APP_CATALOG);
    cJSON_AddStringToObject(r, "app_core", RYZ_APP_CORE_SHA);
    cJSON_AddNumberToObject(r, "flash_bytes", flash_status == ESP_OK ? flash_size : 0);
    cJSON_AddNumberToObject(r, "psram_bytes", esp_psram_get_size());
    cJSON_AddNumberToObject(r, "free_internal_bytes",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(r, "free_internal_largest_block_bytes",
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(r, "minimum_free_internal_bytes",
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(r, "free_psram_bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(r, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddBoolToObject(r, "filesystem_ready", filesystem_ready);
    cJSON_AddBoolToObject(r, "display_driver_ready", display_ready);
    cJSON_AddBoolToObject(r, "font_engine_ready", ryz_font_ready());
    cJSON_AddBoolToObject(r, "ui_fonts_ready", font_ready);
    ryz_touch_info_t touch;
    ryz_touch_get_info(&touch);
    cJSON_AddBoolToObject(r, "touch_driver_ready", touch.ready);
    cJSON_AddNumberToObject(r, "touch_chip_id", touch.chip_id);
    cJSON_AddNumberToObject(r, "touch_firmware_version", touch.firmware_version);
    cJSON_AddNumberToObject(r, "touch_reads", touch.reads);
    cJSON_AddNumberToObject(r, "touch_errors", touch.errors);
    cJSON_AddNumberToObject(r, "touch_presses", touch.presses);
    cJSON_AddNumberToObject(r, "touch_releases", touch.releases);
    cJSON_AddBoolToObject(r, "touch_demo_enabled", ryz_touch_demo_enabled());
    cJSON_AddNumberToObject(r, "source_limit_bytes", RYZ_LUA_SOURCE_MAX);
    cJSON_AddNumberToObject(r, "lua_heap_limit_bytes", RYZ_LUA_HEAP_LIMIT);
    return r;
}

static void output_callback(void *context, const char *bytes, size_t length)
{
    script_job_t *job = context;
    output_t chunk;
    snprintf(chunk.job_id, sizeof(chunk.job_id), "%s", job->id);
    while (length) {
        if (atomic_load(&job->cancel)) break;
        size_t n = length > LOG_CHUNK ? LOG_CHUNK : length;
        chunk.length = n;
        memcpy(chunk.bytes, bytes, n);
        xSemaphoreTake(job_lock, portMAX_DELAY);
        chunk.sequence = job->sequence++;
        if (job->log_length + n > LOG_BYTES) {
            size_t remove = job->log_length + n - LOG_BYTES;
            memmove(job->payload->log, job->payload->log + remove,
                    job->log_length - remove);
            job->log_length -= remove;
        }
        memcpy(job->payload->log + job->log_length, bytes, n);
        job->log_length += n;
        if (xQueueSend(output_queue, &chunk, 0) != pdTRUE) job->dropped_bytes += n;
        xSemaphoreGive(job_lock);
        xTaskNotifyGive(transmitter);
        bytes += n;
        length -= n;
    }
}

static cJSON *legacy_result(const char *id, const ryz_lua_result_t *result,
                           const ryz_workbench_tools_report_t *tools)
{
    const bool clean = !tools || ryz_workbench_tools_clean(tools);
    cJSON *r = response(id, result->ok && clean, !result->ok ? result->error :
        clean ? NULL : "tool cleanup is pending or failed; query tools status");
    if (!r) return NULL;
    if (tools && tools->phase != RYZ_WB_TOOLS_UNUSED) {
        cJSON *value = ryz_workbench_tools_report_json(tools);
        if (!value || !cJSON_AddBoolToObject(r, "lua_ok", result->ok)) {
            cJSON_Delete(value); cJSON_Delete(r); return NULL;
        }
        if (!cJSON_AddItemToObject(r, "tools", value)) { cJSON_Delete(value); cJSON_Delete(r); return NULL; }
    }
    cJSON_AddStringToObject(r, "phase", result->phase);
    cJSON_AddStringToObject(r, "output", result->output);
    cJSON_AddBoolToObject(r, "output_truncated", result->output_truncated);
    cJSON_AddNumberToObject(r, "elapsed_ms", result->elapsed_ms);
    cJSON_AddNumberToObject(r, "lua_peak_bytes", result->peak_bytes);
    cJSON_AddNumberToObject(r, "free_internal_bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return r;
}

static cJSON *boot_execute(const char *id, const char *source, const char *name,
                           uint32_t timeout_ms)
{
    ryz_lua_result_t *result = calloc(1, sizeof(*result));
    if (!result) return response(id, false, "out of memory");
    ryz_lua_execute(source, strlen(source), name, timeout_ms, result);
    cJSON *r = legacy_result(id, result, NULL);
    free(result);
    return r;
}

static void destroy_job(script_job_t *job)
{
    ryz_workbench_job_destroy(job);
}

typedef struct {
    system_shell_t *shell;
    script_job_t *job;
    int64_t *next_touch;
    int64_t *next_network;
    int64_t *next_storage;
} job_completion_context_t;

static void resume_finished_job_ui(void *opaque)
{
    job_completion_context_t *context = opaque;
    resume_system_ui(context->shell);
    script_job_t *job=context->job;
    ryz_workbench_tools_report_t tools;
    esp_err_t observed=ryz_workbench_tools_report(job->id,&job->tools.final,&tools);
    int clean=observed!=ESP_OK?-1:ryz_workbench_tools_clean(&tools)?1:0;
    ryz_workbench_fault_t fault;
    const ryz_lua_result_t *result=&job->payload->result;
    const char *output=job->legacy?result->output:job->payload->log;
    size_t length=job->legacy?strnlen(result->output,sizeof(result->output)):job->log_length;
    if(ryz_workbench_fault_prepare(&fault,result,job->name,job->id,
        esp_app_get_description()->version,clean,output,length,result->output_truncated,
        context->shell->snapshot.ap_password)) {
        ryz_v5_fault_set(&fault.screen);
        if(ryz_diagnostics_publish(&fault.report)==ESP_OK) {
            char path[96];
            if(ryz_provisioning_diagnostics_ready() && ryz_diagnostics_get_path(path,sizeof(path))==ESP_OK)
                (void)ryz_v5_fault_link(path);
        }
        (void)ryz_system_ui_navigate(ryz_system_ui_page_token(),RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT);
        context->shell->render_dirty=true;
    }
    *context->next_touch = 0;
    *context->next_network = 0;
    *context->next_storage = 0;
    refresh_touch();
}

static void publish_finished_job(void *opaque)
{
    job_completion_context_t *context = opaque;
    script_job_t *job = context->job;
    free(job->source);
    job->source = NULL;
    xSemaphoreTake(job_lock, portMAX_DELAY);
    configASSERT(current == job);
    job->active = false;
    destroy_job(recent);
    recent = job;
    current = NULL;
    cJSON *event = NULL;
    if (!job->legacy) {
        event = cJSON_CreateObject();
        cJSON_AddStringToObject(event, "event", "job");
        cJSON_AddStringToObject(event, "boot_id", boot_id);
        cJSON_AddItemToObject(event, "job", job_json(job, false));
    }
    xSemaphoreGive(job_lock);
    if (job->legacy) {
        ryz_workbench_tools_report_t tools;
        esp_err_t observed = ryz_workbench_tools_report(job->id, &job->tools.final, &tools);
        if (observed != ESP_OK) tools = (ryz_workbench_tools_report_t){
            .phase = RYZ_WB_TOOLS_PENDING, .error = observed};
        send_json("RYZOBEE_RPC ",
                  legacy_result(job->reply_id, &job->payload->result, &tools));
    }
    if (event) send_json("RYZOBEE_EVENT ", event);
}

static void execute_runtime_io(void *opaque)
{
    ryz_runtime_io_request_t *request = opaque;
    ryz_runtime_io_execute(request, workbench_input_read, &runtime_input);
}

static bool call_runtime_io(void *opaque, ryz_runtime_io_request_t *request)
{
    (void)opaque;
    /* The sole Lua worker makes synchronous calls. The request, Lua string,
     * scene and cancellation context stay borrowed until owner ack, even when
     * a stop arrives during execution. Cleanup uses this same path. */
    if (xTaskGetCurrentTaskHandle() != lua_worker || !request ||
        !atomic_load(&runtime_gateway_ready)) return false;
    uint32_t ticket = ryz_workbench_io_submit(
        &runtime_channel, execute_runtime_io, request);
    if (!ticket) return false;
    xTaskNotifyGive(ui_owner);
    while (!ryz_workbench_io_completed(&runtime_channel, ticket)) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    }
    return ryz_workbench_io_acknowledge(&runtime_channel, ticket);
}

static int call_runtime_tool(void *opaque, const ryz_tool_request_t *request,
                             ryz_tool_reply_t *reply)
{
    script_job_t *job = opaque;
    if (!reply) return RYZ_TOOL_CALL_INVALID;
    if (!job || xTaskGetCurrentTaskHandle() != lua_worker ||
        !atomic_load(&runtime_gateway_ready) || atomic_load(&job->cancel)) {
        memset(reply, 0, sizeof(*reply));
        reply->error_code = ESP_ERR_INVALID_STATE;
        return RYZ_TOOL_CALL_STATE;
    }
    return ryz_workbench_tools_call(&job->tools, request, reply);
}

static void lua_worker_task(void *unused)
{
    (void)unused;
    for (;;) {
        script_job_t *job;
        if (xQueueReceive(worker_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        ryz_workbench_tools_begin(&job->tools, job->id);
        ryz_lua_options_t options = {
            .cancel = &job->cancel,
            .output = job->legacy ? NULL : output_callback,
            .context = job,
            .io_call = call_runtime_io,
            .tool_call = call_runtime_tool,
        };
        ryz_lua_execute_with_options(job->source, strlen(job->source), job->name,
                                     job->timeout_ms, &options, &job->payload->result);
        /* A retained numeric session record was reserved before first use.
         * Finish only waits to acquire that short observation gate, never for
         * native device teardown. RX continues pending cleanup independently
         * of this worker and the next (possibly long-running) Lua job. */
        while (!ryz_workbench_tools_finish(&job->tools, esp_timer_get_time())) vTaskDelay(1);
        /* Runtime cleanup has been acknowledged before its VM is destroyed.
         * The owner, not this worker, restores navigation and publishes result.
         * No job/source access is allowed after handing it back. */
        configASSERT(ryz_workbench_io_idle(&runtime_channel));
        configASSERT(xQueueSend(completion_queue, &job, 0) == pdTRUE);
        xTaskNotifyGive(ui_owner);
    }
}

static void sample_runtime_input(void)
{
    ryz_touch_sample_t sample;
    esp_err_t error = ryz_touch_read(&sample);
    if(ryz_display_settings_filter_touch(&sample,error,(uint64_t)esp_timer_get_time()/1000U)) {
        workbench_input_suppress(&runtime_input, &sample, error);
        return;
    }
    workbench_input_push(&runtime_input, &sample, error);
}

static void handle_boot_button_long_press(system_shell_t *shell,
                                          script_job_t *executing)
{
    if (executing) {
        /* Lua's instruction hook and every runtime I/O boundary observe this
         * flag. The existing completion path then restores HOME on the UI
         * Owner, so no second task ever touches the panel. */
        if (!atomic_exchange(&executing->cancel, true))
            ESP_LOGI(TAG, "BOOT held %u s: cancelling Lua job %s",
                     (unsigned)(RYZ_WORKBENCH_BOOT_KEY_HOLD_MS / 1000U), executing->id);
        return;
    }
    if (shell && shell->ui_active &&
        ryz_system_ui_current_route() != RYZ_SYSTEM_UI_ROUTE_HOME) {
        const ryz_ui_nav_token_t token = ryz_system_ui_page_token();
        if (ryz_system_ui_navigate(token, RYZ_SYSTEM_UI_ROUTE_HOME) == ESP_OK)
            shell->render_dirty = true;
    }
}

static void execute_task(void *early_display)
{
    if (early_display) {
        boot_animation_started_us = esp_timer_get_time();
        const esp_err_t first_frame = ryz_v5_boot_begin();
        boot_animation_active = first_frame == ESP_OK;
        atomic_store_explicit(&boot_first_frame, first_frame, memory_order_release);
    }
    /* No queue, service view, touch input or normal page before app_main has
     * published the complete workbench setup. Main may initialize FreeType
     * concurrently: the C splash has no FreeType dependency. */
    while (!atomic_load_explicit(&boot_setup_complete, memory_order_acquire)) {
        pump_boot_screen();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    system_shell_t shell;
    const bool autostart_requested = boot_autostart_source != NULL;
    start_system_shell(&shell, autostart_requested);
    if (autostart_requested) {
        char *source = boot_autostart_source;
        boot_autostart_source = NULL;
        cJSON *reply = start_job("autostart", "boot.lua", source, 0, false);
        const cJSON *ok = reply ? cJSON_GetObjectItemCaseSensitive(reply, "ok") : NULL;
        if (!cJSON_IsTrue(ok)) {
            const cJSON *error = reply ? cJSON_GetObjectItemCaseSensitive(reply, "error") : NULL;
            ESP_LOGE(TAG, "boot.lua autostart rejected: %s",
                     cJSON_IsString(error) ? error->valuestring : "response allocation failed");
            resume_system_ui(&shell);
        } else {
            ESP_LOGI(TAG, "boot.lua autostart accepted after splash");
        }
        cJSON_Delete(reply);
    }
    int64_t next_touch = 0;
    int64_t next_network = 0;
    int64_t next_storage = 0;
    int64_t next_boot_button = 0;
    script_job_t *executing = NULL;
    bool display_input_filtered=false;
    ryz_workbench_boot_key_t boot_key;
    ryz_workbench_boot_key_reset(&boot_key);
    for (;;) {
        bool display_changed=ryz_display_settings_owner_tick((uint64_t)esp_timer_get_time()/1000U,
            ryz_system_ui_current_route()!=RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT,
            !ryz_v5_password_needs_cleanup() && !ryz_v5_fault_needs_cleanup());
        if(display_changed && shell.ui_active) shell.render_dirty=true;
        xSemaphoreTake(job_lock, portMAX_DELAY);
        ++ui_owner_cycles;
        ui_input_dropped = runtime_input.dropped;
        xSemaphoreGive(job_lock);
        confirm_running_image_if_healthy(&shell);
        if (ryz_workbench_io_dispatch(&runtime_channel)) {
            refresh_touch();
            xTaskNotifyGive(lua_worker);
        }
        script_job_t *job;
        if (xQueueReceive(completion_queue, &job, 0) == pdTRUE) {
            configASSERT(executing == job);
            configASSERT(ryz_workbench_io_idle(&runtime_channel));
            job_completion_context_t completion_context = {
                .shell = &shell,
                .job = job,
                .next_touch = &next_touch,
                .next_network = &next_network,
                .next_storage = &next_storage,
            };
            const ryz_workbench_completion_t completion = {
                .context = &completion_context,
                .restore_owner_state = resume_finished_job_ui,
                .publish_completion = publish_finished_job,
            };
            ryz_workbench_complete_job(&completion);
            executing = NULL;
            workbench_input_reset(&runtime_input);
        }
        if (!executing && xQueueReceive(job_queue, &job, 0) == pdTRUE) {
            configASSERT(ryz_workbench_io_idle(&runtime_channel));
            esp_err_t handoff_error=suspend_system_ui(&shell);
            if(handoff_error!=ESP_OK) {
                /* No VM or worker_queue dispatch until sensitive pixels are
                 * scrubbed. Publish a terminal preparation failure through
                 * the existing completion path; no resources were acquired. */
                report_system_shell_error(&shell,SYSTEM_SHELL_ERROR_RENDER,handoff_error);
                memset(&job->payload->result, 0, sizeof(job->payload->result));
                job->payload->result.phase="prepare";
                snprintf(job->payload->result.error,
                         sizeof(job->payload->result.error),
                         "Display privacy cleanup failed; application not started");
                job->tools.final=(ryz_workbench_tools_report_t){.phase=RYZ_WB_TOOLS_UNUSED};
                job_completion_context_t failed={
                    .shell=&shell,.job=job,.next_touch=&next_touch,
                    .next_network=&next_network,.next_storage=&next_storage,
                };
                const ryz_workbench_completion_t completion={
                    .context=&failed,.restore_owner_state=resume_finished_job_ui,
                    .publish_completion=publish_finished_job,
                };
                ryz_workbench_complete_job(&completion);
                workbench_input_reset(&runtime_input);
                continue;
            }
            workbench_input_reset(&runtime_input);
            workbench_input_set_event_mode(
                &runtime_input, ryz_app_is_source(job->source, strlen(job->source)));
            /* Seed input before the worker can request it; bootstrap failure
             * is reported as an input error, never a fabricated released tap. */
            if (touch_ready) sample_runtime_input();
            next_touch = esp_timer_get_time() + SYSTEM_UI_TOUCH_INTERVAL_US;
            executing = job;
            configASSERT(xQueueSend(worker_queue, &job, 0) == pdTRUE);
        }
        int64_t now = esp_timer_get_time();
        if (boot_button_ready && now >= next_boot_button) {
            next_boot_button = now + SYSTEM_UI_BOOT_BUTTON_INTERVAL_US;
            bool pressed = false;
            if (ryz_boot_button_read(&pressed) == ESP_OK &&
                ryz_workbench_boot_key_update(
                    &boot_key, pressed, (uint32_t)(now / 1000)))
                handle_boot_button_long_press(&shell, executing);
        }
        if (now >= next_network) {
            next_network = now + SYSTEM_UI_NETWORK_INTERVAL_US;
            (void)refresh_system_network(&shell, false);
            if(shell.ui_active && ryz_system_ui_current_route()==RYZ_SYSTEM_UI_ROUTE_SCRIPT_FAULT) {
                char path[96];
                const char *link=ryz_provisioning_diagnostics_ready() &&
                    ryz_diagnostics_get_path(path,sizeof(path))==ESP_OK?path:NULL;
                if(ryz_v5_fault_link(link)) shell.render_dirty=true;
            }
        }
        if (now >= next_storage) {
            next_storage = now + SYSTEM_UI_STORAGE_INTERVAL_US;
            (void)refresh_system_storage(&shell, false);
        }
        /* Root-menu targets have stable meanings. Detail screens can change
         * a button from ON to OFF or change an operation's eligibility in the
         * just-published model. Present those changes before hit-testing; do
         * not interpret a DOWN against controls the user has not seen. */
        if (ryz_system_ui_current_route() > RYZ_SYSTEM_UI_ROUTE_SETTINGS &&
            render_system_ui(&shell)) continue;
        /* Map fresh service state first, but defer its repaint until after
         * due root-menu input. Recheck time: snapshot work may cross the deadline. */
        now = esp_timer_get_time();
        if (touch_ready && now >= next_touch) {
            ryz_touch_sample_t sample;
            const int64_t read_started_us = esp_timer_get_time();
            esp_err_t err = ryz_touch_read(&sample);
            bool consumed=ryz_display_settings_filter_touch(&sample,err,
                (uint64_t)esp_timer_get_time()/1000U);
            ui_record_timing(&ui_touch_read_timing, read_started_us);
            next_touch = esp_timer_get_time() +
                (err == ESP_OK ? SYSTEM_UI_TOUCH_INTERVAL_US :
                                 SYSTEM_UI_TOUCH_RETRY_US);
            if(consumed) {
                if(executing) workbench_input_suppress(&runtime_input, &sample, err);
                if(!display_input_filtered && shell.ui_active) {
                    ryz_system_ui_invalidate_input();
                    shell.render_dirty=true;
                }
                display_input_filtered=true;
            } else if (executing) {
                display_input_filtered=false;
                /* Exactly one physical reader; preserve DOWN/UP for the app
                 * rather than consuming them in an independent UI poll. */
                workbench_input_push(&runtime_input, &sample, err);
            } else if (shell.ui_active) {
                if(display_input_filtered) shell.render_dirty=true;
                display_input_filtered=false;
                /* Feed errors too: TP recovery may report a held finger as a
                 * new DOWN. The UI owns release gating and never produces an
                 * intent from a failed sample or failed frame transfer. */
                ryz_system_ui_action_t action = RYZ_SYSTEM_UI_ACTION_NONE;
                const uint32_t render_before = ryz_system_ui_render_revision();
                const int64_t handle_started_us = esp_timer_get_time();
                esp_err_t error = ryz_system_ui_process_sample(
                    err == ESP_OK ? &sample : NULL, err, &shell.snapshot, &action);
                ui_record_timing(&ui_touch_handle_timing, handle_started_us);
                if (error != ESP_OK) {
                    report_system_shell_error(
                        &shell, SYSTEM_SHELL_ERROR_TOUCH_HANDLER, error);
                    if (err == ESP_OK) ryz_system_ui_reset();
                    shell.render_dirty = true;
                } else {
                    /* A successful touch render already painted the freshly
                     * mapped snapshot. Do not immediately build it a second
                     * time. Action admission below may make it dirty again. */
                    if (render_before != ryz_system_ui_render_revision())
                        shell.render_dirty = false;
                    consume_system_ui_action(&shell, action);
                }
            }
            refresh_touch();
        }
        sync_apps_view(&shell);
        if (render_system_ui(&shell)) continue;
        if (shell.ui_active) {
            const int64_t tick_started_us = esp_timer_get_time();
            esp_err_t error = ryz_system_ui_tick(&shell.snapshot);
            ui_record_timing(&ui_tick_timing, tick_started_us);
            if (error != ESP_OK) {
                report_system_shell_error(&shell, SYSTEM_SHELL_ERROR_TOUCH_HANDLER, error);
                ryz_system_ui_invalidate_input();
                shell.render_dirty = true;
            }
        }
        /* Higher priority than Lua, but never spin on an empty channel. A
         * pending I/O request or completion wakes us before the next UI tick.
         * A tick-rounded 20ms wake can precede the TP deadline by a fraction
         * of a millisecond. Wait only that remainder, not another full 20ms. */
        uint32_t wait_ms = 20;
        if (touch_ready) {
            const int64_t remaining_us = next_touch - esp_timer_get_time();
            if (remaining_us <= 0) wait_ms = 1;
            else if (remaining_us < 20000)
                wait_ms = (uint32_t)((remaining_us + 999) / 1000);
        }
        if (boot_button_ready) {
            const int64_t remaining_us = next_boot_button - esp_timer_get_time();
            if (remaining_us <= 0) wait_ms = 1;
            else if (remaining_us < (int64_t)wait_ms * 1000)
                wait_ms = (uint32_t)((remaining_us + 999) / 1000);
        }
        TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks ? wait_ticks : 1);
    }
}

static void start_lock(void *unused) { (void)unused; xSemaphoreTake(job_lock, portMAX_DELAY); }
static void start_unlock(void *unused) { (void)unused; xSemaphoreGive(job_lock); }
/* The common Job admission calls ready while already holding job_lock. */
static bool start_ready(void *unused)
{ (void)unused; return atomic_load(&runtime_gateway_ready) && !file_write_active; }
static bool start_enqueue(void *unused, script_job_t *job)
{ (void)unused; return xQueueSend(job_queue, &job, 0) == pdTRUE; }
static void start_notify(void *unused) { (void)unused; xTaskNotifyGive(ui_owner); }
static int64_t start_now(void *unused) { (void)unused; return esp_timer_get_time(); }

static cJSON *start_job(const char *id, const char *name, char *source, uint32_t timeout, bool legacy)
{
    const ryz_workbench_job_start_context_t context = {
        .boot_id = boot_id, .next_job = &next_job, .current = &current,
        .lock = start_lock, .unlock = start_unlock, .ready = start_ready,
        .enqueue = start_enqueue, .notify = start_notify, .now_us = start_now,
    };
    return ryz_workbench_job_start(&context, id, name, source, timeout, legacy);
}

static void launch_system_app(void *unused, const char *name, char *source,
                              ryz_workbench_apps_launch_result_t *result)
{
    (void)unused;
    /* Nonlegacy job_start consumes source and reserves its complete response
     * before enqueue. Therefore NULL here is a known pre-admission OOM, unlike
     * an RPC transport losing an ACK after publication. No path reopens name. */
    cJSON *reply = start_job("system-apps", name, source, 0, false);
    if (!reply) {
        result->error = ESP_ERR_NO_MEM;
        snprintf(result->message, sizeof(result->message), "Cannot allocate application job");
        return;
    }
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(reply, "ok");
    if (cJSON_IsTrue(ok)) {
        const cJSON *job = cJSON_GetObjectItemCaseSensitive(reply, "job");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(job, "job_id");
        result->accepted = true;
        if (cJSON_IsString(id) && strlen(id->valuestring) < sizeof(result->job_id)) {
            strcpy(result->job_id, id->valuestring);
            snprintf(result->message, sizeof(result->message), "Application started");
        } else {
            result->uncertain = true;
            result->error = ESP_ERR_INVALID_STATE;
            snprintf(result->message, sizeof(result->message), "Application launch acknowledgement unavailable");
        }
    } else {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(reply, "error");
        result->uncertain = !cJSON_IsFalse(ok);
        result->error = ESP_ERR_INVALID_STATE;
        snprintf(result->message, sizeof(result->message), "%s", cJSON_IsString(error)
            ? error->valuestring : "Application launch unavailable");
    }
    cJSON_Delete(reply);
}

static cJSON *start_selected_script(void *unused, const char *id,
                                   const ryz_script_store_snapshot_t *snapshot)
{
    (void)unused;
    /* Store snapshot is borrowed only during this call. The job consumes an
     * independent PSRAM copy of these exact verified bytes, never the path. */
    return start_job(id, snapshot->entry.name,
        ryz_workbench_source_copy(snapshot->source, snapshot->entry.bytes), 0, false);
}

static cJSON *query_job(const char *id, const char *job_id, bool stop)
{
    xSemaphoreTake(job_lock, portMAX_DELAY);
    script_job_t *job = current && !strcmp(current->id, job_id) ? current :
        recent && !strcmp(recent->id, job_id) ? recent : NULL;
    cJSON *r = response(id, job != NULL, job ? NULL : "unknown job id");
    if (job) {
        if (stop && job->active) atomic_store(&job->cancel, true);
        cJSON_AddItemToObject(r, "job", job_json(job, !stop));
    }
    xSemaphoreGive(job_lock);
    return r;
}

static const char *help_text =
    "help\nboard info\nlua --run-async --path <file.lua>\nlua --jobs\n"
    "lua --job <job-id>\nlua --stop <job-id>\n"
    "Commands only. No Lua REPL. A valid boot.lua autostarts after the splash.";

static int cmd_help(int argc, char **argv)
{
    (void)argv;
    console_reply = response(console_id, argc == 1, argc == 1 ? NULL : "usage: help");
    cJSON_AddStringToObject(console_reply, "output", help_text);
    return argc == 1 ? 0 : 1;
}

static int cmd_board(int argc, char **argv)
{
    console_reply = argc == 2 && !strcmp(argv[1], "info") ? device_info(console_id) :
        response(console_id, false, "usage: board info");
    return 0;
}

static int cmd_lua(int argc, char **argv)
{
    int errors = arg_parse(argc, argv, (void **)&lua_args);
    int actions = lua_args.run->count + lua_args.jobs->count + lua_args.job->count + lua_args.stop->count;
    if (errors || actions != 1 || (lua_args.run->count != lua_args.path->count)) {
        console_reply = response(console_id, false, "invalid Lua command arguments; use help");
    } else if (lua_args.run->count) {
        if (busy()) {
            console_reply = response(console_id, false, ACTIVE_OWNER_ERROR);
        } else {
            console_reply = start_job(
                console_id, lua_args.path->sval[0],
                read_script(lua_args.path->sval[0]), 0, false);
        }
    } else if (lua_args.jobs->count) {
        console_reply = response(console_id, true, NULL);
        cJSON *jobs = cJSON_AddArrayToObject(console_reply, "jobs");
        xSemaphoreTake(job_lock, portMAX_DELAY);
        if (current) cJSON_AddItemToArray(jobs, job_json(current, false));
        if (recent) cJSON_AddItemToArray(jobs, job_json(recent, false));
        xSemaphoreGive(job_lock);
    } else console_reply = query_job(console_id,
        lua_args.stop->count ? lua_args.stop->sval[0] : lua_args.job->sval[0], lua_args.stop->count != 0);
    return 0;
}

static cJSON *console(const char *id, const char *command)
{
    if (!command || !command[0] || strlen(command) > 512 || strchr(command, '\n') || strchr(command, '\r')) {
        return response(id, false, "command must be a single line of 1..512 bytes");
    }
    console_id = id;
    console_reply = NULL;
    int result = 0;
    esp_err_t err = esp_console_run(command, &result);
    if (err != ESP_OK) return response(id, false, "unknown or invalid command; use help");
    return console_reply ? console_reply : response(id, false, "command produced no result");
}

static cJSON *dispatch(cJSON *request)
{
    const char *id = field(request, "id"), *op = field(request, "op");
    if (!id) id = "";
    if (!op) return response(id, false, "op must be a string");
    if (!strcmp(op, "info")) return device_info(id);
    if (!strcmp(op, "ota")) return ota_control(id, request);
    if (!strcmp(op, "tools")) return ryz_workbench_tools_rpc(request, boot_id);
    if (!strcmp(op, "network")) return ryz_workbench_network_rpc(request, boot_id);
    if (!strcmp(op, "console")) return console(id, field(request, "command"));
    if (ryz_workbench_i2c_rpc_supports(op))
        return ryz_workbench_i2c_rpc(request, boot_id);
    if (ryz_workbench_rgb_rpc_supports(op))
        return ryz_workbench_rgb_rpc(request, boot_id);
    if (ryz_workbench_monitor_rpc_supports(op))
        return ryz_workbench_monitor_rpc(request, boot_id);
    if (ryz_workbench_file_rpc_supports(op))
        return ryz_workbench_file_rpc(request, boot_id, busy(), start_selected_script, NULL, &file_write_guard);
    if (strcmp(op, "eval") && strcmp(op, "run")) return response(id, false, "unknown op");
    if (busy()) return response(id, false, ACTIVE_OWNER_ERROR);
    uint32_t timeout = 1000;
    cJSON *t = cJSON_GetObjectItemCaseSensitive(request, "timeout_ms");
    if (t) {
        if (!cJSON_IsNumber(t) || t->valuedouble < 10 || t->valuedouble > 5000) return response(id, false, "timeout_ms must be 10..5000");
        timeout = t->valueint;
    }
    if (!strcmp(op, "run")) return start_job(id, field(request, "name"), read_script(field(request, "name")), timeout, true);
    const char *source = field(request, "source");
    if (!source || !source[0] || strlen(source) > RYZ_LUA_SOURCE_MAX) return response(id, false, "source must be 1..16384 bytes");
    return start_job(id, "=serial", ryz_workbench_source_copy(source, strlen(source)), timeout, true);
}

static bool contains_json_nul(const char *text)
{
    /* cJSON exposes NUL-terminated strings, so an escaped NUL would otherwise
     * silently truncate a filename, command or uploaded source. Skip escaped
     * backslashes: a Lua source literal containing \\u0000 is not a JSON NUL. */
    for (const char *p = text; *p; ++p) {
        if (*p != '\\') continue;
        if (!strncmp(p, "\\u0000", 6)) return true;
        if (p[1]) ++p;
    }
    return false;
}

esp_err_t ryz_workbench_start_display(void)
{
    if (ui_owner) return ESP_ERR_INVALID_STATE;
    /* The SPI ISR was installed on the calling app_main core. Keep the same
     * task/core for the early splash, normal C pages and Lua display gateway. */
    if (xTaskCreatePinnedToCore(execute_task, "wb_ui", SYSTEM_UI_TASK_STACK_BYTES,
                                (void *)1, 5, &ui_owner, xPortGetCoreID()) != pdPASS)
        return ESP_ERR_NO_MEM;
    int result;
    while ((result = atomic_load_explicit(&boot_first_frame, memory_order_acquire)) == ESP_ERR_NOT_FINISHED)
        vTaskDelay(1);
    return (esp_err_t)result;
}

void ryz_workbench_prepare(void)
{
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0));
    transport_ready = true;
}

void ryz_workbench_start(const ryz_workbench_config_t *workbench_config)
{
    configASSERT(workbench_config);
    configASSERT(transport_ready);
    filesystem_ready = workbench_config->filesystem_ready;
    display_ready = workbench_config->display_ready;
    touch_ready = workbench_config->touch_ready;
    font_ready = workbench_config->font_ready;

    /* Recovery precedes every boot/get/run read. Mount failure never formats
     * storage. Ambiguous legacy backups are preserved for reconciliation. */
    if (filesystem_ready) {
        ryz_script_store_mutation_t recovery;
        esp_err_t error = ryz_script_store_init(SCRIPT_ROOT, &recovery);
        if (error != ESP_OK) ESP_LOGE(TAG, "script store recovery: %s", esp_err_to_name(error));
        if (error == ESP_OK) {
            ryz_script_store_snapshot_t boot = {0};
            error = ryz_script_store_get("boot.lua", &boot);
            if (error == ESP_OK) {
                boot_autostart_source = ryz_workbench_source_copy(boot.source, boot.entry.bytes);
                if (!boot_autostart_source)
                    ESP_LOGE(TAG, "boot.lua autostart preparation: out of memory");
                ryz_script_store_snapshot_free(&boot);
            } else if (error != ESP_ERR_NOT_FOUND) {
                ESP_LOGE(TAG, "boot.lua autostart unavailable: %s", esp_err_to_name(error));
            }
        }
    }

    /* UART0 console pins are kept as initialized by IDF. No Yoke pins are driven. */
    boot_emit(boot_info("boot-info"));
    if (display_ready && workbench_config->display_boot_source) {
        boot_emit(boot_execute("boot-display", workbench_config->display_boot_source,
                               "=display_boot", 1000));
    }
    if (display_ready && touch_ready && workbench_config->touch_boot_source) {
        boot_emit(boot_execute("boot-touch", workbench_config->touch_boot_source,
                               "=touch_boot", 1000));
    }
    /* Readonly opt-in diagnostics stay separate. Mutable user boot.lua is
     * admitted later by the normal Job owner, after the splash and health
     * gate, so BOOT cancellation and HOME restoration use the common path. */
    if (workbench_config->system_boot_source)
        boot_emit(boot_execute("boot-script", workbench_config->system_boot_source,
                               "=system_boot", 1000));
    puts("RYZOBEE_READY");
    fflush(stdout);

    snprintf(boot_id, sizeof(boot_id), "%08lx", (unsigned long)esp_random());
    configASSERT(ryz_workbench_apps_init(boot_id) == ESP_OK);
    configASSERT(ryz_workbench_scripts_init() == ESP_OK);
    const ryz_v5_scripts_binding_t scripts_binding = {.submit = scripts_ui_submit};
    ryz_v5_scripts_bind(&scripts_binding);
    const ryz_v5_display_binding_t display_binding={.get=display_ui_get,.submit=display_ui_submit,
        .preview=display_ui_preview,.end_preview=display_ui_end_preview};
    ryz_v5_display_bind(&display_binding);
    const ryz_v5_apps_binding_t apps_binding = {
        .get = apps_ui_snapshot, .submit = apps_ui_submit,
    };
    ryz_v5_apps_bind(&apps_binding);
    const ryz_v5_password_binding_t password_binding={
        .inspect=password_ui_inspect,.copy=password_ui_copy,
    };
    ryz_v5_password_bind(&password_binding);
    /* Expose a stable, opaque board identity without leaking the base MAC. */
    static const char domain[] = "ryzobee-device-id/v1";
    unsigned char identity_material[sizeof(domain) - 1 + 6];
    memcpy(identity_material, domain, sizeof(domain) - 1);
    if (esp_efuse_mac_get_default(identity_material + sizeof(domain) - 1) == ESP_OK) {
        digest((const char *)identity_material, sizeof(identity_material), device_id);
    }
    memset(identity_material, 0, sizeof(identity_material));
    job_lock = xSemaphoreCreateMutex();
    control_queue = xQueueCreate(8, sizeof(packet_t));
    output_queue = xQueueCreate(16, sizeof(output_t));
    job_queue = xQueueCreate(1, sizeof(script_job_t *));
    worker_queue = xQueueCreate(1, sizeof(script_job_t *));
    completion_queue = xQueueCreate(1, sizeof(script_job_t *));
    configASSERT(job_lock && control_queue && output_queue && job_queue &&
                 worker_queue && completion_queue);
    atomic_init(&runtime_gateway_ready, false);
    ryz_workbench_io_init(&runtime_channel);
    workbench_input_reset(&runtime_input);
    esp_err_t boot_button_status = ryz_boot_button_init();
    boot_button_ready = boot_button_status == ESP_OK;
    if (boot_button_status != ESP_OK)
        ESP_LOGW(TAG, "runtime BOOT key unavailable: %s",
                 esp_err_to_name(boot_button_status));
    refresh_touch();
    configASSERT(xTaskCreate(transmit_task, "wb_tx", 6144, NULL, 6, &transmitter) == pdPASS);
    /* Generic peripheral handles/interrupts are installed and torn down by
     * this single CPU1 owner. The display/SPI2 owner remains on CPU0. */
    configASSERT(xTaskCreatePinnedToCore(lua_worker_task, "wb_lua", 32768, NULL, 4,
                                        &lua_worker, 1) == pdPASS);
    esp_console_config_t config = ESP_CONSOLE_CONFIG_DEFAULT();
    config.max_cmdline_length = 512;
    config.max_cmdline_args = 8;
    ESP_ERROR_CHECK(esp_console_init(&config));
    lua_args.run = arg_lit0(NULL, "run-async", "Run a script task");
    lua_args.jobs = arg_lit0(NULL, "jobs", "List jobs");
    lua_args.path = arg_str0(NULL, "path", "file.lua", "Script filename");
    lua_args.job = arg_str0(NULL, "job", "id", "Query task");
    lua_args.stop = arg_str0(NULL, "stop", "id", "Request cancellation");
    lua_args.end = arg_end(8);
    configASSERT(!arg_nullcheck((void **)&lua_args));
    const esp_console_cmd_t commands[] = {
        { .command = "help", .help = "List commands", .func = cmd_help },
        { .command = "board", .help = "board info", .func = cmd_board },
        { .command = "lua", .help = "Lua script jobs (not a REPL)", .func = cmd_lua, .argtable = &lua_args },
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) ESP_ERROR_CHECK(esp_console_cmd_register(&commands[i]));
    /* CPU-only, boot-lifetime text storage. UART still reads into the internal
     * staging buffer below; keep these 24 KiB available to Wi-Fi/controller. */
    char *line = heap_caps_malloc(RX_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    configASSERT(line);
    /* Diagnostic builds (explicit legacy Lua demos) and early-task allocation
     * failure reach this fallback; normal boot retains its already-live Owner. */
    if (!ui_owner) {
        configASSERT(xTaskCreatePinnedToCore(execute_task, "wb_ui", SYSTEM_UI_TASK_STACK_BYTES,
                                            NULL, 5, &ui_owner, xPortGetCoreID()) == pdPASS);
    }
    atomic_store_explicit(&boot_setup_complete, true, memory_order_release);
    size_t used = 0; bool overflow = false;
    uint8_t buffer[256];
    /* RX must preempt the Lua worker when input arrives; a print-heavy script
     * may spend a long time in native code between instruction hooks. */
    vTaskPrioritySet(NULL, 5);
    for (;;) {
        /* No incoming traffic is required: uart_read_bytes has a 20ms timeout.
         * This task, not UI Owner or a Lua idle hook, retires old tool sessions. */
        ryz_workbench_tools_tick(esp_timer_get_time());
        /* Source loading belongs to RX; the UI Owner only consumes prepared
         * immutable bytes and makes the final page/selection/job admission. */
        ryz_workbench_apps_rx_tick(busy(), &file_write_guard);
        ryz_workbench_scripts_rx_tick(&file_write_guard);
        int count = uart_read_bytes(UART_NUM_0, buffer, sizeof(buffer), pdMS_TO_TICKS(20));
        for (int i = 0; i < count; ++i) {
            unsigned char ch = buffer[i];
            if (ch == '\r') ch = '\n'; /* Accept CR, LF and CRLF terminals. */
            if (ch == 8 || ch == 127) { if (used && !overflow) --used; continue; }
            if (ch != '\n') {
                if (!ch) overflow = true;
                else if (used + 1 < RX_MAX && !overflow) line[used++] = ch;
                else overflow = true;
                continue;
            }
            line[used] = 0;
            if (overflow) send_json("RYZOBEE_RPC ", response("", false, "request line too long or contains NUL"));
            else if (used) {
                char *first = line;
                while (isspace((unsigned char)*first)) ++first;
                if (*first == '{') {
                    cJSON *request = cJSON_ParseWithOpts(first, NULL, true);
                    cJSON *r = !cJSON_IsObject(request) ? response("", false, "invalid JSON object") :
                        contains_json_nul(first) ? response(field(request, "id"), false, "JSON strings must not contain NUL") : dispatch(request);
                    if (r) send_json("RYZOBEE_RPC ", r);
                    cJSON_Delete(request);
                } else {
                    cJSON *r = console("terminal", first);
                    const char *message = field(r, "output");
                    if (!message) message = field(r, "error");
                    if (message) {
                        packet_t packet = { .prefix = "", .text = strdup(message) };
                        if (packet.text && xQueueSend(control_queue, &packet, 0) != pdTRUE) free(packet.text);
                        xTaskNotifyGive(transmitter);
                    }
                    send_json("RYZOBEE_RPC ", r);
                }
            }
            used = 0; overflow = false;
        }
        ryz_workbench_tools_tick(esp_timer_get_time());
    }
}
