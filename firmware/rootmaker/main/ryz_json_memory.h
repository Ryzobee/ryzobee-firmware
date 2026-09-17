#pragma once

/* Bootstrap only: call before creating application tasks or any JSON object.
 * The allocator stays fixed for the entire boot; never switch per request. */
void ryz_json_memory_init(void);
