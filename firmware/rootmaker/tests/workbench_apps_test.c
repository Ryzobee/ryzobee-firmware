#define main apps_test_main
#include "apps_test.c"
#undef main

#include <stdatomic.h>
#include "workbench_apps.h"
#include "workbench_job_start.h"

/* Real Apps pthread/Store/metadata/RPC/Controller/job_start. This test adapts
 * only OS scheduling, the malloc boundary, and shared writer/Job admission;
 * it does not render pixels, run Lua or claim a FreeRTOS/device result. */
static const char wb_boot[] = "apps-boot";
static const char source_a[] = "-- @author: Selected A\nprint('SELECTED_A')";
static const char source_b[] = "-- @author: Replaced B\nerror('MUST_NOT_RUN_B')";
static ryz_ui_nav_token_t shown_page = {.generation=1,.route=2};
static pthread_mutex_t allocation_lock = PTHREAD_MUTEX_INITIALIZER;
static void *source_allocations[16];
static unsigned source_live;
static bool fail_source_allocation;
#ifdef WB_TEST_EXTERNAL_SOURCE
#include "esp_heap_caps.h"
static unsigned external_source_attempts;
static size_t external_source_bytes;
static bool fail_external_source;
void *wb_apps_test_malloc(size_t bytes);
void wb_apps_test_free(void *pointer);

void *heap_caps_malloc(size_t bytes, uint32_t caps)
{
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    ++external_source_attempts;
    external_source_bytes=bytes;
    if (fail_external_source) return NULL;
    return wb_apps_test_malloc(bytes);
}

void *heap_caps_calloc(size_t count, size_t bytes, uint32_t caps)
{
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    return calloc(count,bytes);
}

void heap_caps_free(void *pointer) { wb_apps_test_free(pointer); }
#endif

void *wb_apps_test_malloc(size_t bytes)
{
    assert(pthread_mutex_lock(&allocation_lock)==0);
    if (fail_source_allocation) {
        assert(pthread_mutex_unlock(&allocation_lock)==0);
        return NULL;
    }
    void *pointer=malloc(bytes);
    if (pointer) {
        unsigned index=0;
        while (index<16 && source_allocations[index]) ++index;
        assert(index<16);
        source_allocations[index]=pointer;
        ++source_live;
    }
    assert(pthread_mutex_unlock(&allocation_lock)==0);
    return pointer;
}

void wb_apps_test_free(void *pointer)
{
    assert(pthread_mutex_lock(&allocation_lock)==0);
    for (unsigned index=0;index<16;++index) {
        if (pointer && source_allocations[index]==pointer) {
            source_allocations[index]=NULL;
            assert(source_live>0);
            --source_live;
            break;
        }
    }
    assert(pthread_mutex_unlock(&allocation_lock)==0);
    free(pointer);
}

static unsigned allocated_sources(void)
{
    assert(pthread_mutex_lock(&allocation_lock)==0);
    unsigned count=source_live;
    assert(pthread_mutex_unlock(&allocation_lock)==0);
    return count;
}

typedef struct {
    pthread_mutex_t mutex;
    bool locked, ready, queue_ready, uncertain;
    bool write_allowed, write_active;
    unsigned next_job, callbacks, enqueues, notifications;
    unsigned write_begins, write_accepts, write_ends;
    script_job_t *current, *queued;
    ryz_workbench_job_start_context_t start;
} wb_launch_t;
static wb_launch_t launcher;

static void wb_job_lock(void *context)
{
    wb_launch_t *value=context;
    assert(pthread_mutex_lock(&value->mutex)==0);
    assert(!value->locked);
    value->locked=true;
}
static void wb_job_unlock(void *context)
{
    wb_launch_t *value=context;
    assert(value->locked);
    value->locked=false;
    assert(pthread_mutex_unlock(&value->mutex)==0);
}
static bool wb_job_ready(void *context)
{
    wb_launch_t *value=context;
    assert(value->locked);
    return value->ready && !value->write_active;
}
static bool wb_job_enqueue(void *context, script_job_t *job)
{
    wb_launch_t *value=context;
    assert(value->locked && value->current==job && !value->queued);
    if (!value->queue_ready) return false;
    value->queued=job;
    ++value->enqueues;
    return true;
}
static void wb_job_notify(void *context)
{
    wb_launch_t *value=context;
    assert(!value->locked && value->queued);
    ++value->notifications;
}
static int64_t wb_job_now(void *context) { assert(context); return INT64_C(123456789); }

static bool wb_write_begin(void *context)
{
    wb_launch_t *value=context;
    assert(value==&launcher && lock_depth==0);
    wb_job_lock(value);
    ++value->write_begins;
    bool accepted=value->write_allowed && !value->current && !value->write_active;
    if (accepted) {
        value->write_active=true;
        ++value->write_accepts;
    }
    wb_job_unlock(value);
    return accepted;
}

static void wb_write_end(void *context)
{
    wb_launch_t *value=context;
    assert(value==&launcher && lock_depth==0);
    wb_job_lock(value);
    assert(value->write_active && value->write_accepts==value->write_ends+1U);
    value->write_active=false;
    ++value->write_ends;
    wb_job_unlock(value);
}

static const ryz_workbench_write_guard_t write_guard={
    .context=&launcher,.try_begin_write=wb_write_begin,.end_write=wb_write_end};

static void rx_tick(bool runtime_busy)
{ ryz_workbench_apps_rx_tick(runtime_busy,&write_guard); }

static void assert_write_released(unsigned begins,unsigned accepted)
{
    wb_job_lock(&launcher);
    assert(!launcher.write_active && launcher.write_begins==begins);
    assert(launcher.write_accepts==accepted && launcher.write_ends==accepted);
    wb_job_unlock(&launcher);
}

