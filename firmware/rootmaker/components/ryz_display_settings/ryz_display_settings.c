#include "ryz_display_settings_internal.h"
#include "display.h"
#include "touch.h"
#include <stdatomic.h>
#include <string.h>

static atomic_flag gate = ATOMIC_FLAG_INIT;
static atomic_bool initialized;
static ryz_display_settings_storage_t storage;
static ryz_display_settings_snapshot_t state;
static bool pending, working, result_ready, initial_load;
static bool preview_active, preview_pending;
static uint8_t preview_brightness;
static atomic_bool preview_end_requested;
typedef struct {
    ryz_display_settings_value_t value;
    bool apply, persisted, initial;
    esp_err_t error, load_error;
} worker_result_t;
static worker_result_t result, staged;
static bool staged_ready; /* Worker-private. */
/* These are exclusively owned by the SPI/input Owner. */
static uint64_t last_activity, next_light_retry;
static bool wake_guard;
static unsigned hardware_retries;
static bool apply_finished, light_finished, owner_asleep, owner_input_blocked, owner_light_uncertain;
static struct { ryz_display_settings_value_t value; bool initial,blocked; esp_err_t error; } applied;
static esp_err_t light_error;
static uint64_t next_preview_retry;
static bool owner_preview_blocked;

static bool take(void) { return !atomic_flag_test_and_set_explicit(&gate,memory_order_acquire); }
static void give(void) { atomic_flag_clear_explicit(&gate,memory_order_release); }
static void changed(void) { if(state.revision != UINT64_MAX) ++state.revision; }

bool ryz_display_settings_valid(ryz_display_settings_value_t v)
{ return v.rotation<=3 && v.sleep_index<=4 && v.brightness>=10 && v.brightness<=100 && v.brightness%5==0; }
bool ryz_display_settings_equal(ryz_display_settings_value_t a,ryz_display_settings_value_t b)
{ return a.rotation==b.rotation && a.sleep_index==b.sleep_index && a.brightness==b.brightness; }
ryz_display_settings_value_t ryz_display_settings_defaults(void)
{ return (ryz_display_settings_value_t){.rotation=0,.sleep_index=4,.brightness=50}; }
uint32_t ryz_display_settings_timeout_ms(uint8_t index)
{ static const uint32_t timeouts[]={15000,30000,60000,300000,0}; return index<5?timeouts[index]:0; }

esp_err_t ryz_display_settings_core_init(const ryz_display_settings_storage_t *value)
{
    if(!value || !value->read || !value->write) return ESP_ERR_INVALID_ARG;
    if(atomic_load(&initialized)) return ESP_ERR_INVALID_STATE;
    storage=*value;
    state=(ryz_display_settings_snapshot_t){.revision=1,.phase=RYZ_DISPLAY_SETTINGS_LOADING,
        .confirmed=ryz_display_settings_defaults(),.requested=ryz_display_settings_defaults()};
    pending=true; initial_load=true;
    atomic_store(&initialized,true);
    return ESP_OK;
}

esp_err_t ryz_display_settings_get_snapshot(ryz_display_settings_snapshot_t *out)
{
    if(!out) return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));
    if(!atomic_load(&initialized)) return ESP_ERR_INVALID_STATE;
    if(!take()) return ESP_ERR_TIMEOUT;
    *out=state; give(); return ESP_OK;
}

esp_err_t ryz_display_settings_submit(uint64_t revision,ryz_display_settings_value_t value)
{
    if(!revision || !ryz_display_settings_valid(value)) return ESP_ERR_INVALID_ARG;
    if(!atomic_load(&initialized)) return ESP_ERR_INVALID_STATE;
    if(!take()) return ESP_ERR_TIMEOUT;
    esp_err_t error=ESP_OK;
    if(revision!=state.revision || state.revision==UINT64_MAX || state.operation_id==UINT64_MAX)
        error=ESP_ERR_INVALID_STATE;
    else if(pending || working || result_ready || state.phase==RYZ_DISPLAY_SETTINGS_APPLYING)
        error=ESP_ERR_TIMEOUT;
    else {
        ++state.operation_id; state.requested=value; state.persisted=false;
        state.phase=RYZ_DISPLAY_SETTINGS_SAVING; state.error=ESP_OK;
        preview_pending=false; /* SAVE owns the next applied tuple. */
        pending=true; initial_load=false; changed();
    }
    give(); return error;
}

esp_err_t ryz_display_settings_preview(uint64_t revision,uint8_t brightness)
{
    if(!revision || !ryz_display_settings_valid((ryz_display_settings_value_t){0,4,brightness}))
        return ESP_ERR_INVALID_ARG;
    if(!atomic_load(&initialized)) return ESP_ERR_INVALID_STATE;
    if(!take()) return ESP_ERR_TIMEOUT;
    esp_err_t error=ESP_OK;
    if(revision!=state.revision || !state.ready || state.input_blocked)
        error=ESP_ERR_INVALID_STATE;
    else if(pending || working || result_ready || state.phase==RYZ_DISPLAY_SETTINGS_APPLYING)
        error=ESP_ERR_TIMEOUT;
    else {
        preview_brightness=brightness;
        preview_active=preview_pending=true;
        next_preview_retry=0;
        atomic_store(&preview_end_requested,false);
    }
    give(); return error;
}

