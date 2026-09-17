#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Portable value contract shared by native C and the application facade.
 * Error fields retain native diagnostic integers; no driver handles or SDK
 * dependency. The native Adapter interprets their numeric error namespace. */
typedef struct { uint8_t red, green, blue; } ryz_rgb_color_t;
typedef enum {
    RYZ_RGB_IDLE = 0,
    RYZ_RGB_QUEUED,
    RYZ_RGB_SENDING,
    RYZ_RGB_COMPLETED,
    RYZ_RGB_FAILED,
    RYZ_RGB_CLEANING,
    RYZ_RGB_CLEANED,
} ryz_rgb_phase_t;

typedef struct {
    uint32_t request_id;
    ryz_rgb_phase_t phase;
    ryz_rgb_color_t requested;
    int error;
    /* Last worker observation, including retained channel/encoder allocations.
     * In active phases, false is not proof that an in-flight call owns nothing.
     * Cleanup errors are separate from the original color-write error. */
    bool resources_held;
    int cleanup_error;
    int64_t queued_ms, finished_ms;
    /* Completion is the driver's complete frame + successful quiesce, NOT
     * optical feedback or a Test Hub PASS. Historical completion survives a
     * later failure; output_known is revoked when a new send begins. */
    bool output_known;
    uint32_t last_completed_id;
    ryz_rgb_color_t last_completed_color;
} ryz_rgb_snapshot_t;