static void wb_launch(void *context,const char *name,char *source,
                      ryz_workbench_apps_launch_result_t *result)
{
    wb_launch_t *value=context;
    ++value->callbacks;
    assert(name && source && !value->locked && lock_depth==0);
    memset(result,0,sizeof(*result));
    if (value->uncertain) {
        wb_apps_test_free(source);
        result->uncertain=true;
        result->error=ESP_FAIL;
        strcpy(result->message,"launch acknowledgement unknown");
        return;
    }
    cJSON *reply=ryz_workbench_job_start(&value->start,"apps-run",name,source,0,false);
    if (!reply) {
        result->uncertain=true;
        result->error=ESP_ERR_NO_MEM;
        return;
    }
    result->accepted=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply,"ok"));
    if (result->accepted) {
        const cJSON *job=cJSON_GetObjectItemCaseSensitive(reply,"job");
        const cJSON *id=cJSON_GetObjectItemCaseSensitive(job,"job_id");
        assert(cJSON_IsString(id) && strlen(id->valuestring)<sizeof(result->job_id));
        strcpy(result->job_id,id->valuestring);
    } else {
        result->error=ESP_ERR_INVALID_STATE;
        const cJSON *error=cJSON_GetObjectItemCaseSensitive(reply,"error");
        assert(cJSON_IsString(error));
        snprintf(result->message,sizeof(result->message),"%s",error->valuestring);
    }
    cJSON_Delete(reply);
}

static void wb_setup(void)
{
    initialize_store();
    assert(pthread_mutex_init(&launcher.mutex,NULL)==0);
    launcher.ready=launcher.queue_ready=launcher.write_allowed=true;
    launcher.start=(ryz_workbench_job_start_context_t){.context=&launcher,
        .boot_id=wb_boot,.next_job=&launcher.next_job,.current=&launcher.current,
        .lock=wb_job_lock,.unlock=wb_job_unlock,.ready=wb_job_ready,
        .enqueue=wb_job_enqueue,.notify=wb_job_notify,.now_us=wb_job_now};
    assert(ryz_workbench_apps_init(wb_boot)==ESP_OK);
    start_apps();
}

static ryz_workbench_apps_snapshot_t wb_snapshot(void)
{
    ryz_workbench_apps_snapshot_t value;
    memset(&value,0xa5,sizeof(value));
    size_t calls=script_store_host_platform_calls();
    assert(ryz_workbench_apps_get_snapshot(&value)==ESP_OK);
    assert(script_store_host_platform_calls()==calls);
    return value;
}

static void owner_tick(bool active)
{
    size_t calls=script_store_host_platform_calls();
    unsigned callbacks=launcher.callbacks;
    ryz_workbench_apps_owner_tick(active,shown_page,wb_launch,&launcher);
    /* Actual job_start hashes owned memory once; all other Store calls are
     * forbidden here. Source hashing does not read or lock the filesystem. */
    size_t expected=launcher.callbacks!=callbacks && !launcher.uncertain ? 1U : 0U;
    assert(script_store_host_platform_calls()==calls+expected);
}

static ryz_workbench_apps_snapshot_t wb_settle(ryz_apps_view_t view,ryz_apps_state_t state)
{
    for (unsigned index=0;index<10;++index) {
        owner_tick(true);
        ryz_workbench_apps_snapshot_t value=wb_snapshot();
        if (value.open && value.reader.view==view && value.reader.state==state &&
            value.reader.request_id && value.reader.completed_id==value.reader.request_id)
            return value;
        step();
    }
    ryz_workbench_apps_snapshot_t value=wb_snapshot();
    fprintf(stderr,"controller view=%d state=%d request=%llu completed=%llu error=%d\n",
        value.reader.view,value.reader.state,(unsigned long long)value.reader.request_id,
        (unsigned long long)value.reader.completed_id,value.action_error);
    abort();
}

static ryz_workbench_apps_intent_t intent(ryz_workbench_apps_action_t action)
{
    return (ryz_workbench_apps_intent_t){.expected=wb_snapshot().token,.action=action};
}

static esp_err_t submit(const ryz_workbench_apps_intent_t *value)
{
    size_t calls=script_store_host_platform_calls();
    esp_err_t error=ryz_workbench_apps_submit(value);
    assert(script_store_host_platform_calls()==calls);
    return error;
}

static ryz_workbench_apps_snapshot_t select_index(size_t index)
{
    ryz_workbench_apps_intent_t value=intent(RYZ_WB_APPS_SELECT);
    value.index=index;
    assert(submit(&value)==ESP_OK);
    return wb_settle(RYZ_APPS_VIEW_DETAIL,RYZ_APPS_READY);
}

static ryz_workbench_apps_snapshot_t open_selected(void)
{
    put("selected.lua",source_a);
    (void)wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    return select_index(0);
}

static void prepare_run(void)
{
    ryz_workbench_apps_intent_t value=intent(RYZ_WB_APPS_RUN);
    assert(submit(&value)==ESP_OK);
    unsigned before=launcher.callbacks;
    owner_tick(true);
    ryz_workbench_apps_snapshot_t state=wb_snapshot();
    assert(state.run_state==RYZ_WB_APPS_RUN_PREPARING && state.run_id);
    assert(launcher.callbacks==before && allocated_sources()==0);
}

