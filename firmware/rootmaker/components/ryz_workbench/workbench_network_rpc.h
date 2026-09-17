#pragma once
#include "cJSON.h"

/* Strict ryz-network/1, copy-only status and asynchronous network commands.
 * Full ACK allocation precedes submission; NULL is an unknown/no reply, never
 * automatic retry permission. Transport rejects embedded NUL before dispatch.
 * setup opens the AP portal without erasing saved Wi-Fi; it is not an ON alias.
 * No password serialization; mutation requires current boot. Reprovision also
 * requires confirm=true because it erases the stored STA record. */
cJSON *ryz_workbench_network_rpc(const cJSON *request, const char *boot_id);
