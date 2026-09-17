/* Real production policy and SDK cJSON. Only the heap-capability boundary is
 * replaced. Separate processes keep all cases free of inherited global hooks. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "ryz_json_memory.h"

#define POINTERS 1024U
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static void *pointers[POINTERS];
static size_t live, allocations, attempts, releases;
static long budget=-1;
static unsigned installs;
static cJSON_Hooks installed;

void cJSON_RealInitHooks(cJSON_Hooks *hooks);
void cJSON_InitHooks(cJSON_Hooks *hooks)
{
    assert(hooks && hooks->malloc_fn && hooks->free_fn==heap_caps_free);
    assert(installs++==0); /* No hook mutation after the startup installation. */
    installed=*hooks;
    cJSON_RealInitHooks(hooks);
}

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    assert(!(caps&MALLOC_CAP_INTERNAL));
    assert(!pthread_mutex_lock(&guard));
    ++attempts;
    if(budget==0) { assert(!pthread_mutex_unlock(&guard)); return NULL; }
    if(budget>0) --budget;
    void *pointer=malloc(size);
    assert(pointer);
    unsigned slot=0;
    while(slot<POINTERS && pointers[slot]) ++slot;
    assert(slot<POINTERS);
    pointers[slot]=pointer; ++live; ++allocations;
    assert(!pthread_mutex_unlock(&guard));
    return pointer;
}

static void require_owned(void *pointer)
{
    assert(pointer && !pthread_mutex_lock(&guard));
    unsigned slot=0;
    while(slot<POINTERS && pointers[slot]!=pointer) ++slot;
    assert(slot<POINTERS);
    assert(!pthread_mutex_unlock(&guard));
}

static void forget_pointer(void *pointer)
{
    assert(!pthread_mutex_lock(&guard));
    unsigned slot=0;
    while(slot<POINTERS && pointers[slot]!=pointer) ++slot;
    assert(slot<POINTERS && live);
    pointers[slot]=NULL; --live; ++releases;
    assert(!pthread_mutex_unlock(&guard));
}

void heap_caps_free(void *pointer)
{
    if(!pointer) return;
    forget_pointer(pointer);
    free(pointer);
}

static void balanced(void)
{
    assert(!live && allocations==releases && installs==1);
    for(unsigned i=0;i<POINTERS;++i) assert(!pointers[i]);
}

static cJSON *document(void)
{
    cJSON *root=cJSON_CreateObject();
    assert(root);
    assert(cJSON_AddBoolToObject(root,"ready",true));
    assert(cJSON_AddStringToObject(root,"name","RootMaker"));
    cJSON *array=cJSON_AddArrayToObject(root,"samples");
    assert(array);
    for(unsigned i=0;i<8;++i) assert(cJSON_AddItemToArray(array,cJSON_CreateNumber(i)));
    return root;
}

static void lifecycle(void)
{
    const char text[]="{\"ready\":true,\"n\":42.5,\"message\":\"A\\u0042\",\"items\":[1,null,false]}";
    const char *end=NULL;
    cJSON *parsed=cJSON_ParseWithOpts(text,&end,true);
    assert(parsed && end && !*end);
    assert(cJSON_GetObjectItemCaseSensitive(parsed,"n")->valuedouble==42.5);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(parsed,"message")->valuestring,"AB"));
    cJSON *root=document();
    assert(cJSON_AddItemToObject(root,"parsed",parsed));
    char long_value[2049]; memset(long_value,'x',sizeof(long_value)-1); long_value[2048]=0;
    assert(cJSON_AddStringToObject(root,"long",long_value));
    cJSON *copy=cJSON_Duplicate(root,true); assert(copy);
    char *pretty=cJSON_Print(copy); require_owned(pretty);
    assert(strstr(pretty,"RootMaker") && strstr(pretty,long_value));
    cJSON_free(pretty);
    char *compact=cJSON_PrintBuffered(root,1,false); require_owned(compact);
    assert(strstr(compact,"\"ready\":true") && strstr(compact,long_value));
    cJSON_free(compact);
    char buffer[4096]; assert(cJSON_PrintPreallocated(root,buffer,sizeof(buffer),false));
    void *raw=cJSON_malloc(128); require_owned(raw); cJSON_free(raw);
    cJSON_Delete(copy); cJSON_Delete(root);
    assert(allocations>30); balanced();
}

static void output_free(void)
{
    cJSON *root=document();
    char *first=cJSON_PrintUnformatted(root); require_owned(first);
    cJSON_free(first);
    char *second=cJSON_PrintUnformatted(root); require_owned(second);
    /* Account for a caller releasing the raw malloc-compatible output. The
     * Host proves pointer/lifetime safety, not ESP-IDF's PSRAM free routing. */
    forget_pointer(second);
    free(second);
    cJSON_Delete(root); balanced();
}

static void failure(void)
{
    budget=0;
    assert(!cJSON_CreateObject() && !cJSON_CreateString("value"));
    assert(!cJSON_Parse("{\"value\":42}") && !cJSON_malloc(16));
    assert(attempts>=4 && !allocations);
    const char source[]="{\"a\":[1,2,3],\"b\":\"text\",\"c\":{\"d\":true}}";
    unsigned partial_failures=0;
    for(long limit=0;limit<32;++limit) {
        budget=limit;
        cJSON *root=cJSON_Parse(source);
        if(!root) ++partial_failures;
        cJSON_Delete(root); balanced();
    }
    assert(partial_failures>8);
    budget=-1; cJSON *root=document(); const size_t before=live;
    budget=0;
    assert(!cJSON_PrintUnformatted(root) && live==before);
    assert(!cJSON_AddStringToObject(root,"later","not stored") && live==before);
    /* Print's initial allocation succeeds, but growth must still fail closed. */
    budget=1;
    assert(!cJSON_PrintBuffered(root,1,false) && live==before);
    cJSON_Delete(root); balanced();
}

static void *worker(void *context)
{
    const uintptr_t id=(uintptr_t)context;
    for(unsigned iteration=0;iteration<300;++iteration) {
        cJSON *root=document();
        assert(cJSON_AddNumberToObject(root,"worker",(double)id));
        char *text=cJSON_PrintUnformatted(root); require_owned(text);
        assert(strstr(text,"\"ready\":true") && strstr(text,"RootMaker"));
        cJSON_free(text); cJSON_Delete(root);
    }
    return NULL;
}

static void threads(void)
{
    /* Do not race Parse/GetErrorPtr: this SDK's parser writes a global error
     * slot even on success. Independent create/print/delete uses fixed hooks. */
    pthread_t workers[6];
    for(uintptr_t i=0;i<6;++i) assert(!pthread_create(&workers[i],NULL,worker,(void *)i));
    for(unsigned i=0;i<6;++i) assert(!pthread_join(workers[i],NULL));
    balanced();
}

int main(int argc, char **argv)
{
    assert(argc==2);
    ryz_json_memory_init(); ryz_json_memory_init();
    assert(installs==1 && !attempts && installed.malloc_fn && installed.free_fn==heap_caps_free);
    if(!strcmp(argv[1],"lifecycle")) lifecycle();
    else if(!strcmp(argv[1],"output-free")) output_free();
    else if(!strcmp(argv[1],"failure")) failure();
    else if(!strcmp(argv[1],"threads")) threads();
    else assert(!"unknown case");
    printf("JSON_MEMORY_PASS %s\n",argv[1]);
    return 0;
}
