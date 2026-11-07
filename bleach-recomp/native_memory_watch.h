/* Windows dirty-page tracking for immutable-between-writes graphics sources.
 * One render-thread reader. Writers need no instrumentation. Query and reset
 * are one OS operation, and page epochs retain dirtiness for aliased textures. */
#ifndef BLEACH_NATIVE_MEMORY_WATCH_H
#define BLEACH_NATIVE_MEMORY_WATCH_H
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct bleach_memory_watch {
    uint8_t* base;
    size_t size, pages, page_size, capacity;
    uint64_t epoch;
    uint64_t* changed;
    void** addresses;
    int enabled;
} bleach_memory_watch;
static bleach_memory_watch bleach_watch;
static void bleach_watch_destroy(void)
{
    free(bleach_watch.changed); free(bleach_watch.addresses);
    memset(&bleach_watch,0,sizeof(bleach_watch));
}
static int bleach_watch_init(void* base, size_t size)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    bleach_watch_destroy();
    bleach_watch.base=(uint8_t*)base; bleach_watch.size=size;
    bleach_watch.page_size=si.dwPageSize;
    bleach_watch.pages=(size+si.dwPageSize-1)/si.dwPageSize;
    bleach_watch.changed=(uint64_t*)calloc(bleach_watch.pages,sizeof(uint64_t));
    bleach_watch.epoch=1;
    bleach_watch.enabled=bleach_watch.changed!=NULL;
    return bleach_watch.enabled;
}
/* Returns -1 for unknown, 0 for unchanged, 1 for possibly changed. The caller
 * commits the returned epoch only after validating/uploading its source. */
static int bleach_memory_changed(const void* source, size_t size, uint64_t* token)
{
    bleach_memory_watch* w=&bleach_watch;
    const uintptr_t address=(uintptr_t)source, base=(uintptr_t)w->base;
    if (!w->enabled || !source || !size || address<base || size>w->size || address-base>w->size-size)
        return -1;
    const size_t first=(address-base)/w->page_size;
    const size_t last=(address-base+size-1)/w->page_size;
    const size_t count=last-first+1;
    if (count>w->capacity) {
        void** grown=(void**)realloc(w->addresses,count*sizeof(void*));
        if (!grown) { w->enabled=0; return -1; }
        w->addresses=grown; w->capacity=count;
    }
    ULONG_PTR found=count;
    DWORD granularity=0;
    UINT error=GetWriteWatch(WRITE_WATCH_FLAG_RESET,w->base+first*w->page_size,
        count*w->page_size,w->addresses,&found,&granularity);
    if (error || granularity!=w->page_size || w->epoch==UINT64_MAX) {
        /* Never trust prior tokens after an incomplete/failed reset. */
        w->enabled=0; return -1;
    }
    if (found) {
        ++w->epoch;
        for (ULONG_PTR i=0;i<found;i++) {
            const uintptr_t p=(uintptr_t)w->addresses[i];
            if (p<base || p-base>=w->size) { w->enabled=0; return -1; }
            w->changed[(p-base)/w->page_size]=w->epoch;
        }
    }
    int dirty=*token==0;
    for (size_t i=first;i<=last;i++) if (w->changed[i]>*token) { dirty=1; break; }
    *token=w->epoch;
    return dirty;
}
#endif
