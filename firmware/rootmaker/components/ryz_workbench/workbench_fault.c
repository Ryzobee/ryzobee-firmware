#include "workbench_fault.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int32_t source_line(const char *error,const char *name)
{
    if(!error || !name || !name[0]) return -1;
    char prefix[64];
    snprintf(prefix,sizeof(prefix),"%s:",name);
    const char *number=NULL;
    if(!strncmp(error,prefix,strlen(prefix))) number=error+strlen(prefix);
    else {
        snprintf(prefix,sizeof(prefix),"[string \"%s\"]:",name);
        if(!strncmp(error,prefix,strlen(prefix))) number=error+strlen(prefix);
    }
    if(!number || *number<'1' || *number>'9') return -1;
    uint32_t value=0;
    while(*number>='0' && *number<='9') {
        unsigned digit=(unsigned)(*number++-'0');
        if(value>((uint32_t)INT32_MAX-digit)/10U) return -1;
        value=value*10U+digit;
    }
    return *number==':'?(int32_t)value:-1;
}

static void mask_range(char *out,size_t start,size_t kept,size_t at,size_t count)
{
    size_t first=at>start?at:start, end=at+count;
    if(end>start+kept) end=start+kept;
    if(end>first) memset(out+first-start,'*',end-first);
}

static void copy_redacted(char *out,size_t capacity,const char *input,size_t length,
                           bool tail,const char *secret)
{
    size_t keep=length<capacity-1?length:capacity-1;
    size_t start=tail?length-keep:0;
    if(keep) memcpy(out,input+start,keep);
    out[keep]=0;
    if(!secret || !secret[0] || !length) return;
    size_t n=strlen(secret);
    /* Match before selecting the output window, including a secret straddling
     * its left boundary. A bounded upstream capture can also split either
     * end of a credential; conservatively mask matching boundary fragments. */
    for(size_t at=0;at<length;++at) {
        size_t available=length-at;
        size_t match=available<n?available:n;
        if(!memcmp(input+at,secret,match)) mask_range(out,start,keep,at,match);
    }
    for(size_t count=1;count<n && count<=length;++count)
        if(!memcmp(input,secret+n-count,count)) mask_range(out,start,keep,0,count);
}

bool ryz_workbench_fault_prepare(ryz_workbench_fault_t *out,const ryz_lua_result_t *r,
    const char *name,const char *id,const char *version,int cleanup_ok,
    const char *output,size_t length,bool truncated,const char *secret)
{
    if(!out || !r || !name || !id || !version || (!output && length)) return false;
    memset(out,0,sizeof(*out));
    const char *phase=r->phase?r->phase:"unknown";
    if(!strcmp(phase,"stopped") || (r->ok && cleanup_ok==1)) return false;
    const char *code="LUA ERROR", *summary="Script execution failed";
    if(r->ok) { code="CLEANUP"; summary="Resource cleanup incomplete"; }
    else if(!strcmp(phase,"runtime")) { code="LUA-R01"; summary="Runtime error"; }
    else if(!strcmp(phase,"syntax")) { code="LUA-S01"; summary="Syntax error"; }
    else if(!strcmp(phase,"timeout")) { code="LUA-T01"; summary="Execution timed out"; }
    else if(!strcmp(phase,"memory")) summary="Lua memory limit reached";
    else if(!strcmp(phase,"prepare")) summary="Script preparation failed";
    else if(!strcmp(phase,"init")) summary="Script initialization failed";
    snprintf(out->screen.code,sizeof(out->screen.code),"%s",code);
    snprintf(out->screen.summary,sizeof(out->screen.summary),"%s",summary);
    snprintf(out->screen.source_file,sizeof(out->screen.source_file),"%.40s",name);
    snprintf(out->error,sizeof(out->error),"%.*s",(int)sizeof(r->error)-1,r->error);
    out->screen.line=source_line(out->error,name);
    copy_redacted(out->error,sizeof(out->error),r->error,strnlen(r->error,sizeof(r->error)),false,secret);
    copy_redacted(out->output,sizeof(out->output),output,length,true,secret);
    out->report=(ryz_diagnostics_input_t){.phase=r->ok?"cleanup":phase,
        .code=out->screen.code,.summary=out->screen.summary,.source_file=out->screen.source_file,
        .job_id=id,.firmware_version=version,.error=out->error,.output=out->output,
        .line=out->screen.line,.duration_ms=!strcmp(phase,"prepare")?-1:(int64_t)r->elapsed_ms,
        .peak_bytes=!strcmp(phase,"prepare")?-1:(int64_t)r->peak_bytes,
        .lua_ok=r->ok,.ok=r->ok && cleanup_ok==1,.cleanup_ok=cleanup_ok,
        .error_truncated=strnlen(r->error,sizeof(r->error))>=sizeof(r->error)-1,
        .output_truncated=truncated || length>512};
    return true;
}