void ryz_display_settings_end_preview(void)
{ atomic_store(&preview_end_requested,true); }

void ryz_display_settings_worker_tick(void)
{
    if(!atomic_load(&initialized) || !take()) return;
    if(staged_ready) {
        result=staged; result_ready=true; staged_ready=false; working=false;
        give(); return;
    }
    if(!pending || working || result_ready) { give(); return; }
    ryz_display_settings_value_t desired=state.requested;
    bool initial=initial_load;
    pending=false; working=true; give();
    ryz_display_settings_value_t stored={0};
    esp_err_t read_error=storage.read(&stored);
    if(read_error==ESP_OK && !ryz_display_settings_valid(stored)) read_error=ESP_ERR_INVALID_ARG;
    bool apply=false,persisted=false;
    esp_err_t error=ESP_OK,load_error=ESP_OK;
    if(initial) {
        desired=read_error==ESP_OK?stored:ryz_display_settings_defaults();
        persisted=read_error==ESP_OK;
        load_error=read_error==ESP_ERR_NOT_FOUND?ESP_OK:read_error;
        apply=true;
    } else if(read_error!=ESP_OK && read_error!=ESP_ERR_NOT_FOUND && read_error!=ESP_ERR_INVALID_ARG) {
        /* In particular, an uncertain prior write cannot lead to another
         * write while its complete current tuple is still unreadable. */
        error=read_error;
    } else if(read_error==ESP_OK && ryz_display_settings_equal(stored,desired)) {
        persisted=true; apply=true;
    } else {
        error=storage.write(desired,&persisted);
        apply=error==ESP_OK && persisted;
        if(error==ESP_OK && !persisted) error=ESP_ERR_INVALID_STATE;
    }
    staged=(worker_result_t){.value=desired,.apply=apply,.persisted=persisted,
        .initial=initial,.error=error,.load_error=load_error};
    staged_ready=true;
    /* One bounded deferred publication, no spin against a preempted Owner. */
    if(take()) { result=staged; result_ready=true; staged_ready=false; working=false; give(); }
}

static void publish_light(bool asleep,esp_err_t error)
{
    owner_asleep=asleep; light_error=error; light_finished=true;
    owner_light_uncertain=error!=ESP_OK;
    if(!take()) return;
    bool blocked=owner_input_blocked || owner_light_uncertain || owner_preview_blocked;
    if(state.asleep!=asleep || state.error!=error || state.input_blocked!=blocked) {
        state.asleep=asleep;
        state.input_blocked=blocked;
        if(error!=ESP_OK) state.error=error;
        changed();
    }
    light_finished=false;
    give();
}

static bool publish_apply(void)
{
    if(!apply_finished) return true;
    if(!take()) return false;
    state.error=applied.error;
    if(applied.error==ESP_OK) {
        state.confirmed=applied.value; state.ready=true;
        state.phase=applied.initial?RYZ_DISPLAY_SETTINGS_READY:RYZ_DISPLAY_SETTINGS_SAVED;
        /* A committed SAVE supersedes the temporary preview and any old
         * route-exit request. Never restore the pre-SAVE brightness later. */
        preview_active=preview_pending=false;
        atomic_store(&preview_end_requested,false);
        state.preview_error=ESP_OK;
        owner_preview_blocked=false;
    } else state.phase=RYZ_DISPLAY_SETTINGS_APPLY_FAILED;
    state.input_blocked=applied.blocked || owner_light_uncertain || owner_preview_blocked;
    apply_finished=false; changed(); give(); return true;
}

static void owner_preview_tick(uint64_t now_ms,bool allowed)
{
    if(!allowed || !take()) return;
    bool ending=atomic_load(&preview_end_requested);
    if(!preview_active && ending) atomic_store(&preview_end_requested,false);
    if(!preview_active || (!preview_pending && !ending) || !state.ready ||
       pending || working || result_ready || state.phase==RYZ_DISPLAY_SETTINGS_APPLYING ||
       now_ms<next_preview_retry) { give(); return; }
    uint8_t brightness=ending?state.confirmed.brightness:preview_brightness;
    give();
    /* Same SPI/input Owner as panel rendering; only LEDC, no NVS/rotation.
     * The driver keeps asleep panels dark and rejects privacy barriers. */
    esp_err_t error=ryz_display_set_brightness(brightness);
    bool blocked=error!=ESP_OK && (ending || ryz_display_configuration_blocked());
    owner_preview_blocked=blocked;
    /* Preserve the Owner's rate limit even when publishing the result loses
     * the short snapshot lock to the persistence worker. */
    next_preview_retry=error==ESP_OK?0:now_ms+1000;
    if(!take()) {
        /* Leave the coalesced request intact; replay is idempotent. */
        return;
    }
    if(error==ESP_OK) {
        preview_pending=false;
        if(ending) {
            preview_active=false;
            atomic_store(&preview_end_requested,false);
        }
    } else {
        /* Recover uncertain PWM or an unfinished discard without writing
         * storage. One attempt per second, not a busy retry loop. */
        preview_pending=blocked;
    }
    bool input_blocked=owner_input_blocked || owner_light_uncertain || owner_preview_blocked;
    if(state.preview_error!=error || state.input_blocked!=input_blocked) {
        state.preview_error=error; state.input_blocked=input_blocked; changed();
    }
    give();
}