static void case_navigation(void)
{
    wb_setup();
    for (unsigned index=0;index<20;++index) {
        char name[20];
        snprintf(name,sizeof(name),"tool_%02u.lua",index);
        put(name,source_a);
    }
    ryz_workbench_apps_snapshot_t first=wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    assert(first.reader.page.total==20 && first.reader.page.offset==0);
    assert(first.reader.page.count==RYZ_SCRIPT_STORE_PAGE_MAX && first.page_limit==RYZ_SCRIPT_STORE_PAGE_MAX);
    ryz_workbench_apps_intent_t page=intent(RYZ_WB_APPS_PAGE);
    /* Argument rejection must not occupy the single pending intent slot. */
    assert(submit(&page)==ESP_ERR_INVALID_ARG);
    page.limit=3; page.index=1;
    assert(submit(&page)==ESP_ERR_INVALID_ARG);
    page.index=0; page.expected.page.modal=true;
    assert(submit(&page)==ESP_ERR_INVALID_ARG);
    page.expected.page.modal=false;
    page.offset=6; page.limit=3;
    assert(submit(&page)==ESP_OK);
    assert(submit(&page)==ESP_ERR_TIMEOUT);
    ryz_workbench_apps_snapshot_t second=wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    assert(second.reader.page.offset==6 && second.reader.page.count==3 && second.page_limit==3);
    assert(!strcmp(second.reader.page.entries[1].name,"tool_07.lua"));
    ryz_workbench_apps_snapshot_t detail=select_index(1);
    assert(detail.reader.detail.source_valid && !strcmp(detail.reader.detail.file.entry.name,"tool_07.lua"));
    assert(!strcmp(detail.reader.detail.metadata.author,"Selected A"));
    ryz_workbench_apps_intent_t back=intent(RYZ_WB_APPS_BACK);
    assert(submit(&back)==ESP_OK);
    second=wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    assert(second.reader.page.offset==6 && second.reader.page.count==3 && second.page_limit==3);
    assert(!strcmp(second.reader.page.entries[1].name,"tool_07.lua"));
    owner_tick(false);
    assert(!wb_snapshot().open && submit(&back)==ESP_ERR_INVALID_STATE);
    ++shown_page.generation;
    second=wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    assert(second.reader.page.offset==6 && second.reader.page.count==3 && second.page_limit==3);
    assert(!strcmp(second.reader.page.entries[1].name,"tool_07.lua"));
}

static void case_back_reader_refresh(void)
{
    wb_setup();
    ryz_workbench_apps_snapshot_t selected = open_selected();
    ryz_workbench_apps_intent_t back = intent(RYZ_WB_APPS_BACK);

    uint64_t first = request_detail("selected.lua", selected.reader.detail.revision);
    (void)settle(first, RYZ_APPS_READY);
    owner_tick(true);
    assert(wb_snapshot().token.reader_request_id == first);
    assert(submit(&back) == ESP_OK);

    uint64_t second = request_detail("selected.lua", selected.reader.detail.revision);
    (void)settle(second, RYZ_APPS_READY);
    owner_tick(true);
    ryz_workbench_apps_snapshot_t catalog =
        wb_settle(RYZ_APPS_VIEW_CATALOG, RYZ_APPS_READY);
    assert(catalog.reader.page.offset == 0);
}

static void case_run_snapshot(void)
{
    wb_setup();
    ryz_workbench_apps_snapshot_t selected=open_selected();
    assert(!strcmp(selected.reader.detail.metadata.author,"Selected A"));
    prepare_run();
    ryz_workbench_apps_intent_t duplicate=intent(RYZ_WB_APPS_RUN);
    assert(submit(&duplicate)==ESP_OK);
    owner_tick(true);
    assert(wb_snapshot().run_state==RYZ_WB_APPS_RUN_PREPARING && !launcher.callbacks);
    size_t calls=script_store_host_platform_calls();
    rx_tick(false);
    assert(script_store_host_platform_calls()>calls && allocated_sources()==1);
#ifdef WB_TEST_EXTERNAL_SOURCE
    assert(external_source_attempts==1);
#endif
    /* Do not advance the reader: this is a versioned source snapshot, not a
     * promise that a later reopen of its filename would still return A. */
    put("selected.lua",source_b);
    owner_tick(true);
    ryz_workbench_apps_snapshot_t started=wb_snapshot();
    assert(started.run_state==RYZ_WB_APPS_RUN_STARTED && started.action_error==ESP_OK);
    assert(launcher.callbacks==1 && launcher.enqueues==1 && launcher.notifications==1);
    assert(launcher.queued && !strcmp(launcher.queued->source,source_a));
    assert(!strcmp(launcher.queued->sha256,selected.reader.detail.file.sha256));
    assert(!strcmp(started.job_id,launcher.queued->id));
    assert(submit(&duplicate)==ESP_OK);
    owner_tick(true);
    rx_tick(false);
    owner_tick(true);
    assert(launcher.callbacks==1 && launcher.enqueues==1);
    assert_write_released(0,0);
}

static void case_run_max_source(void)
{
    wb_setup();
    char source[16385];
    memset(source,'x',16384);
    source[0]='-'; source[1]='-'; source[16384]='\0';
    put("max.lua",source);
    (void)wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    ryz_workbench_apps_snapshot_t selected=select_index(0);
    assert(selected.reader.detail.source_valid && selected.reader.detail.file.entry.bytes==16384);
    prepare_run();
    rx_tick(false);
    owner_tick(true);
    ryz_workbench_apps_snapshot_t started=wb_snapshot();
    assert(started.run_state==RYZ_WB_APPS_RUN_STARTED && started.action_error==ESP_OK);
    assert(launcher.queued && allocated_sources()==1);
    assert(!memcmp(launcher.queued->source,source,sizeof(source)));
#ifdef WB_TEST_EXTERNAL_SOURCE
    assert(external_source_attempts==1 && external_source_bytes==16385);
#endif
}

