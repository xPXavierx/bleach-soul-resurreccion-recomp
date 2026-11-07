#ifndef BLEACH_NATIVE_GEOMETRY_HANDOFF_HOST_H
#define BLEACH_NATIVE_GEOMETRY_HANDOFF_HOST_H
#include "rsx_geometry_handoff.h"
namespace bleach_geometry_handoff {
static thread_local rsx_geometry_lease pending;
static thread_local spu_context* owner;
static std::atomic<unsigned long long> attempts{0},published{0},declined{0};
static void cancel() {
    rsx_geometry_handoff_cancel(pending.token);pending={};owner=nullptr;
}
static void report() {
    fprintf(stderr,"[geometry-handoff-producer] attempts=%llu published=%llu declined=%llu\n",
        attempts.load(),published.load(),declined.load());
    rsx_geometry_handoff_report();
}
}
extern "C" {
int bleach_geometry_handoff_mode=0;
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
unsigned char* bleach_geometry_handoff_destination(uint32_t ea,size_t bytes) {
    if(!vm_base || !ea || (uint64_t)ea+bytes>0x100000000ull ||
       (ppu_vm_size && (uint64_t)ea+bytes>ppu_vm_size))return nullptr;
    return vm_base+ea;
}
rsx_geometry_lease* bleach_geometry_handoff_begin(spu_context* ctx) {
    using namespace bleach_geometry_handoff;
    cancel();attempts.fetch_add(1,std::memory_order_relaxed);
    pending=rsx_geometry_handoff_reserve();owner=ctx;
    if(!pending.token)declined.fetch_add(1,std::memory_order_relaxed);
    return &pending;
}
rsx_geometry_lease* bleach_geometry_handoff_pending(spu_context* ctx) {
    return bleach_geometry_handoff::owner==ctx?&bleach_geometry_handoff::pending:nullptr;
}
void bleach_geometry_handoff_finish(spu_context* ctx,int success) {
    using namespace bleach_geometry_handoff;
    if(owner!=ctx)return;
    if(success){published.fetch_add(1,std::memory_order_relaxed);pending={};owner=nullptr;}
    else {declined.fetch_add(1,std::memory_order_relaxed);cancel();}
    if((attempts.load(std::memory_order_relaxed)&4095)==0)report();
}
}
static void bleach_geometry_handoff_initialize() {
    const char* mode=getenv("BLEACH_GEOMETRY_HANDOFF");
    if(!mode || (strcmp(mode,"shadow") && strcmp(mode,"gpu")))return;
    if(!rsx_geometry_handoff_init(128,128*1024))abort();
    bleach_geometry_handoff_mode=!strcmp(mode,"gpu")?2:1;atexit(bleach_geometry_handoff::report);
}
#endif
