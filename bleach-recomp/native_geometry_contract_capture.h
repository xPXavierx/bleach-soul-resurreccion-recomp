#ifndef BLEACH_NATIVE_GEOMETRY_CONTRACT_CAPTURE_H
#define BLEACH_NATIVE_GEOMETRY_CONTRACT_CAPTURE_H
/* Diagnostic-only complete phase pairs. These snapshots are offline oracles,
 * never the native runtime's data interface or a cache used to skip work. */
#include "spu_context.h"
#include <atomic>
#include <mutex>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
namespace bleach_geometry_contract_capture {
static constexpr unsigned per_phase=32, phases=3, capacity=per_phase*phases;
struct Snapshot { u128 gpr[128]; unsigned char ls[SPU_LS_SIZE]; };
struct Record {
    uint32_t index,target,return_pc,image,mxcsr,status_before,status_after,reserved;
    uint64_t key,padding;
    Snapshot before,after;
};
struct Slot { std::atomic<bool> ready{false}; Record data; };
static_assert(sizeof(Snapshot)==264192 && sizeof(Record)==528432);
static Slot* slots;
static unsigned counts[phases]{};
static std::mutex claim_mutex;
static thread_local Slot* active[phases]{};
static uint64_t origin,start_ms,end_ms;
static std::string path;
static void (*previous)(spu_context*,unsigned,uint32_t,uint32_t,int);
static int phase_index(uint32_t target) {
    return target==0x9828?0:(target==0x98e0?1:(target==0x5110?2:-1));
}
static void copy(Snapshot& dst,const spu_context* ctx) {
    memcpy(dst.gpr,ctx->gpr,sizeof(dst.gpr));memcpy(dst.ls,ctx->ls,sizeof(dst.ls));
}
static void observe(spu_context* ctx,unsigned index,uint32_t target,uint32_t ret,int after) {
    if(previous)previous(ctx,index,target,ret,after);
    const int p=phase_index(target);if(p<0)return;
    if(after) {
        Slot* s=active[p];if(!s)return;
        if(s->data.target!=target || s->data.return_pc!=ret)abort();
        copy(s->data.after,ctx);s->data.status_after=ctx->status;
        active[p]=nullptr;s->ready.store(true,std::memory_order_release);return;
    }
    if(active[p])abort();
    const uint64_t elapsed=GetTickCount64()-origin;
    if(elapsed<start_ms || elapsed>=end_ms || ctx->ls!=ctx->ls_store)return;
    const uint32_t at=ctx->gpr[3]._u32[0];if(at>SPU_LS_SIZE-416)return;
    // Diverse descriptor headers and source content, not changing LS addresses.
    uint64_t key=UINT64_C(0xcbf29ce484222325);
    for(unsigned i=0;i<16;i++)key=(key^ctx->ls[at+i])*UINT64_C(0x100000001b3);
    const uint32_t input=ctx->gpr[4]._u32[0];
    if(input<=SPU_LS_SIZE-48)
        for(unsigned i=0;i<48;i++)key=(key^ctx->ls[input+i])*UINT64_C(0x100000001b3);
    Slot* s=nullptr;
    { std::lock_guard<std::mutex> guard(claim_mutex);
      if(counts[p]>=per_phase)return;
      for(unsigned i=0;i<counts[p];i++)if(slots[p*per_phase+i].data.key==key)return;
      s=&slots[p*per_phase+counts[p]++];s->data.key=key; }
    auto& r=s->data;r.index=index;r.target=target;r.return_pc=ret;
    r.image=ctx->image_id;r.mxcsr=_mm_getcsr();r.status_before=ctx->status;r.reserved=0;r.padding=0;
    copy(r.before,ctx);active[p]=s;
}
static void dump() {
    if(!slots)return;
    FILE* f=fopen(path.c_str(),"wb");if(!f){fprintf(stderr,"[geometry-contract] write failed\n");abort();}
    const char magic[8]={'G','E','O','C','T','R','1',0};
    uint32_t count=0,bytes=sizeof(Record);
    for(unsigned i=0;i<capacity;i++)if(slots[i].ready.load(std::memory_order_acquire))++count;
    fwrite(magic,1,8,f);fwrite(&bytes,4,1,f);fwrite(&count,4,1,f);
    for(unsigned i=0;i<capacity;i++)if(slots[i].ready.load(std::memory_order_acquire))
        if(fwrite(&slots[i].data,sizeof(Record),1,f)!=1)abort();
    if(fclose(f))abort();
    fprintf(stderr,"[geometry-contract] records=%u bytes=%u phases=%u,%u,%u path=%s\n",
        count,bytes,counts[0],counts[1],counts[2],path.c_str());
}
static void initialize() {
    const char* env=getenv("BLEACH_GEOMETRY_CONTRACT_CAPTURE");if(!env || !*env)return;
    unsigned start,duration;int used=0;
    if(sscanf(env,"%u:%u:%n",&start,&duration,&used)!=2 || !duration || !used || !env[used])abort();
    path=env+used;start_ms=start;end_ms=uint64_t(start)+duration;origin=GetTickCount64();
    slots=new Slot[capacity];previous=bleach_geometry_phase_observer;
    bleach_geometry_phase_observer=observe;atexit(dump);
}
}
#endif