static void case_identity_and_invalid_source(void)
{
    ryz_workbench_apps_snapshot_t blank;
    memset(&blank,0xa5,sizeof(blank));
    assert(ryz_workbench_apps_get_snapshot(&blank)==ESP_ERR_INVALID_STATE);
    assert_zero(&blank,sizeof(blank));
    assert(ryz_workbench_apps_get_snapshot(NULL)==ESP_ERR_INVALID_ARG);
    assert(ryz_workbench_apps_submit(NULL)==ESP_ERR_INVALID_ARG);
    assert(ryz_workbench_apps_init("")==ESP_ERR_INVALID_ARG);
    wb_setup();
    assert(ryz_workbench_apps_init(wb_boot)==ESP_ERR_INVALID_STATE);
    raw_file("bad.lua","",0);
    put("good.lua",source_a);
    ryz_workbench_apps_snapshot_t catalog=wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    ryz_workbench_apps_intent_t stale=intent(RYZ_WB_APPS_SELECT);
    stale.expected.page.generation++;
    assert(submit(&stale)==ESP_ERR_INVALID_STATE);
    stale.expected=catalog.token;
    stale.expected.reader_request_id++;
    assert(submit(&stale)==ESP_ERR_INVALID_STATE);
    stale.expected=catalog.token;
    stale.index=catalog.reader.page.count;
    assert(submit(&stale)==ESP_OK);
    owner_tick(true);
    assert(wb_snapshot().action_error==ESP_ERR_INVALID_STATE);
    ryz_workbench_apps_snapshot_t bad=select_index(0);
    assert(!bad.reader.detail.source_valid && bad.reader.detail.file.entry.bytes==0);
    assert(bad.reader.detail.source_error==ESP_ERR_INVALID_SIZE);
    ryz_workbench_apps_intent_t run=intent(RYZ_WB_APPS_RUN);
    assert(submit(&run)==ESP_OK);
    owner_tick(true);
    assert(wb_snapshot().action_error==ESP_ERR_INVALID_STATE);
    size_t calls=script_store_host_platform_calls();
    rx_tick(false);
    assert(script_store_host_platform_calls()==calls && !launcher.callbacks && !allocated_sources());
    stale.expected=catalog.token;
    stale.index=1;
    assert(submit(&stale)==ESP_ERR_INVALID_STATE);
    ryz_workbench_apps_intent_t back=intent(RYZ_WB_APPS_BACK);
    assert(submit(&back)==ESP_OK);
    ++shown_page.generation;
    owner_tick(true);
    assert(submit(&back)==ESP_ERR_INVALID_STATE);
    assert(wb_snapshot().token.page.generation==shown_page.generation);
}

static void case_change_before_rx(void)
{
    wb_setup();
    (void)open_selected();
    prepare_run();
    put("selected.lua",source_b);
    rx_tick(false);
    assert(allocated_sources()==0);
    owner_tick(true);
    ryz_workbench_apps_snapshot_t value=wb_snapshot();
    assert(value.run_state==RYZ_WB_APPS_RUN_FAILED && value.action_error!=ESP_OK);
    assert(strstr(value.message,"changed") && !launcher.callbacks && !launcher.enqueues);
    size_t calls=script_store_host_platform_calls();
    for (unsigned index=0;index<10;++index) { owner_tick(true); rx_tick(false); }
    assert(script_store_host_platform_calls()==calls);
}

static void case_discard(const char *mode)
{
    wb_setup();
    (void)open_selected();
    prepare_run();
    rx_tick(false);
    assert(allocated_sources()==1);
    if (!strcmp(mode,"discard_back")) {
        ryz_workbench_apps_intent_t back=intent(RYZ_WB_APPS_BACK);
        assert(submit(&back)==ESP_OK);
        owner_tick(true);
        assert(wb_snapshot().reader.view==RYZ_APPS_VIEW_CATALOG);
    } else if (!strcmp(mode,"discard_close")) {
        owner_tick(false);
        assert(!wb_snapshot().open);
    } else if (!strcmp(mode,"discard_page")) {
        ++shown_page.generation;
        owner_tick(true);
        assert(wb_snapshot().token.page.generation==shown_page.generation);
    } else {
        assert(!strcmp(mode,"discard_reader"));
        put("selected.lua",source_b);
        step();
        owner_tick(true);
        assert(wb_snapshot().reader.state==RYZ_APPS_STALE);
    }
    assert(!allocated_sources() && !launcher.callbacks && !launcher.enqueues);
}

static void *rx_thread(void *unused)
{
    assert(unused==NULL);
    rx_tick(false);
    return NULL;
}

static void case_inflight_close(void)
{
    wb_setup();
    (void)open_selected();
    prepare_run();
    script_store_host_sha_gate_arm();
    pthread_t rx;
    assert(pthread_create(&rx,NULL,rx_thread,NULL)==0);
    script_store_host_sha_gate_wait();
    /* RX is inside the real Store hash while UI must still close/copy without
     * waiting for the Store lock or adopting an unprepared job. */
    owner_tick(false);
    assert(!wb_snapshot().open && !launcher.callbacks);
    script_store_host_sha_gate_release();
    assert(pthread_join(rx,NULL)==0);
    assert(allocated_sources()==1);
    owner_tick(false);
    assert(!allocated_sources() && !launcher.callbacks && !launcher.enqueues);
}

static void *json_refuse(size_t bytes) { (void)bytes; return NULL; }

