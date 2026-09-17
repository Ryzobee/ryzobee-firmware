#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ryz_ota.h"
#include "ryz_v5_version.h"

/* Owner-only pure formatting of trusted C observations. No SDK/Flash access,
 * allocation, retained pointers, or inference of health/update availability.
 * NULL info revokes slot/A-B, independently of the observed chip identity.
 * Only slot, chip, ab_slots change; install history and source policy do not. */
void ryz_workbench_version_hardware(ryz_v5_version_model_t *view,
                                   const ryz_ota_running_info_t *info,
                                   bool is_esp32s3, uint16_t chip_revision);
