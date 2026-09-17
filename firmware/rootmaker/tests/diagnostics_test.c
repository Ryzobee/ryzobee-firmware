#include "ryz_diagnostics.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static _Atomic int64_t clock_us;
static _Atomic unsigned random_calls;
int64_t ryz_diagnostics_test_time(void) { return clock_us; }
void ryz_diagnostics_test_random(void *out, size_t length)
{
    unsigned call = atomic_fetch_add(&random_calls, 1);
    for (size_t i = 0; i < length; ++i) ((unsigned char *)out)[i] = (unsigned char)(i + call);
}

static ryz_diagnostics_input_t input = {.phase="runtime", .code="LUA-R01", .summary="Runtime error",
    .source_file="sensor.lua", .job_id="17", .firmware_version="SAMPLE", .error="sensor.lua:42: test \"error\"",
    .output="[SAMPLE] captured output", .line=42, .duration_ms=1284, .peak_bytes=81920,
    .lua_ok=false, .ok=false, .cleanup_ok=1};

static void path_parts(char path[64], char id[9], char cap[33])
{
    assert(ryz_diagnostics_get_path(path,64)==ESP_OK);
    assert(sscanf(path,"/f/%8[0-9a-f]?cap=%32[0-9a-f]",id,cap)==2);
}

static void *writer(void *unused)
{
    (void)unused;
    for (unsigned i=0; i<1000; ++i) assert(ryz_diagnostics_publish(&input)==ESP_OK);
    return NULL;
}

int main(void)
{
    char path[64], id[9], cap[33]; ryz_diagnostics_snapshot_t snapshot;
    ryz_diagnostics_clear();
    assert(ryz_diagnostics_get_path(path,sizeof(path))==ESP_ERR_NOT_FOUND);
    assert(ryz_diagnostics_publish(NULL)==ESP_ERR_INVALID_ARG);
    assert(ryz_diagnostics_publish(&input)==ESP_OK); path_parts(path,id,cap);
    assert(strlen(path)==48 && strlen(cap)==32);
    assert(ryz_diagnostics_snapshot(id,cap,&snapshot)==ESP_OK);
    assert(!strcmp(snapshot.source_file,input.source_file));
    assert(!strcmp(snapshot.error,input.error) && !strcmp(snapshot.output,input.output));
    assert(snapshot.line==42 && snapshot.cleanup_ok==1 && snapshot.duration_ms==1284);
    assert(!snapshot.error_truncated && !snapshot.output_truncated);
    cap[0]=cap[0]=='0'?'1':'0'; assert(ryz_diagnostics_snapshot(id,cap,&snapshot)==ESP_ERR_NOT_FOUND);
    path_parts(path,id,cap);
    assert(ryz_diagnostics_snapshot("00000000",cap,&snapshot)==ESP_ERR_NOT_FOUND);
    assert(ryz_diagnostics_publish(&input)==ESP_OK);
    assert(ryz_diagnostics_snapshot(id,cap,&snapshot)==ESP_ERR_NOT_FOUND);
    path_parts(path,id,cap);clock_us=599999999;
    assert(ryz_diagnostics_snapshot(id,cap,&snapshot)==ESP_OK);
    clock_us=600000000;assert(ryz_diagnostics_snapshot(id,cap,&snapshot)==ESP_ERR_NOT_FOUND);
    assert(ryz_diagnostics_get_path(path,sizeof(path))==ESP_ERR_NOT_FOUND);
    char huge[1000]; memset(huge,'x',sizeof(huge)-1);huge[999]=0;
    ryz_diagnostics_input_t long_input=input;long_input.error=huge;long_input.output=huge;
    long_input.line=-1;long_input.duration_ms=-1;long_input.peak_bytes=-1;long_input.cleanup_ok=-1;
    assert(ryz_diagnostics_publish(&long_input)==ESP_OK);path_parts(path,id,cap);
    assert(ryz_diagnostics_snapshot(id,cap,&snapshot)==ESP_OK);
    assert(strlen(snapshot.error)==255 && strlen(snapshot.output)==512);
    assert(snapshot.error_truncated && snapshot.output_truncated && snapshot.cleanup_ok==-1);
    assert(snapshot.line==-1 && snapshot.duration_ms==-1 && snapshot.peak_bytes==-1);
    memset(huge,'y',900); /* Published report owns its data. */
    assert(ryz_diagnostics_snapshot(id,cap,&snapshot)==ESP_OK && snapshot.output[0]=='x');
    char tiny[2]={'x',0};assert(ryz_diagnostics_get_path(tiny,sizeof(tiny))==ESP_ERR_INVALID_SIZE && !tiny[0]);
    pthread_t thread;assert(pthread_create(&thread,NULL,writer,NULL)==0);
    for(unsigned i=0;i<1000;++i){path_parts(path,id,cap);esp_err_t e=ryz_diagnostics_snapshot(id,cap,&snapshot);assert(e==ESP_OK||e==ESP_ERR_NOT_FOUND);if(e==ESP_OK){assert(!strcmp(snapshot.id,id));assert(!strcmp(snapshot.error,input.error)||strlen(snapshot.error)==255);}}
    assert(pthread_join(thread,NULL)==0);ryz_diagnostics_clear();
    assert(ryz_diagnostics_snapshot(id,cap,&snapshot)==ESP_ERR_NOT_FOUND);
    long_input.error="valid 中文 \xff\xc0 end";long_input.output="ok";
    assert(ryz_diagnostics_publish(&long_input)==ESP_OK);path_parts(path,id,cap);
    assert(ryz_diagnostics_snapshot(id,cap,&snapshot)==ESP_OK && snapshot.text_sanitized);
    assert(!strcmp(snapshot.error,"valid 中文 ?? end"));
    puts("DIAGNOSTICS_PASS identity capability replacement ttl bounded unknown immutable concurrency clear");
    return 0;
}
