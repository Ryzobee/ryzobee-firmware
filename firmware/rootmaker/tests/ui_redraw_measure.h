#include <stdlib.h>
#include <time.h>
/* Pixel work, not elapsed host time, is the deterministic regression oracle.
 * Optional RGB565 dumps support before/after exact visual comparison. */
static uint16_t audit_frame[240*240];
static size_t audit_pixels;
static struct timespec audit_started;
static unsigned audit_global, audit_sequence;
static void audit_begin(void)
{
    memcpy(audit_frame,ryz_host_display_frame(),sizeof(audit_frame));
    audit_pixels=ryz_host_display_blit_pixels();
    clock_gettime(CLOCK_MONOTONIC,&audit_started);
}
static void audit_end(const char *name)
{
    struct timespec ended; clock_gettime(CLOCK_MONOTONIC,&ended);
    size_t changed=0,painted=ryz_host_display_blit_pixels()-audit_pixels;
    for(unsigned i=0;i<240*240;++i) changed+=audit_frame[i]!=ryz_host_display_frame()[i];
    double ms=(ended.tv_sec-audit_started.tv_sec)*1000.0+(ended.tv_nsec-audit_started.tv_nsec)/1000000.0;
    bool global=painted>=240U*240U || (!changed && painted);
    audit_global+=global;
    printf("AUDIT %-30s LVGL=%-6zu CHANGED=%-6zu HOST_MS=%.3f %s\n",name,painted,changed,ms,global?"EXCESS_REDRAW":"");
    const char *prefix=getenv("RYZ_REDRAW_DUMP_PREFIX");
    if(prefix) {
        char path[1024]; snprintf(path,sizeof(path),"%s-%02u.rgb565",prefix,audit_sequence);
        FILE *file=fopen(path,"wb"); assert(file);
        assert(fwrite(ryz_host_display_frame(),sizeof(audit_frame),1,file)==1);
        assert(fclose(file)==0);
    }
    ++audit_sequence;
#ifdef RYZ_REDRAW_FULL_ORACLE
    memcpy(audit_frame,ryz_host_display_frame(),sizeof(audit_frame));
    audit_full_render();
    assert(!memcmp(audit_frame,ryz_host_display_frame(),sizeof(audit_frame)));
#endif
}