static void case_launch_failure(const char *mode)
{
    wb_setup();
    (void)open_selected();
    prepare_run();
    if (!strcmp(mode,"source_oom")) {
#ifdef WB_TEST_EXTERNAL_SOURCE
        /* Internal malloc remains available: failure must not fall back. */
        fail_external_source=true;
#else
        fail_source_allocation=true;
#endif
    }
    if (!strcmp(mode,"json_oom")) {
        cJSON_Hooks hooks={.malloc_fn=json_refuse,.free_fn=free};
        cJSON_InitHooks(&hooks);
    }
    rx_tick(false);
    fail_source_allocation=false;
#ifdef WB_TEST_EXTERNAL_SOURCE
    fail_external_source=false;
    if (!strcmp(mode,"source_oom")) assert(external_source_attempts==1);
#endif
    cJSON_InitHooks(NULL);
    script_job_t occupied={.active=true};
    if (!strcmp(mode,"gateway")) launcher.ready=false;
    else if (!strcmp(mode,"queue")) launcher.queue_ready=false;
    else if (!strcmp(mode,"busy")) launcher.current=&occupied;
    else if (!strcmp(mode,"uncertain")) launcher.uncertain=true;
    if (!strcmp(mode,"no_handler")) {
        size_t calls=script_store_host_platform_calls();
        ryz_workbench_apps_owner_tick(true,shown_page,NULL,NULL);
        assert(script_store_host_platform_calls()==calls);
    } else owner_tick(true);
    ryz_workbench_apps_snapshot_t value=wb_snapshot();
    assert(value.run_state==(!strcmp(mode,"uncertain") ? RYZ_WB_APPS_RUN_UNKNOWN : RYZ_WB_APPS_RUN_FAILED));
    assert(value.action_error!=ESP_OK && !launcher.enqueues && !launcher.notifications && !allocated_sources());
    if (!strcmp(mode,"busy")) {
        assert(launcher.current==&occupied);
        launcher.current=NULL;
    } else assert(!launcher.current);
    if (!strcmp(mode,"no_handler")) assert(!launcher.callbacks && value.action_error==ESP_ERR_NOT_SUPPORTED);
    if (!strcmp(mode,"source_oom") || !strcmp(mode,"json_oom"))
        assert(!launcher.callbacks && value.action_error==ESP_ERR_NO_MEM);
    unsigned callbacks=launcher.callbacks;
    size_t calls=script_store_host_platform_calls();
    if (!strcmp(mode,"uncertain")) {
        ryz_workbench_apps_intent_t retry=intent(RYZ_WB_APPS_RUN);
        assert(submit(&retry)==ESP_OK);
    }
    for (unsigned index=0;index<10;++index) { owner_tick(true); rx_tick(false); }
    assert(script_store_host_platform_calls()==calls && launcher.callbacks==callbacks);
}

static void case_back_deleted_page(void)
{
    wb_setup();
    for (unsigned index=0;index<10;++index) {
        char name[20];
        snprintf(name,sizeof(name),"tool_%02u.lua",index);
        put(name,source_a);
    }
    (void)wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    ryz_workbench_apps_intent_t page=intent(RYZ_WB_APPS_PAGE);
    page.offset=8; page.limit=2;
    assert(submit(&page)==ESP_OK);
    (void)wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    (void)select_index(0);
    for (unsigned index=6;index<10;++index) {
        char name[20];
        snprintf(name,sizeof(name),"tool_%02u.lua",index);
        remove_script(name);
    }
    ryz_workbench_apps_intent_t back=intent(RYZ_WB_APPS_BACK);
    assert(submit(&back)==ESP_OK);
    ryz_workbench_apps_snapshot_t first=wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    assert(first.reader.page.offset==0 && first.reader.page.total==6 && first.reader.page.count==2);
    assert(first.page_limit==2);
    size_t calls=script_store_host_platform_calls();
    for (unsigned index=0;index<10;++index) owner_tick(true);
    assert(script_store_host_platform_calls()==calls);
}

static ryz_workbench_apps_intent_t delete_intent(void)
{
    ryz_workbench_apps_snapshot_t selected=wb_snapshot();
    ryz_workbench_apps_intent_t value={.expected=selected.token,.action=RYZ_WB_APPS_DELETE};
    strcpy(value.delete_name,selected.reader.detail.file.entry.name);
    strcpy(value.delete_sha256,selected.reader.detail.file.sha256);
    return value;
}

static void case_delete_selected(void)
{
    wb_setup();
    (void)open_selected();
    ryz_workbench_apps_intent_t confirmed=delete_intent();
    assert(submit(&confirmed)==ESP_OK);
    owner_tick(true);
    ryz_workbench_apps_snapshot_t pending=wb_snapshot();
    assert(pending.deletion.id && pending.deletion.state==RYZ_WB_APPS_DELETE_PENDING);
    ryz_script_store_description_t before;
    assert(ryz_script_store_describe("selected.lua",&before)==ESP_OK);
    rx_tick(false);
    owner_tick(true);
    ryz_workbench_apps_snapshot_t done=wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_EMPTY);
    assert(done.deletion.id==pending.deletion.id && done.deletion.state==RYZ_WB_APPS_DELETE_COMMITTED);
    assert(done.deletion.storage_result_valid && done.deletion.commit==RYZ_SCRIPT_STORE_COMMITTED);
    assert(done.deletion.error==ESP_OK && done.deletion.cleanup_error==ESP_OK && !done.deletion.recovery_required);
    assert(!strcmp(done.deletion.name,"selected.lua") && !launcher.callbacks && !allocated_sources());
    assert(ryz_script_store_describe("selected.lua",&before)==ESP_ERR_NOT_FOUND);
    owner_tick(false);
    assert(wb_snapshot().deletion.state==RYZ_WB_APPS_DELETE_COMMITTED);
    assert_write_released(1,1);
}

static void prepare_delete(void)
{
    ryz_workbench_apps_intent_t confirmed=delete_intent();
    assert(submit(&confirmed)==ESP_OK);
    owner_tick(true);
    assert(wb_snapshot().deletion.state==RYZ_WB_APPS_DELETE_PENDING);
}

static void assert_file_source(const char *name,const char *source)
{
    ryz_script_store_snapshot_t value;
    assert(ryz_script_store_get(name,&value)==ESP_OK);
    assert(value.entry.bytes==strlen(source) && !strcmp(value.source,source));
    ryz_script_store_snapshot_free(&value);
}

