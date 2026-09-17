#pragma once
#include "lua_runtime.h"
#include "ryz_diagnostics.h"
#include "ryz_v5_fault.h"
typedef struct {
    ryz_v5_fault_model_t screen;
    ryz_diagnostics_input_t report;
    char error[256], output[513];
} ryz_workbench_fault_t;
/* Pure bounded completion mapping. All report pointers stay borrowed until
 * immediate diagnostics_publish; never queue this pointer-bearing struct. */
bool ryz_workbench_fault_prepare(ryz_workbench_fault_t *out,const ryz_lua_result_t *result,
    const char *name,const char *job_id,const char *version,int cleanup_ok,
    const char *captured_output,size_t length,bool output_truncated,const char *known_secret);
