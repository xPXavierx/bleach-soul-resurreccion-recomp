#include "rsx_fifo_wait.h"
#include "ps3emu/ps3_write_watch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
extern unsigned char* vm_base;
typedef struct {
    uint64_t armed, ready, consumed, slept, ready_sleep, last_end;
    uint32_t kind, ea, value, equal_ready, frame_start, frame_end;
    uint32_t source, waits, timeouts, flags;
} Wait;
typedef struct { uint32_t kind,ea,value,equal_ready; } Dependency;
/* flags: 1 ready when armed (timestamp lower bound), 2 changed without observed
 * write, 4 replaced while blocked, 8 truncated at window end, 16 ready revoked. */
static SRWLOCK lock = SRWLOCK_INIT;
static Dependency dependency;
static Wait current, *records;
static uint32_t count, dropped;
static uint64_t origin_ms, start_ms, end_ms, frequency;
static char path[1024];
static int initialized, enabled, finished, wake_enabled;
static void (*wake_consumer)(void);
static uint64_t stamp(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static uint32_t word(uint32_t ea) { uint32_t raw; memcpy(&raw,vm_base+ea,4); return __builtin_bswap32(raw); }
static int ready(void) { return (word(dependency.ea)==dependency.value)==(int)dependency.equal_ready; }
static void disarm(void)
{
    __atomic_store_n(&g_ps3_write_watch.addr,0,__ATOMIC_RELEASE);
    memset(&dependency,0,sizeof dependency);
}
static void save(uint64_t now, uint32_t frame, uint32_t flags)
{
    if (!current.armed) return;
    current.consumed=now; current.frame_end=frame; current.flags|=flags;
    if(count<65536) records[count++]=current; else ++dropped;
    memset(&current,0,sizeof current);
}
static void commit(uint32_t ea,uint32_t size,uint32_t source)
{
    AcquireSRWLockExclusive(&lock);
    if(dependency.ea && (uint64_t)ea<(uint64_t)dependency.ea+4 && (uint64_t)ea+size>dependency.ea) {
        if(ready()) {
            if(current.armed && !current.ready) { current.ready=stamp();current.source=source; }
            /* The write has committed. The retained event permits this signal
             * before WaitForSingleObject; it is never a GPU completion signal. */
            if(wake_enabled)wake_consumer();
        } else if(current.ready) {
            current.ready=0;current.source=0;current.flags|=16;
        }
    }
    ReleaseSRWLockExclusive(&lock);
}
static void dump(void)
{
    if(!enabled)return;
    AcquireSRWLockExclusive(&lock);
    if(finished){ReleaseSRWLockExclusive(&lock);return;}
    finished=1;save(stamp(),current.frame_start,8);
    if(!wake_enabled)disarm();
    FILE* f=fopen(path,"w");
    if(f) {
        fprintf(f,"# frequency=%llu start_ms=%llu duration_ms=%llu records=%u dropped=%u\n",
            (unsigned long long)frequency,(unsigned long long)start_ms,
            (unsigned long long)(end_ms-start_ms),count,dropped);
        fprintf(f,"armed,ready,consumed,slept,ready_sleep,last_wait_end,kind,ea,value,equal_ready,frame_start,frame_end,source,waits,timeouts,flags\n");
        for(uint32_t i=0;i<count;i++) { Wait* r=records+i;
            fprintf(f,"%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
              (unsigned long long)r->armed,(unsigned long long)r->ready,(unsigned long long)r->consumed,
              (unsigned long long)r->slept,(unsigned long long)r->ready_sleep,(unsigned long long)r->last_end,
              r->kind,r->ea,r->value,r->equal_ready,r->frame_start,r->frame_end,r->source,r->waits,r->timeouts,r->flags);
        }
        fclose(f);
    }
    fprintf(stderr,"[fifo-wait-audit] records=%u dropped=%u output=%s status=%s\n",count,dropped,path,f?"ok":"failed");
    ReleaseSRWLockExclusive(&lock);
}
void rsx_fifo_wait_init(void (*wake)(void))
{
    if(initialized)return; initialized=1;
    const char* w=getenv("GCM_FIFO_PUBLISH_WAKE");wake_enabled=w && !strcmp(w,"1") && wake;
    wake_consumer=wake;
    const char* e=getenv("GCM_FIFO_WAIT_AUDIT");
    if(e) {
        unsigned start,duration;int used=0;
        if(sscanf(e,"%u:%u:%n",&start,&duration,&used)!=2 || !duration || !used || !e[used] || strlen(e+used)>=sizeof path)abort();
        records=calloc(65536,sizeof(Wait));if(!records)abort();
        strcpy(path,e+used);start_ms=start;end_ms=(uint64_t)start+duration;origin_ms=GetTickCount64();
        LARGE_INTEGER f;QueryPerformanceFrequency(&f);frequency=f.QuadPart;
        enabled=1;atexit(dump);
    }
    /* Callback has static process lifetime. Publishing addr later releases it. */
    if(wake_enabled || enabled)g_ps3_write_watch.commit=commit;
    if(wake_enabled)__atomic_store_n(&g_ps3_write_watch.ordered_dma,1,__ATOMIC_RELEASE);
    fprintf(stderr,"[fifo-publication] wake=%d audit=%d fallback_timeout=preserved\n",wake_enabled,enabled);
}
static int active(void)
{
    if(!enabled || finished)return 0;
    uint64_t elapsed=GetTickCount64()-origin_ms;
    if(elapsed>=end_ms){dump();return 0;}
    return elapsed>=start_ms;
}
void rsx_fifo_wait_resume(uint32_t frame)
{
    int audit=active();if(!wake_enabled && !audit)return;
    AcquireSRWLockExclusive(&lock);
    if(dependency.ea && ready()) {
        if(current.armed) {
            if(!current.ready)current.flags|=2;
            save(stamp(),frame,0);
        }
        disarm();
    }
    ReleaseSRWLockExclusive(&lock);
}
void rsx_fifo_wait_arm(uint32_t kind,uint32_t ea,uint32_t value,int equal,uint32_t frame)
{
    int audit=active();if(!ea || ea>UINT32_MAX-3 || (!wake_enabled && !audit))return;
    AcquireSRWLockExclusive(&lock);
    int same=dependency.kind==kind && dependency.ea==ea && dependency.value==value && dependency.equal_ready==(uint32_t)equal;
    if(!same) {
        if(current.armed)save(stamp(),frame,4);
        dependency=(Dependency){kind,ea,value,(uint32_t)equal};
        __atomic_exchange_n(&g_ps3_write_watch.addr,ea,__ATOMIC_SEQ_CST);
    }
    if(audit && !current.armed) {
        current.armed=stamp();current.kind=kind;current.ea=ea;current.value=value;
        current.equal_ready=equal;current.frame_start=frame;
    }
    /* Publish BEFORE re-reading. Ordered DMA/atomic publishers either see the
     * watch or this recheck sees the completed value. Other writers retain the
     * existing timeout fallback. A stale/spurious event never consumes data. */
    if(ready()) {
        if(current.armed && !current.ready){current.ready=stamp();current.flags|=1;}
        if(wake_enabled)wake_consumer();
    }
    ReleaseSRWLockExclusive(&lock);
}
uint64_t rsx_fifo_wait_begin(void)
{
    if(!active())return 0;
    AcquireSRWLockExclusive(&lock);
    uint64_t result=current.armed?stamp():0;
    ReleaseSRWLockExclusive(&lock);return result;
}
void rsx_fifo_wait_end(uint64_t begin,int timeout)
{
    if(!begin)return;
    uint64_t now=stamp();AcquireSRWLockExclusive(&lock);
    if(current.armed) {
        current.slept+=now-begin;current.last_end=now;++current.waits;
        if(timeout)++current.timeouts;
        uint64_t available=current.ready>begin?current.ready:begin;
        if(current.ready && available<now)current.ready_sleep+=now-available;
    }
    ReleaseSRWLockExclusive(&lock);
}
#else
void rsx_fifo_wait_init(void (*w)(void)){(void)w;}
void rsx_fifo_wait_resume(uint32_t f){(void)f;}
void rsx_fifo_wait_arm(uint32_t k,uint32_t a,uint32_t v,int e,uint32_t f){(void)k;(void)a;(void)v;(void)e;(void)f;}
uint64_t rsx_fifo_wait_begin(void){return 0;}
void rsx_fifo_wait_end(uint64_t b,int t){(void)b;(void)t;}
#endif