static void case_delete_guard(const char *mode)
{
    wb_setup();
    (void)open_selected();
    prepare_delete();
    ryz_workbench_write_guard_t guard=write_guard;
    const ryz_workbench_write_guard_t *selected=&guard;
    script_job_t occupied={.active=true};
    unsigned expected_begins=0;
    if (!strcmp(mode,"delete_guard_missing")) selected=NULL;
    else if (!strcmp(mode,"delete_guard_no_begin")) guard.try_begin_write=NULL;
    else if (!strcmp(mode,"delete_guard_no_end")) guard.end_write=NULL;
    else {
        expected_begins=1;
        if (!strcmp(mode,"delete_guard_denied")) launcher.write_allowed=false;
        else {
            assert(!strcmp(mode,"delete_guard_new_job"));
            /* The earlier runtime_busy sample was false. Only the shared Job
             * gate can observe this subsequently admitted owner's identity. */
            wb_job_lock(&launcher);
            launcher.current=&occupied;
            wb_job_unlock(&launcher);
        }
    }
    size_t calls=script_store_host_platform_calls();
    ryz_workbench_apps_rx_tick(false,selected);
    assert(script_store_host_platform_calls()==calls);
    owner_tick(true);
    ryz_workbench_apps_snapshot_t done=wb_snapshot();
    assert(done.deletion.state==RYZ_WB_APPS_DELETE_FAILED && done.deletion.error!=ESP_OK);
    assert(!done.deletion.storage_result_valid && !launcher.callbacks && !allocated_sources());
    assert_write_released(expected_begins,0);
    assert_file_source("selected.lua",source_a);
    wb_job_lock(&launcher);
    launcher.current=NULL;
    launcher.write_allowed=true;
    wb_job_unlock(&launcher);
    calls=script_store_host_platform_calls();
    for (unsigned index=0;index<10;++index) { owner_tick(true); rx_tick(false); }
    assert(script_store_host_platform_calls()==calls);
    assert_write_released(expected_begins,0);
}

static void *wb_hold_store_read(void *unused)
{
    assert(unused==NULL);
    assert_file_source("selected.lua",source_a);
    return NULL;
}

static void case_delete_store_busy(void)
{
    wb_setup();
    (void)open_selected();
    prepare_delete();
    script_store_host_sha_gate_arm();
    pthread_t reader;
    assert(pthread_create(&reader,NULL,wb_hold_store_read,NULL)==0);
    script_store_host_sha_gate_wait();
    /* A successful writer reservation does not imply Store availability. Its
     * nonblocking rejection still has to return that reservation exactly once. */
    rx_tick(false);
    owner_tick(true);
    ryz_workbench_apps_snapshot_t done=wb_snapshot();
    assert(done.deletion.state==RYZ_WB_APPS_DELETE_FAILED && done.deletion.error!=ESP_OK);
    assert(done.deletion.storage_result_valid && done.deletion.commit==RYZ_SCRIPT_STORE_NOT_COMMITTED);
    assert_write_released(1,1);
    script_store_host_sha_gate_release();
    assert(pthread_join(reader,NULL)==0);
    assert_file_source("selected.lua",source_a);
    size_t calls=script_store_host_platform_calls();
    for (unsigned index=0;index<10;++index) { owner_tick(true); rx_tick(false); }
    assert(script_store_host_platform_calls()==calls);
    assert_write_released(1,1);
}

static void case_delete_rejected(const char *mode)
{
    wb_setup();
    (void)open_selected();
    ryz_workbench_apps_intent_t confirmed=delete_intent();
    if (!strcmp(mode,"delete_mismatch")) {
        strcpy(confirmed.delete_name,"different.lua");
        assert(submit(&confirmed)==ESP_OK);
        owner_tick(true);
    } else {
        prepare_delete();
        if (!strcmp(mode,"delete_changed")) put("selected.lua",source_b);
        if (!strcmp(mode,"delete_back")) {
            ryz_workbench_apps_intent_t back=intent(RYZ_WB_APPS_BACK);
            assert(submit(&back)==ESP_OK); /* Not yet consumed: RX must see it. */
        } else if (!strcmp(mode,"delete_close")) owner_tick(false);
        else if (!strcmp(mode,"delete_newpage")) {
            ++shown_page.generation;
            owner_tick(true);
        }
        rx_tick(!strcmp(mode,"delete_busy"));
        owner_tick(strcmp(mode,"delete_close")!=0);
    }
    ryz_workbench_apps_snapshot_t done=wb_snapshot();
    assert(done.deletion.state==RYZ_WB_APPS_DELETE_FAILED && done.deletion.error!=ESP_OK);
    if (!strcmp(mode,"delete_changed")) {
        assert(done.deletion.storage_result_valid && done.deletion.commit==RYZ_SCRIPT_STORE_NOT_COMMITTED);
        assert_file_source("selected.lua",source_b);
    } else {
        assert(!done.deletion.storage_result_valid);
        assert_file_source("selected.lua",source_a);
    }
    assert(!launcher.callbacks && !allocated_sources());
    size_t calls=script_store_host_platform_calls();
    for (unsigned index=0;index<10;++index) {
        owner_tick(strcmp(mode,"delete_close")!=0);
        rx_tick(false);
    }
    assert(script_store_host_platform_calls()==calls);
    unsigned expected=!strcmp(mode,"delete_changed") ? 1U : 0U;
    assert_write_released(expected,expected);
}

