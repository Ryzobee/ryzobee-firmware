/* Real time core and ESP SNTP adapter, driven through the registered SDK
 * callback. No network sockets, wall-clock writes or device access. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ryz_time.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

#ifdef NDEBUG
#error "Time ESP assertions must remain active"
#endif

struct host_mutex { pthread_mutex_t native; bool live; };
static struct host_mutex snapshot_mutex={.native=PTHREAD_MUTEX_INITIALIZER};
static pthread_mutex_t observer_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t observer_changed=PTHREAD_COND_INITIALIZER;
static bool callback_waiting;
static atomic_bool watch_blocking;
static atomic_int_fast64_t monotonic_us;
static atomic_uint try_calls,blocking_calls;
static unsigned init_calls,start_calls;
static esp_sntp_config_t registered;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    assert(!snapshot_mutex.live);
    snapshot_mutex.live=true;
    return &snapshot_mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex,TickType_t ticks)
{
    assert(mutex==&snapshot_mutex && mutex->live);
    if(ticks==0) {
        atomic_fetch_add(&try_calls,1);
        int result=pthread_mutex_trylock(&mutex->native);
        assert(result==0 || result==EBUSY);
        return result==0?pdTRUE:pdFALSE;
    }
    assert(ticks==portMAX_DELAY);
    atomic_fetch_add(&blocking_calls,1);
    if(atomic_load(&watch_blocking)) {
        assert(!pthread_mutex_lock(&observer_lock));
        callback_waiting=true;
        assert(!pthread_cond_broadcast(&observer_changed));
        assert(!pthread_mutex_unlock(&observer_lock));
    }
    assert(!pthread_mutex_lock(&mutex->native));
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex==&snapshot_mutex && mutex->live);
    assert(!pthread_mutex_unlock(&mutex->native));
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    assert(mutex==&snapshot_mutex && mutex->live);
    mutex->live=false;
}

int64_t esp_timer_get_time(void)
{ return atomic_load(&monotonic_us); }

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    assert(config && config->sync_cb && !config->smooth_sync && !config->wait_for_sync);
    assert(config->start && config->num_of_servers==1);
    assert(!strcmp(config->servers[0],"fixture.invalid"));
    /* Starting the SDK must not retain the core snapshot mutex. */
    assert(!pthread_mutex_trylock(&snapshot_mutex.native));
    assert(!pthread_mutex_unlock(&snapshot_mutex.native));
    ++init_calls;
    registered=*config;
    return ESP_OK;
}

esp_err_t esp_netif_sntp_start(void)
{ ++start_calls; return ESP_OK; }

static void start(void)
{
    atomic_store(&monotonic_us,1000000);
    assert(ryz_time_init()==ESP_OK);
    assert(ryz_time_set_network_ready(true)==ESP_OK);
    assert(init_calls==1 && !start_calls && registered.sync_cb);
}

static void sync_at(int64_t seconds,int32_t microseconds,int64_t mono)
{
    struct timeval value={.tv_sec=(time_t)seconds,.tv_usec=(suseconds_t)microseconds};
    atomic_store(&monotonic_us,mono);
    registered.sync_cb(&value);
}

static ryz_time_utc_sample_t sample_at(int64_t mono)
{
    atomic_store(&monotonic_us,mono);
    ryz_time_utc_sample_t out={.unix_seconds=INT64_MAX,.valid=true};
    const unsigned inits=init_calls,starts=start_calls;
    unsigned tries=atomic_load(&try_calls),blocks=atomic_load(&blocking_calls);
    assert(ryz_time_sample_utc(&out)==ESP_OK);
    assert(init_calls==inits && start_calls==starts);
    assert(atomic_load(&try_calls)==tries+1 && atomic_load(&blocking_calls)==blocks);
    return out;
}

static void known(int64_t mono,int64_t seconds)
{
    ryz_time_utc_sample_t out=sample_at(mono);
    assert(out.valid && out.unix_seconds==seconds);
}

static void unknown_at(int64_t mono)
{
    ryz_time_utc_sample_t out=sample_at(mono);
    assert(!out.valid && !out.unix_seconds);
}

static void unknown(void)
{
    ryz_time_utc_sample_t out={.unix_seconds=INT64_MAX,.valid=true};
    assert(ryz_time_sample_utc(NULL)==ESP_ERR_INVALID_ARG);
    assert(ryz_time_sample_utc(&out)==ESP_ERR_INVALID_STATE);
    assert(!out.valid && !out.unix_seconds && !init_calls && !start_calls);
    assert(ryz_time_init()==ESP_OK);
    unknown_at(1);
    unknown_at(2000000);
    assert(!init_calls && !start_calls);
}

