#pragma once
#include <stdint.h>

/* SDK 5.5.4 public VHCI callback shape; this test never sends radio traffic. */
typedef struct {
    void (*notify_host_send_available)(void);
    int (*notify_host_recv)(uint8_t *, uint16_t);
} esp_vhci_host_callback_t;