bool ryz_display_settings_owner_tick(uint64_t now_ms,bool allow_idle_sleep,bool allow_configuration_apply)
{
    if(!atomic_load(&initialized)) return false;
    bool had_publication=apply_finished || light_finished;
    if(!publish_apply()) return true;
    if(light_finished) publish_light(owner_asleep,light_error);
    bool apply=false,initial=false;
    ryz_display_settings_value_t desired={0};
    uint64_t before=0;
    if(!take()) return false;
    before=state.revision;
    if(result_ready && allow_configuration_apply) {
        result_ready=false;
        state.requested=result.value; state.persisted=result.persisted;
        state.error=result.error; state.load_error=result.load_error;
        apply=result.apply; initial=result.initial; desired=result.value;
        state.phase=apply?RYZ_DISPLAY_SETTINGS_APPLYING:RYZ_DISPLAY_SETTINGS_UNKNOWN;
        hardware_retries=0; changed();
    } else if(allow_configuration_apply && state.phase==RYZ_DISPLAY_SETTINGS_APPLY_FAILED && state.input_blocked &&
              hardware_retries<3 && now_ms>=next_light_retry) {
        /* A failed rollback leaves no safe visible retry button. Retry only
         * the already-persisted hardware tuple, never write NVS again. */
        apply=true; desired=state.requested; ++hardware_retries;
    }
    give();
    if(apply) {
        esp_err_t error=ryz_display_apply_configuration(desired.rotation,desired.brightness);
        if(error==ESP_OK) error=ryz_touch_set_rotation(desired.rotation);
        bool blocked=ryz_display_configuration_blocked();
        owner_input_blocked=blocked;
        if(error==ESP_OK) owner_light_uncertain=false;
        wake_guard=true; last_activity=now_ms;
        next_light_retry=now_ms+1000;
        applied.value=desired; applied.initial=initial; applied.blocked=blocked; applied.error=error;
        apply_finished=true;
        if(!publish_apply()) return true;
    }
    owner_preview_tick(now_ms,allow_configuration_apply);
    ryz_display_settings_snapshot_t current;
    if(ryz_display_settings_get_snapshot(&current)!=ESP_OK) return true;
    if(!allow_idle_sleep) {
        last_activity=now_ms;
        if((owner_asleep || owner_light_uncertain) && now_ms>=next_light_retry) {
            esp_err_t error=ryz_display_set_asleep(false);
            if(error==ESP_OK) wake_guard=true;
            next_light_retry=now_ms+1000;
            publish_light(error==ESP_OK?false:current.asleep,error);
        }
    } else if(current.ready && !owner_asleep && !current.input_blocked &&
              current.phase!=RYZ_DISPLAY_SETTINGS_SAVING && current.phase!=RYZ_DISPLAY_SETTINGS_APPLYING &&
              now_ms>=next_light_retry) {
        uint32_t timeout=ryz_display_settings_timeout_ms(current.confirmed.sleep_index);
        if(timeout && now_ms>=last_activity && now_ms-last_activity>=timeout) {
            esp_err_t error=ryz_display_set_asleep(true);
            next_light_retry=now_ms+1000;
            publish_light(error==ESP_OK,error);
        }
    }
    if(ryz_display_settings_get_snapshot(&current)!=ESP_OK) return true;
    return had_publication || current.revision!=before;
}

bool ryz_display_settings_filter_touch(ryz_touch_sample_t *sample,esp_err_t error,uint64_t now_ms)
{
    /* All input/barrier facts are Owner-private. A worker/snapshot lock miss
     * must never eat a physical UP or synthesize a transport failure. */
    if(!atomic_load(&initialized)) return false;
    if(error!=ESP_OK || !sample) { wake_guard=true; return owner_asleep || owner_light_uncertain || owner_input_blocked || owner_preview_blocked || apply_finished; }
    if(sample->pressed) last_activity=now_ms;
    if(owner_input_blocked || owner_preview_blocked || apply_finished) { wake_guard=true; return true; }
    if(owner_asleep || owner_light_uncertain) {
        if(sample->pressed) {
            wake_guard=true;
            esp_err_t wake_error=ryz_display_set_asleep(false);
            publish_light(wake_error==ESP_OK?false:owner_asleep,wake_error);
        }
        return true;
    }
    if(wake_guard) {
        if(!sample->pressed) wake_guard=false;
        return true;
    }
    return false;
}
