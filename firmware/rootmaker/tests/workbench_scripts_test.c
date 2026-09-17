/* Production controller with explicit reader/Store/guard substitutes.
 * These tests cover admission and outcomes, not flash/filesystem durability. */
#include "workbench_scripts.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

static ryz_apps_snapshot_t reader;
static unsigned starts, closes, pages, details, fs_calls, removes, copies, guard_begins, guard_ends;
static uint64_t next_reader;
static size_t requested_offset, requested_limit;
static uint32_t requested_revision;
static char requested_name[41], written_sha[65];
static ryz_script_store_status_t store = {.ready=true, .revision=3};
static ryz_script_store_description_t boot;
static esp_err_t boot_error=ESP_ERR_NOT_FOUND, write_error;
static bool change_after_describe, guard_allowed=true, change_in_guard;
static unsigned status_timeouts;
static ryz_script_store_mutation_t mutation = {.commit=RYZ_SCRIPT_STORE_COMMITTED, .revision=4};
static pthread_mutex_t wait_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond=PTHREAD_COND_INITIALIZER;
static bool block_guard, guard_waiting, guard_release;
static bool fail_next_gate, fail_after_reader, fail_after_close;
static unsigned failed_gates;
static const ryz_ui_nav_token_t settings={1,10,false}, picker={2,11,false}, modal={3,12,true};
static const char sha_a[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char sha_b[]="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

bool scripts_test_and_set(volatile atomic_flag *object, memory_order order) {
    if(fail_next_gate) {fail_next_gate=false; ++failed_gates; return true;}
    return scripts_real_test_and_set(object,order);
}
esp_err_t ryz_apps_start(void) {++starts; return ESP_OK;}
esp_err_t ryz_apps_close(void) {
    ++closes; memset(&reader,0,sizeof(reader));
    if(fail_after_close) {fail_after_close=false; fail_next_gate=true;}
    return ESP_OK;
}
esp_err_t ryz_apps_get_snapshot(ryz_apps_snapshot_t *out) {
    *out=reader;
    if(fail_after_reader) {fail_after_reader=false; fail_next_gate=true;}
    return ESP_OK;
}
esp_err_t ryz_apps_request_page(size_t offset,size_t limit,uint32_t revision,uint64_t *out) {
    ++pages; requested_offset=offset; requested_limit=limit; requested_revision=revision;
    *out=++next_reader;
    reader=(ryz_apps_snapshot_t){.request_id=*out,.view=RYZ_APPS_VIEW_CATALOG,.state=RYZ_APPS_LOADING};
    return ESP_OK;
}
esp_err_t ryz_apps_request_detail(const char *name,uint32_t revision,uint64_t *out) {
    ++details; requested_revision=revision; snprintf(requested_name,sizeof(requested_name),"%s",name);
    *out=++next_reader;
    reader=(ryz_apps_snapshot_t){.request_id=*out,.view=RYZ_APPS_VIEW_DETAIL,.state=RYZ_APPS_LOADING};
    return ESP_OK;
}
esp_err_t ryz_script_store_status(ryz_script_store_status_t *out) {
    ++fs_calls; if(status_timeouts) {--status_timeouts; return ESP_ERR_TIMEOUT;}
    *out=store; return ESP_OK;
}
esp_err_t ryz_script_store_describe(const char *name,ryz_script_store_description_t *out) {
    assert(!strcmp(name,"boot.lua")); ++fs_calls; *out=boot;
    if(change_after_describe) ++store.revision;
    return boot_error;
}
esp_err_t ryz_script_store_remove(const char *name,const char *sha,ryz_script_store_mutation_t *out) {
    assert(guard_begins==guard_ends+1); assert(!strcmp(name,"boot.lua"));
    ++removes; ++fs_calls; strcpy(written_sha,sha); *out=mutation;
    return write_error;
}
esp_err_t ryz_script_store_copy_boot(const char *name,const char *sha,uint32_t revision,ryz_script_store_mutation_t *out) {
    assert(guard_begins==guard_ends+1); assert(!strcmp(name,"clock.lua") && revision==3);
    ++copies; ++fs_calls; strcpy(written_sha,sha); *out=mutation;
    return write_error;
}
/* Deliberately no delete_all substitute: a production reference would fail linking. */
bool ryz_script_store_valid_name(const char *name) {
    return name && strlen(name)>4 && strlen(name)<=40 && !strcmp(name+strlen(name)-4,".lua");
}
static ryz_v5_scripts_snapshot_t snapshot(void) {
    ryz_v5_scripts_snapshot_t out; assert(ryz_workbench_scripts_get_snapshot(&out)==ESP_OK); return out;
}
static void finish_page(size_t offset,size_t total,uint32_t revision) {
    reader.state=total?RYZ_APPS_READY:RYZ_APPS_EMPTY; reader.completed_id=reader.request_id;
    reader.store_status_valid=reader.store_ready=true;
    size_t count=total-offset; if(count>RYZ_SCRIPT_STORE_PAGE_MAX) count=RYZ_SCRIPT_STORE_PAGE_MAX;
    reader.page=(ryz_script_store_page_t){.revision=revision,.offset=offset,.count=count,.total=total};
    for(size_t i=0;i<count;++i) snprintf(reader.page.entries[i].name,41,"file%02zu.lua",offset+i);
    if(offset==0 && count) strcpy(reader.page.entries[0].name,"clock.lua");
}
static void catalog(void) {
    assert(ryz_workbench_scripts_init()==ESP_OK);
    ryz_workbench_scripts_owner_tick(true,settings,RYZ_V5_SCRIPTS_SETTINGS);
    assert(starts==1 && pages==1 && requested_offset==0 && requested_limit==16 && fs_calls==0);
    finish_page(0,24,3);
    ryz_workbench_scripts_owner_tick(true,settings,RYZ_V5_SCRIPTS_SETTINGS);
    const ryz_v5_scripts_snapshot_t s=snapshot();
    assert(s.generation && s.store_ready && s.catalog.total==24 && s.catalog.count==16);
    assert(!strcmp(s.catalog.entries[0].name,"clock.lua"));
    assert(!s.can_change_boot && !s.can_restart && !s.can_delete_all && fs_calls==0);
}
static void boot_present(void) {
    boot_error=ESP_OK; strcpy(boot.entry.name,"boot.lua"); boot.entry.bytes=321; strcpy(boot.sha256,sha_b);
}
static void read_identity(void) {
    ryz_workbench_scripts_rx_tick(NULL);
    ryz_workbench_scripts_owner_tick(true,settings,RYZ_V5_SCRIPTS_SETTINGS);
}
static void choose(bool valid) {
    ryz_workbench_scripts_owner_tick(true,picker,RYZ_V5_SCRIPTS_PICKER);
    ryz_v5_scripts_intent_t intent={.action=RYZ_V5_SCRIPTS_ACTION_SELECT,
        .generation=snapshot().generation,.revision=3};
    strcpy(intent.name,"clock.lua");
    assert(ryz_workbench_scripts_submit(&intent)==ESP_OK);
    assert(ryz_workbench_scripts_submit(&intent)==ESP_ERR_TIMEOUT);
    ryz_workbench_scripts_owner_tick(true,picker,RYZ_V5_SCRIPTS_PICKER);
    assert(details==1 && requested_revision==3 && !strcmp(requested_name,"clock.lua"));
    reader.state=RYZ_APPS_READY; reader.store_status_valid=reader.store_ready=true;
    reader.completed_id=reader.request_id; reader.detail.revision=3;
    reader.detail.source_valid=valid; reader.detail.source_error=valid?ESP_OK:ESP_ERR_INVALID_SIZE;
    strcpy(reader.detail.file.entry.name,"clock.lua"); strcpy(reader.detail.file.sha256,sha_a);
    ryz_workbench_scripts_owner_tick(true,picker,RYZ_V5_SCRIPTS_PICKER);
    assert(snapshot().selected_identity_valid && snapshot().can_change_boot==valid);
    assert(ryz_workbench_scripts_submit(&intent)==ESP_ERR_INVALID_STATE);
}
static void selection(void) {
    catalog(); choose(true);
    unsigned before=pages;
    ryz_v5_scripts_intent_t page={.action=RYZ_V5_SCRIPTS_ACTION_PAGE,
        .generation=snapshot().generation,.revision=3,.limit=16,.scroll_y=37};
    assert(ryz_workbench_scripts_submit(&page)==ESP_OK);
    ryz_workbench_scripts_owner_tick(true,picker,RYZ_V5_SCRIPTS_PICKER);
    assert(snapshot().scroll_y==37 && pages==before && snapshot().selected_identity_valid);
    page.generation=snapshot().generation; page.offset=8; page.scroll_y=18*31;
    assert(ryz_workbench_scripts_submit(&page)==ESP_OK);
    ryz_workbench_scripts_owner_tick(true,picker,RYZ_V5_SCRIPTS_PICKER);
    assert(pages==before+1 && requested_offset==8 && requested_limit==16 && requested_revision==3);
    finish_page(8,24,3); ryz_workbench_scripts_owner_tick(true,picker,RYZ_V5_SCRIPTS_PICKER);
    assert(snapshot().selected_identity_valid && snapshot().can_change_boot && snapshot().scroll_y==18*31);
    assert(!strcmp(snapshot().selected.entry.name,"clock.lua") && fs_calls==0);
    ryz_workbench_scripts_owner_tick(false,(ryz_ui_nav_token_t){0},RYZ_V5_SCRIPTS_SETTINGS);
    assert(closes==1 && snapshot().generation==0);
    ryz_workbench_scripts_owner_tick(false,(ryz_ui_nav_token_t){0},RYZ_V5_SCRIPTS_SETTINGS);
    assert(closes==1);
}
static void boot_read(const char *mode) {
    catalog(); if(strcmp(mode,"boot-none")) boot_present();
    change_after_describe=!strcmp(mode,"boot-stale");
    read_identity();
    assert(fs_calls==3 && snapshot().boot_known==!change_after_describe);
    if(!change_after_describe) {
        assert(snapshot().boot_bytes_known && snapshot().boot_bytes==(boot_error==ESP_OK?321U:0U));
        assert(!strcmp(snapshot().boot_name,boot_error==ESP_OK?"boot.lua":""));
        assert(snapshot().can_clear_boot==(boot_error==ESP_OK));
        if(boot_error==ESP_OK) assert(snapshot().boot_revision==3 && !strcmp(snapshot().boot_sha256,sha_b));
    }
    for(unsigned i=0;i<4;++i) {ryz_workbench_scripts_owner_tick(true,settings,RYZ_V5_SCRIPTS_SETTINGS); ryz_workbench_scripts_rx_tick(NULL);}
    assert(fs_calls==3);
}
static bool begin_write(void *unused) {
    (void)unused; ++guard_begins; assert(snapshot().result==RYZ_V5_SCRIPTS_PENDING);
    if(block_guard) {
        pthread_mutex_lock(&wait_mutex); guard_waiting=true; pthread_cond_broadcast(&wait_cond);
        while(!guard_release) pthread_cond_wait(&wait_cond,&wait_mutex);
        pthread_mutex_unlock(&wait_mutex);
    }
    if(change_in_guard) ++store.revision;
    return guard_allowed;
}
static void end_write(void *unused) {(void)unused; ++guard_ends;}
static const ryz_workbench_scripts_write_guard_t guard={NULL,begin_write,end_write};

static ryz_v5_scripts_intent_t write_intent(bool copy) {
    ryz_v5_scripts_intent_t out={.action=copy?RYZ_V5_SCRIPTS_ACTION_APPLY_BOOT:RYZ_V5_SCRIPTS_ACTION_CLEAR_BOOT,
        .generation=snapshot().generation,.revision=3};
    strcpy(out.name,copy?"clock.lua":"boot.lua"); strcpy(out.sha256,copy?sha_a:sha_b); return out;
}
static ryz_ui_nav_token_t queue_write(bool copy,bool delay_publication) {
    catalog(); boot_present(); read_identity();
    if(copy) choose(true); else ryz_workbench_scripts_owner_tick(true,modal,RYZ_V5_SCRIPTS_CLEAR_BOOT);
    assert(!snapshot().can_delete_all && (copy?snapshot().can_change_boot:snapshot().can_clear_boot));
    const ryz_v5_scripts_intent_t intent=write_intent(copy);
    assert(ryz_workbench_scripts_submit(&intent)==ESP_OK);
    unsigned before=fs_calls;
    fail_after_reader=delay_publication;
    ryz_workbench_scripts_owner_tick(true,copy?picker:modal,copy?RYZ_V5_SCRIPTS_PICKER:RYZ_V5_SCRIPTS_CLEAR_BOOT);
    if(delay_publication) {
        assert(failed_gates==1 && !removes && !copies);
        ryz_workbench_scripts_owner_tick(true,copy?picker:modal,copy?RYZ_V5_SCRIPTS_PICKER:RYZ_V5_SCRIPTS_CLEAR_BOOT);
    }
    assert(snapshot().result==RYZ_V5_SCRIPTS_PENDING && !snapshot().can_clear_boot && !snapshot().can_change_boot);
    assert(!removes && !copies && fs_calls==before);
    return copy?picker:modal;
}
static void write_success(bool copy,bool publication) {
    ryz_ui_nav_token_t token=queue_write(copy,publication);
    ryz_workbench_scripts_rx_tick(&guard);
    assert(removes==(copy?0U:1U) && copies==(copy?1U:0U) && guard_begins==1 && guard_ends==1);
    assert(!strcmp(written_sha,copy?sha_a:sha_b));
    ryz_workbench_scripts_owner_tick(true,token,copy?RYZ_V5_SCRIPTS_PICKER:RYZ_V5_SCRIPTS_CLEAR_BOOT);
    assert(snapshot().result==RYZ_V5_SCRIPTS_COMMITTED && snapshot().show_saved==copy);
    assert(!snapshot().can_clear_boot && requested_offset==0 && requested_limit==16 && requested_revision==0);
    for(unsigned i=0;i<3;++i) {
        ryz_workbench_scripts_rx_tick(&guard);
        ryz_workbench_scripts_owner_tick(true,token,copy?RYZ_V5_SCRIPTS_PICKER:RYZ_V5_SCRIPTS_CLEAR_BOOT);
    }
    assert(removes+copies==1);
}
static void write_failure(const char *mode,bool copy) {
    ryz_ui_nav_token_t token=queue_write(copy,false);
    unsigned before=fs_calls;
    const ryz_workbench_scripts_write_guard_t missing_begin={NULL,NULL,end_write}, missing_end={NULL,begin_write,NULL};
    const ryz_workbench_scripts_write_guard_t *which=&guard;
    bool no_write=true, active=true;
    if(strstr(mode,"no-guard")) which=NULL;
    else if(strstr(mode,"no-begin")) which=&missing_begin;
    else if(strstr(mode,"no-end")) which=&missing_end;
    else if(strstr(mode,"busy")) guard_allowed=false;
    else if(strstr(mode,"revision")) change_in_guard=true;
    else if(strstr(mode,"close")) {
        fail_after_close=strstr(mode,"publication")!=NULL;
        ryz_workbench_scripts_owner_tick(false,(ryz_ui_nav_token_t){0},RYZ_V5_SCRIPTS_SETTINGS); active=false;
    } else if(strstr(mode,"new-page")) {
        ryz_workbench_scripts_owner_tick(true,(ryz_ui_nav_token_t){4,token.route,token.modal},
            copy?RYZ_V5_SCRIPTS_PICKER:RYZ_V5_SCRIPTS_CLEAR_BOOT);
    } else {
        no_write=false; mutation.commit=RYZ_SCRIPT_STORE_NOT_COMMITTED; write_error=ESP_FAIL;
        if(strstr(mode,"unknown")) {mutation.commit=RYZ_SCRIPT_STORE_COMMIT_UNKNOWN; mutation.recovery_required=true;}
        if(strstr(mode,"cleanup")) {mutation.commit=RYZ_SCRIPT_STORE_COMMITTED; mutation.cleanup_error=ESP_FAIL; mutation.recovery_required=true;}
        if(strstr(mode,"missing")) write_error=ESP_ERR_NOT_FOUND;
    }
    ryz_workbench_scripts_rx_tick(which);
    assert(removes+copies==(no_write?0U:1U));
    if(no_write) assert(fs_calls==before+(strstr(mode,"revision")?1U:0U));
    ryz_workbench_scripts_owner_tick(active,token,copy?RYZ_V5_SCRIPTS_PICKER:RYZ_V5_SCRIPTS_CLEAR_BOOT);
    ryz_v5_scripts_result_t expected=strstr(mode,"unknown")?RYZ_V5_SCRIPTS_UNKNOWN:
        strstr(mode,"cleanup")?RYZ_V5_SCRIPTS_COMMITTED:RYZ_V5_SCRIPTS_FAILED;
    assert(snapshot().result==expected && !snapshot().can_clear_boot && !snapshot().show_saved);
    if(strstr(mode,"busy")) assert(guard_begins==1 && !guard_ends);
    if(strstr(mode,"no-")) assert(!guard_begins && !guard_ends);
    if(strstr(mode,"revision")) assert(guard_begins==1 && guard_ends==1);
    for(unsigned i=0;i<3;++i) ryz_workbench_scripts_rx_tick(&guard);
    assert(removes+copies==(no_write?0U:1U));
}
static void *rx_thread(void *unused) {(void)unused; ryz_workbench_scripts_rx_tick(&guard); return NULL;}
static void inflight(bool copy) {
    (void)queue_write(copy,false); block_guard=true;
    pthread_t worker; assert(!pthread_create(&worker,NULL,rx_thread,NULL));
    pthread_mutex_lock(&wait_mutex);
    while(!guard_waiting) pthread_cond_wait(&wait_cond,&wait_mutex);
    pthread_mutex_unlock(&wait_mutex);
    assert(!removes && !copies);
    ryz_workbench_scripts_owner_tick(false,(ryz_ui_nav_token_t){0},RYZ_V5_SCRIPTS_SETTINGS);
    assert(snapshot().result==RYZ_V5_SCRIPTS_PENDING && !snapshot().generation);
    const ryz_ui_nav_token_t reopened={9,10,false};
    ryz_workbench_scripts_owner_tick(true,reopened,RYZ_V5_SCRIPTS_SETTINGS);
    assert(snapshot().result==RYZ_V5_SCRIPTS_PENDING && !snapshot().can_clear_boot);
    pthread_mutex_lock(&wait_mutex); guard_release=true; pthread_cond_broadcast(&wait_cond); pthread_mutex_unlock(&wait_mutex);
    assert(!pthread_join(worker,NULL));
    ryz_workbench_scripts_owner_tick(true,reopened,RYZ_V5_SCRIPTS_SETTINGS);
    assert(snapshot().page==RYZ_V5_SCRIPTS_SETTINGS && snapshot().result==RYZ_V5_SCRIPTS_COMMITTED);
    assert(removes+copies==1 && guard_ends==1 && !snapshot().show_saved);
}
static void boot_retry(bool bounded) {
    catalog(); status_timeouts=bounded?100:1;
    for(unsigned i=0;i<400;++i) {
        ryz_workbench_scripts_rx_tick(NULL);
        ryz_workbench_scripts_owner_tick(true,settings,RYZ_V5_SCRIPTS_SETTINGS);
    }
    if(!bounded) {assert(fs_calls==4 && snapshot().boot_known && !snapshot().boot_name[0]); return;}
    assert(fs_calls==3 && !snapshot().boot_known && status_timeouts==97);
    status_timeouts=0;
    ryz_workbench_scripts_owner_tick(true,picker,RYZ_V5_SCRIPTS_PICKER);
    ryz_workbench_scripts_rx_tick(NULL);
    ryz_workbench_scripts_owner_tick(true,picker,RYZ_V5_SCRIPTS_PICKER);
    assert(fs_calls==6 && snapshot().boot_known);
}
static void invalid_source(void) {
    catalog(); read_identity(); choose(false);
    assert(snapshot().selected_source_error==ESP_ERR_INVALID_SIZE && !snapshot().can_change_boot);
    reader.recovery_required=true;
    ryz_workbench_scripts_owner_tick(true,picker,RYZ_V5_SCRIPTS_PICKER);
    assert(snapshot().recovery_required && !snapshot().selected_identity_valid && !snapshot().boot_known);
    assert(!snapshot().can_change_boot && !snapshot().can_clear_boot && !snapshot().can_delete_all);
}
static void strict_fields(void) {
    ryz_v5_scripts_snapshot_t out; memset(&out,0xaa,sizeof(out));
    assert(ryz_workbench_scripts_get_snapshot(&out)==ESP_ERR_INVALID_STATE);
    const ryz_v5_scripts_snapshot_t zero={0}; assert(!memcmp(&out,&zero,sizeof(out)));
    assert(ryz_workbench_scripts_get_snapshot(NULL)==ESP_ERR_INVALID_ARG);
    catalog(); boot_present(); read_identity();
    assert(ryz_workbench_scripts_init()==ESP_ERR_INVALID_STATE);
    ryz_workbench_scripts_owner_tick(true,modal,RYZ_V5_SCRIPTS_CLEAR_BOOT);
    ryz_v5_scripts_intent_t command=write_intent(false);
    strcpy(command.name,"clock.lua"); assert(ryz_workbench_scripts_submit(&command)==ESP_ERR_INVALID_ARG);
    command=write_intent(false); strcpy(command.sha256,sha_a); assert(ryz_workbench_scripts_submit(&command)==ESP_ERR_INVALID_STATE);
    command=write_intent(false); command.sha256[3]='g'; assert(ryz_workbench_scripts_submit(&command)==ESP_ERR_INVALID_ARG);
    command=write_intent(false); ++command.revision; assert(ryz_workbench_scripts_submit(&command)==ESP_ERR_INVALID_STATE);
    command=write_intent(false); command.name[0]=command.sha256[0]=0; /* Old bulk intent is never accepted. */
    assert(ryz_workbench_scripts_submit(&command)==ESP_ERR_INVALID_ARG);
    command.action=RYZ_V5_SCRIPTS_ACTION_RESTART; assert(ryz_workbench_scripts_submit(&command)==ESP_ERR_NOT_SUPPORTED);
    assert(!removes && !copies && !guard_begins);
    ryz_workbench_scripts_owner_tick(true,picker,RYZ_V5_SCRIPTS_PICKER);
    command=(ryz_v5_scripts_intent_t){.action=RYZ_V5_SCRIPTS_ACTION_PAGE,.generation=snapshot().generation,.revision=3,.limit=4};
    assert(ryz_workbench_scripts_submit(&command)==ESP_ERR_INVALID_ARG);
    command.limit=16; command.scroll_y=-1; assert(ryz_workbench_scripts_submit(&command)==ESP_ERR_INVALID_ARG);
    command.scroll_y=1; command.sha256[0]='a'; assert(ryz_workbench_scripts_submit(&command)==ESP_ERR_INVALID_ARG);
    command.sha256[0]=0; assert(ryz_workbench_scripts_submit(&command)==ESP_OK);
}
int main(int argc,char **argv) {
    assert(argc==2); const char *s=argv[1];
    if(!strcmp(s,"catalog")) catalog();
    else if(!strcmp(s,"selection")) selection();
    else if(!strcmp(s,"boot-retry")) boot_retry(false);
    else if(!strcmp(s,"boot-retry-bound")) boot_retry(true);
    else if(!strncmp(s,"boot-",5)) boot_read(s);
    else if(!strcmp(s,"invalid-source")) invalid_source();
    else if(!strcmp(s,"strict-fields")) strict_fields();
    else if(!strcmp(s,"clear")) write_success(false,false);
    else if(!strcmp(s,"copy")) write_success(true,false);
    else if(!strcmp(s,"publication-busy")) write_success(false,true);
    else if(!strcmp(s,"clear-inflight")) inflight(false);
    else if(!strcmp(s,"copy-inflight")) inflight(true);
    else if(!strncmp(s,"clear-",6)) write_failure(s,false);
    else if(!strncmp(s,"copy-",5)) write_failure(s,true);
    else assert(!"unknown scenario");
    printf("WORKBENCH_SCRIPTS_PASS %s\n",s); return 0;
}
