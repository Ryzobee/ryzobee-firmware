#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ryz_captive_dns.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

struct task { void (*entry)(void *); void *argument; bool deleted; unsigned notifications; };
struct semaphore { bool ready; };
static struct task task;
static bool allow_ack;
static bool close_failure;
static bool bind_failure;
static bool task_failure;
static bool self_call;
static unsigned close_calls;
static unsigned socket_live;
static unsigned semaphore_live;
static unsigned shutdown_calls;

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    ++semaphore_live;
    return calloc(1, sizeof(struct semaphore));
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    assert(timeout != portMAX_DELAY); /* A worker stop must not wait forever. */
    if (!semaphore->ready && allow_ack && !task.deleted) task.entry(task.argument);
    if (!semaphore->ready) return pdFALSE;
    semaphore->ready = false;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) { semaphore->ready = true; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t semaphore) { --semaphore_live; free(semaphore); }
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned stack, void *argument,
                       unsigned priority, TaskHandle_t *out)
{
    (void)name; (void)stack; (void)priority;
    if (task_failure) return pdFALSE;
    task = (struct task){.entry = entry, .argument = argument};
    *out = &task;
    return pdPASS;
}
unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    (void)clear; (void)timeout;
    unsigned result = task.notifications;
    task.notifications = 0;
    return result;
}
void xTaskNotifyGive(TaskHandle_t handle) { assert(!handle->deleted); ++handle->notifications; }
void vTaskDelete(TaskHandle_t handle) { assert(handle == NULL); task.deleted = true; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return self_call ? &task : NULL; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info)
{ (void)netif; info->ip.addr = 0x0104a8c0U; return ESP_OK; }
int dns_test_socket(int family, int type, int protocol)
{ (void)family; (void)type; (void)protocol; ++socket_live; return 19; }
int dns_test_setsockopt(int fd, int level, int option, const void *value, socklen_t length)
{ (void)fd; (void)level; (void)option; (void)value; (void)length; return 0; }
int dns_test_bind(int fd, const struct sockaddr *address, socklen_t length)
{ (void)fd; (void)address; (void)length; return bind_failure ? -1 : 0; }
ssize_t dns_test_recvfrom(int fd, void *bytes, size_t length, int flags,
                          struct sockaddr *address, socklen_t *size)
{ (void)fd; (void)bytes; (void)length; (void)flags; (void)address; (void)size; abort(); }
ssize_t dns_test_sendto(int fd, const void *bytes, size_t length, int flags,
                        const struct sockaddr *address, socklen_t size)
{ (void)fd; (void)bytes; (void)length; (void)flags; (void)address; (void)size; abort(); }
int dns_test_shutdown(int fd, int how)
{ (void)fd; (void)how; ++shutdown_calls; errno = EOPNOTSUPP; return -1; }
int dns_test_close(int fd)
{
    assert(fd == 19 && socket_live == 1); ++close_calls;
    if (close_failure) { errno = EIO; return -1; }
    --socket_live;
    return 0;
}

static ryz_captive_dns_handle_t start(void)
{
    static esp_netif_t netif;
    ryz_captive_dns_handle_t handle = NULL;
    assert(ryz_captive_dns_start(&netif, &handle) == ESP_OK && handle != NULL);
    return handle;
}
static void assert_released(void) { assert(socket_live == 0 && semaphore_live == 0); }
static void timeout_test(void)
{
    ryz_captive_dns_handle_t handle = start();
    assert(ryz_captive_dns_stop(handle) == ESP_ERR_TIMEOUT);
    assert(close_calls == 0 && socket_live == 1 && semaphore_live == 1);
    allow_ack = true;
    assert(ryz_captive_dns_stop(handle) == ESP_OK);
    assert_released();
    assert(shutdown_calls == 1);
}
static void close_test(void)
{
    ryz_captive_dns_handle_t handle = start();
    allow_ack = true;
    close_failure = true;
    assert(ryz_captive_dns_stop(handle) == ESP_FAIL);
    assert(task.deleted && socket_live == 1 && semaphore_live == 1);
    close_failure = false;
    assert(ryz_captive_dns_stop(handle) == ESP_OK);
    assert(close_calls == 2 && shutdown_calls == 1);
    assert_released();
}
static void self_test(void)
{
    assert(ryz_captive_dns_stop(NULL) == ESP_OK);
    ryz_captive_dns_handle_t handle = start();
    self_call = true;
    assert(ryz_captive_dns_stop(handle) == ESP_ERR_INVALID_STATE);
    assert(shutdown_calls == 0 && close_calls == 0 && task.notifications == 0);
    self_call = false;
    allow_ack = true;
    assert(ryz_captive_dns_stop(handle) == ESP_OK);
    assert_released();
}
static void failed_start_test(bool fail_task)
{
    static esp_netif_t netif;
    ryz_captive_dns_handle_t handle = NULL;
    task_failure = fail_task;
    bind_failure = !fail_task;
    close_failure = true;
    assert(ryz_captive_dns_start(&netif, &handle) != ESP_OK);
    assert(handle != NULL && socket_live == 1 && semaphore_live == 1);
    close_failure = false;
    assert(ryz_captive_dns_stop(handle) == ESP_OK);
    assert(shutdown_calls == 0 && close_calls == 2);
    assert_released();
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "timeout") == 0) timeout_test();
    else if (strcmp(argv[1], "close") == 0) close_test();
    else if (strcmp(argv[1], "self") == 0) self_test();
    else if (strcmp(argv[1], "bind_failure") == 0) failed_start_test(false);
    else if (strcmp(argv[1], "task_failure") == 0) failed_start_test(true);
    else abort();
    puts("CAPTIVE_DNS_PASS");
    return 0;
}
