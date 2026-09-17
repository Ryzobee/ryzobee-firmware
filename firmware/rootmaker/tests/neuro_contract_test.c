#include "nervous_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { max_align_t align; size_t size; } allocation_t;
static size_t live;
static void *resize(void *ptr, size_t size) {
    allocation_t *old = ptr ? (allocation_t *)ptr - 1 : NULL;
    size_t previous = old ? old->size : 0;
    if (!size) { live -= previous; free(old); return NULL; }
    allocation_t *next = realloc(old, sizeof(*next) + size);
    if (!next) return NULL;
    next->size = size; live = live - previous + size; return next + 1;
}
typedef struct { uint64_t clock, cancel_at, end_at, paint_at; unsigned delay, starts, polls, cleanups, heartbeat; int painted, privacy, latest, expiry; bool fail_paint; } fixture_t;
static uint64_t now(void *p) { return ((fixture_t *)p)->clock; }
static void pause_ms(void *p, unsigned ms) { ((fixture_t *)p)->clock += ms ? ms : 1; }
static bool cancelled(void *p) { fixture_t *f = p; return f->cancel_at && f->clock >= f->cancel_at; }
static bool finished(void *p) { fixture_t *f = p; return f->clock >= f->end_at; }
static int sample(void *p) { (void)p; return 0; }
static int paint(void *p, uint16_t color, bool begin) { fixture_t *f = p; (void)color; if (begin) { f->starts++; f->paint_at = f->clock + f->delay; } else f->polls++; return f->fail_paint ? -1 : f->clock >= f->paint_at; }
static void cleanup(void *p) { ((fixture_t *)p)->cleanups++; }
static void observe(void *p, const char *id, const char *state, int32_t value) { fixture_t *f = p; (void)state; if (!strcmp(id, "heartbeat")) f->heartbeat = value; if (!strcmp(id,"screen")) f->painted = value; if (!strcmp(id,"privacy")) f->privacy = value; if (!strcmp(id,"latest")) f->latest = value; if (!strcmp(id,"expiry")) f->expiry = value; }
static const char source[] =
"return {runtime='ryz-neuro/1',catalog='ryz-signals/1',nodes={"
"{id='input',kind='touch',inputs={},outputs={},code=[=[shared=99 while true do sleep(1) end]=]},"
"{id='screen',kind='display',inputs={{'color',3,'reject',0}},outputs={},code=[=[while true do local p,v=receive(-1) paint(v) mark(v) end]=]},"
"{id='sender',kind='center',inputs={},outputs={{'color',3}},code=[=[emit('color',42) while true do sleep(1) end]=]},"
"{id='heartbeat',kind='center',inputs={},outputs={},code=[=[local n=0 while true do n=n+1 mark(n) sleep(1) end]=]},"
"{id='privacy',kind='center',inputs={},outputs={},code=[=[mark(shared or 0) while true do sleep(1) end]=]}"
"},wires={{3,'color',2,'color'}}}";
static ryz_lua_result_t run_source(fixture_t *f, const char *code) {
    ryz_neuro_platform_t p = {.context=f,.resize=resize,.now_ms=now,.pause_ms=pause_ms,.cancelled=cancelled,.finished=finished,.sample=sample,.paint=paint,.cleanup=cleanup,.observe=observe};
    ryz_lua_result_t r; ryz_neuro_execute(code, strlen(code), "@review.lua", 0, &p, &r); assert(live==0); return r;
}
static ryz_lua_result_t run(fixture_t *f) { return run_source(f, source); }
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int32_t value;
    const uint8_t valid[]={2,0,1,0,1}, invalid[]={2,0,1,0,2}, unknown[]={7,0,0,0};
    assert(ryz_neuro_decode(valid,sizeof(valid),2,&value) && value==1);
    assert(!ryz_neuro_decode(valid,sizeof(valid)-1,2,&value));
    assert(!ryz_neuro_decode(invalid,sizeof(invalid),2,&value));
    assert(!ryz_neuro_decode(unknown,sizeof(unknown),7,&value));
    fixture_t complete={.end_at=60,.delay=20,.privacy=-1}; ryz_lua_result_t r=run(&complete);
    assert(r.ok && complete.painted==42 && complete.starts==1 && complete.polls>=20 && complete.heartbeat>=50 && complete.privacy==0 && complete.cleanups==1);
    printf("PASS pending-paint completion, cooperative heartbeat, private globals, cleanup; heartbeat=%u polls=%u\n",complete.heartbeat,complete.polls);
    fixture_t stop={.end_at=100,.cancel_at=20,.delay=50}; r=run(&stop);
    assert(!r.ok && !strcmp(r.phase,"stopped") && stop.painted==0 && stop.cleanups==1 && stop.heartbeat>=10);
    printf("PASS cancellation while native operation pending; virtual stop=%llu ms\n",(unsigned long long)stop.clock);
    fixture_t error={.end_at=60,.fail_paint=true}; r=run(&error);
    assert(!r.ok && strstr(r.error,"display operation failed") && error.cleanups==1);
    printf("PASS native error cleanup\n");
    for (unsigned i=0;i<50;i++) { fixture_t repeat={.end_at=5,.delay=1}; r=run(&repeat); assert(r.ok); }
    printf("PASS 50 VM lifecycles with zero outstanding tracked bytes\n");
    const char policy_source[] =
    "return {runtime='ryz-neuro/1',catalog='ryz-signals/1',nodes={"
    "{id='input',kind='touch',inputs={},outputs={},code=[=[while true do sleep(1) end]=]},"
    "{id='screen',kind='display',inputs={},outputs={},code=[=[while true do sleep(1) end]=]},"
    "{id='sender',kind='center',inputs={},outputs={{'count',4}},code=[=[emit('count',1) emit('count',2) emit('count',3)]=]},"
    "{id='latest',kind='center',inputs={{'count',4,'latest',0}},outputs={},code=[=[sleep(10) local p,v=receive(-1) mark(v)]=]},"
    "{id='expiry',kind='center',inputs={{'count',4,'reject',5}},outputs={},code=[=[sleep(10) local p=receive(1) if p=='timeout' then mark(1) else mark(2) end]=]}"
    "},wires={{3,'count',4,'count'},{3,'count',5,'count'}}}";
    fixture_t policies={.end_at=30}; r=run_source(&policies,policy_source);
    assert(r.ok && policies.latest==3 && policies.expiry==1 && policies.cleanups==1);
    assert(strstr(r.output,"delivered=6 expired=3 merged=2"));
    printf("PASS per-subscriber latest merge and optional expiry; %s",r.output);
    const char loop_source[] =
    "return {runtime='ryz-neuro/1',catalog='ryz-signals/1',nodes={"
    "{id='input',kind='touch',inputs={},outputs={},code=[=[while true do end]=]},"
    "{id='screen',kind='display',inputs={},outputs={},code=[=[sleep(1)]=]}},wires={}}";
    fixture_t loop={.end_at=30}; r=run_source(&loop,loop_source);
    assert(!r.ok && strstr(r.error,"instruction budget exceeded") && loop.cleanups==1);
    printf("PASS non-yielding node fails within bounded instruction budget\n");
    return 0;
}