static void subseconds(void)
{
    start();
    const int64_t seconds=RYZ_TIME_MIN_VALID_UNIX_SECONDS+5;
    sync_at(seconds,900000,1000000);
    known(1000000,seconds);
    known(1099999,seconds);
    known(1100000,seconds+1);
    assert(ryz_time_set_network_ready(false)==ESP_OK);
    known(2100000,seconds+2);
    known(12100000,seconds+12);
    assert(init_calls==1 && !start_calls);
    ryz_time_snapshot_t status;
    assert(ryz_time_get_snapshot(&status)==ESP_OK);
    assert(!status.network_ready && status.clock_valid && status.last_sync_unix==seconds);
}

static void invalid(void)
{
    start();
    const int64_t seconds=RYZ_TIME_MIN_VALID_UNIX_SECONDS+20;
    const struct { int64_t sec; int32_t usec; } bad[]={
        {RYZ_TIME_MIN_VALID_UNIX_SECONDS-1,0},{-1,0},
        {RYZ_TIME_MIN_VALID_UNIX_SECONDS,-1},{RYZ_TIME_MIN_VALID_UNIX_SECONDS,1000000},
    };
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        sync_at(seconds,123456,1000000);
        known(1000000,seconds);
        sync_at(bad[i].sec,bad[i].usec,2000000);
        unknown_at(2000000); unknown_at(3000000);
        ryz_time_snapshot_t status;
        assert(ryz_time_get_snapshot(&status)==ESP_OK);
        assert(!status.clock_valid);
    }
    sync_at(seconds,0,4000000); known(4000000,seconds);
    registered.sync_cb(NULL);
    unknown_at(4000000); unknown_at(5000000);
    sync_at(seconds+50,999999,6000000);
    known(6000001,seconds+51);
}

static void clock_boundaries(void)
{
    start();
    const int64_t seconds=RYZ_TIME_MIN_VALID_UNIX_SECONDS+30;
    sync_at(seconds,0,1000000);
    known(2000000,seconds+1);
    unknown_at(1999999); /* A rollback after a successful sample is also unsafe. */
    unknown_at(3000000);
    sync_at(seconds+10,0,4000000);
    known(4000000,seconds+10);
    sync_at(seconds,0,-1);
    unknown_at(5000000);
    sync_at(INT64_MAX,999999,6000000);
    known(6000000,INT64_MAX);
    unknown_at(6000001); unknown_at(6000000); /* Overflow does not revive on a clock rollback. */
    sync_at(seconds,0,INT64_MAX-1000000);
    known(INT64_MAX,seconds+1);
    sync_at(seconds+20,0,0);
    known(0,seconds+20);
}

static void *deliver(void *context)
{
    registered.sync_cb(context);
    return NULL;
}

static void busy(void)
{
    start();
    sync_at(RYZ_TIME_MIN_VALID_UNIX_SECONDS,0,1000000);
    assert(!pthread_mutex_lock(&snapshot_mutex.native));
    unsigned tries=atomic_load(&try_calls),blocks=atomic_load(&blocking_calls);
    ryz_time_utc_sample_t out={.unix_seconds=INT64_MAX,.valid=true};
    assert(ryz_time_sample_utc(&out)==ESP_ERR_TIMEOUT);
    assert(!out.valid && !out.unix_seconds);
    assert(atomic_load(&try_calls)==tries+1 && atomic_load(&blocking_calls)==blocks);
    assert(!pthread_mutex_unlock(&snapshot_mutex.native));
    known(1000000,RYZ_TIME_MIN_VALID_UNIX_SECONDS);

    assert(!pthread_mutex_lock(&snapshot_mutex.native));
    atomic_store(&monotonic_us,10000000);
    atomic_store(&watch_blocking,true);
    struct timeval update={.tv_sec=(time_t)(RYZ_TIME_MIN_VALID_UNIX_SECONDS+100),.tv_usec=900000};
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,deliver,&update));
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME,&deadline)); deadline.tv_sec+=5;
    assert(!pthread_mutex_lock(&observer_lock));
    while(!callback_waiting)
        assert(!pthread_cond_timedwait(&observer_changed,&observer_lock,&deadline));
    assert(!pthread_mutex_unlock(&observer_lock));
    atomic_store(&monotonic_us,12000000);
    assert(!pthread_mutex_unlock(&snapshot_mutex.native));
    assert(!pthread_join(thread,NULL));
    atomic_store(&watch_blocking,false);
    /* The event happened two seconds before its lock could be acquired. */
    known(12000000,RYZ_TIME_MIN_VALID_UNIX_SECONDS+102);
    known(12100000,RYZ_TIME_MIN_VALID_UNIX_SECONDS+103);
}

int main(int argc,char **argv)
{
    assert(argc==2);
    if(!strcmp(argv[1],"unknown")) unknown();
    else if(!strcmp(argv[1],"subseconds")) subseconds();
    else if(!strcmp(argv[1],"invalid")) invalid();
    else if(!strcmp(argv[1],"clock")) clock_boundaries();
    else if(!strcmp(argv[1],"busy")) busy();
    else assert(false);
    printf("TIME_ESP_UTC_PASS %s\n",argv[1]);
    return 0;
}