static void case_delete_file_kind(const char *mode)
{
    wb_setup();
    const bool boot_file=!strcmp(mode,"delete_boot");
    const char *name=boot_file ? "boot.lua" : "broken.lua";
    if (boot_file) raw_file(name,source_a,strlen(source_a));
    else if (!strcmp(mode,"delete_empty")) raw_file(name,"",0);
    else if (!strcmp(mode,"delete_nul")) raw_file(name,"bad\0source",10);
    else {
        assert(!strcmp(mode,"delete_oversize"));
        char large[RYZ_SCRIPT_STORE_SOURCE_MAX+1];
        memset(large,'a',sizeof(large));
        raw_file(name,large,sizeof(large));
    }
    (void)wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    ryz_workbench_apps_snapshot_t selected=select_index(0);
    assert(!selected.reader.detail.file.entry.protected_file);
    assert(selected.reader.detail.source_valid==boot_file);
    ryz_workbench_apps_intent_t confirmed=delete_intent();
    ryz_workbench_apps_intent_t invalid=confirmed;
    memset(invalid.delete_name,'x',sizeof(invalid.delete_name));
    assert(submit(&invalid)==ESP_ERR_INVALID_ARG);
    invalid=confirmed; invalid.delete_sha256[0]='G';
    assert(submit(&invalid)==ESP_ERR_INVALID_ARG);
    invalid=confirmed; invalid.delete_sha256[64]='x';
    assert(submit(&invalid)==ESP_ERR_INVALID_ARG);
    assert(submit(&confirmed)==ESP_OK);
    assert(submit(&confirmed)==ESP_ERR_TIMEOUT);
    owner_tick(true);
    size_t calls=script_store_host_platform_calls();
    rx_tick(false);
    assert(script_store_host_platform_calls()>calls);
    owner_tick(true);
    ryz_workbench_apps_snapshot_t done=wb_snapshot();
    ryz_script_store_description_t description;
    done=wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_EMPTY);
    assert(done.deletion.state==RYZ_WB_APPS_DELETE_COMMITTED);
    assert(done.deletion.storage_result_valid && done.deletion.commit==RYZ_SCRIPT_STORE_COMMITTED);
    assert(ryz_script_store_describe(name,&description)==ESP_ERR_NOT_FOUND);
    assert(!launcher.callbacks && !allocated_sources());
}

static void case_delete_inflight_close(void)
{
    wb_setup();
    (void)open_selected();
    prepare_delete();
    const uint64_t id=wb_snapshot().deletion.id;
    ryz_workbench_apps_intent_t duplicate=delete_intent();
    assert(submit(&duplicate)==ESP_OK);
    owner_tick(true);
    assert(wb_snapshot().deletion.id==id && wb_snapshot().action_error!=ESP_OK);
    ryz_workbench_apps_intent_t run=intent(RYZ_WB_APPS_RUN);
    assert(submit(&run)==ESP_OK);
    owner_tick(true);
    assert(wb_snapshot().run_state==RYZ_WB_APPS_RUN_IDLE);
    script_store_host_sha_gate_arm();
    pthread_t rx;
    assert(pthread_create(&rx,NULL,rx_thread,NULL)==0);
    script_store_host_sha_gate_wait();
    /* Store is blocked, but the Job mutex must be free. A real final admission
     * sees the active writer reservation and cannot publish a competing Job. */
    wb_job_lock(&launcher);
    assert(launcher.write_active && launcher.write_begins==1 && launcher.write_ends==0);
    wb_job_unlock(&launcher);
    char *candidate=wb_apps_test_malloc(sizeof(source_a));
    assert(candidate);
    memcpy(candidate,source_a,sizeof(source_a));
    cJSON *reply=ryz_workbench_job_start(&launcher.start,"competing-run","other.lua",candidate,0,false);
    assert(reply && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(reply,"ok")));
    cJSON_Delete(reply);
    assert(!launcher.current && !launcher.queued && !launcher.enqueues && !allocated_sources());
    owner_tick(false); /* RX is accepted and holding Store, but not UI gate. */
    assert(!wb_snapshot().open && wb_snapshot().deletion.state==RYZ_WB_APPS_DELETE_PENDING);
    script_store_host_sha_gate_release();
    assert(pthread_join(rx,NULL)==0);
    owner_tick(false);
    ryz_workbench_apps_snapshot_t done=wb_snapshot();
    assert(!done.open && done.deletion.id==id && done.deletion.state==RYZ_WB_APPS_DELETE_COMMITTED);
    ryz_script_store_description_t description;
    assert(ryz_script_store_describe("selected.lua",&description)==ESP_ERR_NOT_FOUND);
    assert(!launcher.callbacks && !allocated_sources());
    size_t calls=script_store_host_platform_calls();
    for (unsigned index=0;index<10;++index) { owner_tick(false); rx_tick(false); }
    assert(script_store_host_platform_calls()==calls);
    assert_write_released(1,1);
}

static void case_delete_last_page(void)
{
    wb_setup();
    for (unsigned index=0;index<3;++index) {
        char name[20];
        snprintf(name,sizeof(name),"tool_%02u.lua",index);
        put(name,source_a);
    }
    (void)wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    ryz_workbench_apps_intent_t page=intent(RYZ_WB_APPS_PAGE);
    page.offset=2; page.limit=2;
    assert(submit(&page)==ESP_OK);
    (void)wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    (void)select_index(0);
    prepare_delete();
    rx_tick(false);
    ryz_workbench_apps_snapshot_t done=wb_settle(RYZ_APPS_VIEW_CATALOG,RYZ_APPS_READY);
    assert(done.deletion.state==RYZ_WB_APPS_DELETE_COMMITTED);
    assert(done.reader.page.offset==0 && done.reader.page.total==2 && done.reader.page.count==2);
    assert(done.page_limit==2 && !strcmp(done.deletion.name,"tool_02.lua"));
}

static unsigned post_commit_allocation_failures;
static void *json_fail_after_storage_fault(size_t bytes)
{
    if (script_store_host_fault_hits()) { ++post_commit_allocation_failures; return NULL; }
    return malloc(bytes);
}

static void case_delete_failure(const char *mode)
{
    wb_setup();
    (void)open_selected();
    prepare_delete();
    const bool request_oom=!strcmp(mode,"delete_request_oom");
    const bool ack_unknown=!strcmp(mode,"delete_ack_unknown");
    const bool storage_unknown=!strcmp(mode,"delete_storage_unknown");
    if (request_oom) {
        cJSON_Hooks hooks={.malloc_fn=json_refuse,.free_fn=free};
        cJSON_InitHooks(&hooks);
    } else if (storage_unknown) {
        script_store_host_fault(SCRIPT_HOST_WRITE_PARTIAL,".ryz-txn",NULL,0,1);
    } else {
        script_store_host_fault(SCRIPT_HOST_REMOVE_AFTER,".ryz-old",NULL,0,1);
        if (ack_unknown) {
            cJSON_Hooks hooks={.malloc_fn=json_fail_after_storage_fault,.free_fn=free};
            cJSON_InitHooks(&hooks);
        }
    }
    rx_tick(false);
    cJSON_InitHooks(NULL);
    owner_tick(true);
    ryz_workbench_apps_snapshot_t done=wb_snapshot();
    if (request_oom) {
        assert(done.deletion.state==RYZ_WB_APPS_DELETE_FAILED && done.deletion.error==ESP_ERR_NO_MEM);
        assert(!done.deletion.storage_result_valid);
        assert_file_source("selected.lua",source_a);
    } else {
        assert(script_store_host_fault_hits()==1);
        if (storage_unknown) {
            assert_file_source("selected.lua",source_a);
            assert(done.deletion.state==RYZ_WB_APPS_DELETE_UNKNOWN && done.deletion.error!=ESP_OK);
            assert(done.deletion.storage_result_valid && done.deletion.commit==RYZ_SCRIPT_STORE_COMMIT_UNKNOWN);
            assert(done.deletion.recovery_required);
            ryz_workbench_apps_intent_t duplicate=delete_intent();
            assert(submit(&duplicate)==ESP_OK);
        } else {
            ryz_script_store_description_t description;
            assert(ryz_script_store_describe("selected.lua",&description)==ESP_ERR_NOT_FOUND);
        }
        if (ack_unknown) {
            assert(post_commit_allocation_failures>0 && done.deletion.state==RYZ_WB_APPS_DELETE_UNKNOWN);
            assert(!done.deletion.storage_result_valid && done.deletion.error==ESP_ERR_NO_MEM);
            ryz_workbench_apps_intent_t duplicate=delete_intent();
            assert(submit(&duplicate)==ESP_OK);
        } else if (!storage_unknown) {
            assert(done.deletion.state==RYZ_WB_APPS_DELETE_COMMITTED && done.deletion.storage_result_valid);
            assert(done.deletion.commit==RYZ_SCRIPT_STORE_COMMITTED && done.deletion.cleanup_error!=ESP_OK);
            assert(done.deletion.error!=ESP_OK && done.deletion.recovery_required);
        }
    }
    size_t calls=script_store_host_platform_calls();
    for (unsigned index=0;index<10;++index) { owner_tick(true); rx_tick(false); }
    assert(script_store_host_platform_calls()==calls && !launcher.callbacks && !allocated_sources());
    assert_write_released(request_oom ? 0U : 1U,request_oom ? 0U : 1U);
    script_store_host_clear_fault();
}

static void finish_test(void)
{
    owner_tick(false);
    rx_tick(false);
    owner_tick(false);
    stop_worker();
    if (launcher.current) {
        assert(launcher.current==launcher.queued);
        ryz_workbench_job_destroy(launcher.current);
        launcher.current=launcher.queued=NULL;
    }
    assert(allocated_sources()==0);
    assert(!launcher.write_active && launcher.write_accepts==launcher.write_ends);
    assert(pthread_mutex_destroy(&launcher.mutex)==0);
}

int main(int argc,char **argv)
{
    assert(argc==3);
    data_root=argv[2];
    script_store_host_configure(data_root);
    if (!strcmp(argv[1],"navigation")) case_navigation();
    else if (!strcmp(argv[1],"back_reader_refresh")) case_back_reader_refresh();
    else if (!strcmp(argv[1],"run_snapshot")) case_run_snapshot();
    else if (!strcmp(argv[1],"run_max_source")) case_run_max_source();
    else if (!strcmp(argv[1],"identities")) case_identity_and_invalid_source();
    else if (!strcmp(argv[1],"change_before_rx")) case_change_before_rx();
    else if (!strncmp(argv[1],"discard_",8)) case_discard(argv[1]);
    else if (!strcmp(argv[1],"inflight_close")) case_inflight_close();
    else if (!strcmp(argv[1],"deleted_page")) case_back_deleted_page();
    else if (!strcmp(argv[1],"delete_selected")) case_delete_selected();
    else if (!strncmp(argv[1],"delete_guard_",13)) case_delete_guard(argv[1]);
    else if (!strcmp(argv[1],"delete_store_busy")) case_delete_store_busy();
    else if (!strcmp(argv[1],"delete_mismatch") || !strcmp(argv[1],"delete_changed") ||
             !strcmp(argv[1],"delete_back") || !strcmp(argv[1],"delete_close") ||
             !strcmp(argv[1],"delete_newpage") || !strcmp(argv[1],"delete_busy")) case_delete_rejected(argv[1]);
    else if (!strcmp(argv[1],"delete_boot") || !strcmp(argv[1],"delete_empty") ||
             !strcmp(argv[1],"delete_nul") || !strcmp(argv[1],"delete_oversize")) case_delete_file_kind(argv[1]);
    else if (!strcmp(argv[1],"delete_inflight_close")) case_delete_inflight_close();
    else if (!strcmp(argv[1],"delete_last_page")) case_delete_last_page();
    else if (!strcmp(argv[1],"delete_cleanup") || !strcmp(argv[1],"delete_request_oom") ||
             !strcmp(argv[1],"delete_ack_unknown") || !strcmp(argv[1],"delete_storage_unknown")) case_delete_failure(argv[1]);
    else if (!strcmp(argv[1],"gateway") || !strcmp(argv[1],"queue") ||
             !strcmp(argv[1],"busy") || !strcmp(argv[1],"uncertain") ||
             !strcmp(argv[1],"no_handler") || !strcmp(argv[1],"source_oom") ||
             !strcmp(argv[1],"json_oom")) case_launch_failure(argv[1]);
    else abort();
    finish_test();
    printf("WORKBENCH_APPS_PASS %s\n",argv[1]);
    return 0;
}
