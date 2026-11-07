#ifndef BLEACH_NATIVE_GEOMETRY_TASKS_H
#define BLEACH_NATIVE_GEOMETRY_TASKS_H
#include "spurs_native_batch.h"
#include "native_phase_pool.h"
#include "spu_workload.h"
#include "spu_image_cache.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
extern "C" unsigned char* vm_base;
namespace bleach_geometry_tasks {
static ps3_native::PhasePool* pool=nullptr;
static unsigned worker_count;
static std::atomic<uint64_t> accepted{0},declined{0};
static uint32_t read32(uint32_t ea) {
    const unsigned char* p=vm_base+ea;
    return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];
}
static int accept(const spurs_native_batch* batch) {
    // Only the audited, immutable EDGE image and independent 256-byte list ABI.
    // The ordinary scheduler handles audio, completions and other descriptors.
    if(batch->count<2 || batch->descriptor_size!=256 || batch->stride!=256 ||
       uint64_t(batch->array)+uint64_t(batch->count)*256>0x10000000ull) {
        ++declined;return 0;
    }
    const uint32_t binary=read32(batch->array+4)&~7u;
    if(!binary || binary+48272u>0x10000000u || read32(batch->array)){++declined;return 0;}
    for(unsigned i=0;i<batch->count;i++) {
        const uint32_t ea=batch->array+i*256;
        if(read32(ea) || (read32(ea+4)&~7u)!=binary || read32(ea+8)!=0x0bc900a0u ||
           read32(ea+0xc) || read32(ea+0x20) || read32(ea+0x24) ||
           !read32(ea+0xd0) || !read32(ea+0xd4)) {++declined;return 0;}
    }
    static thread_local spu_image_cache cache{};
    if(spu_image_cache_fingerprint(&cache,vm_base+binary,48272)!=0xde1065fcb7151ac3ull) {
        ++declined;return 0;
    }
    ++accepted;return 1;
}
static void run_job(void* user,unsigned index,unsigned lane) {
    const auto* batch=static_cast<const spurs_native_batch*>(user);
    batch->run_job(batch->runtime,index,batch->lanes[lane]);
}
static bool cancelled(void* user) {
    const auto* batch=static_cast<const spurs_native_batch*>(user);
    return batch->cancelled(batch->runtime)!=0;
}
static void execute(const spurs_native_batch* batch) {
    ps3_native::PhasePool::Batch work{const_cast<spurs_native_batch*>(batch),batch->count,
        batch->lane_count,run_job,cancelled};
    pool->run(work);
}
static void shared_begin(spu_context*) {ps3_native::PhasePool::shared_begin();}
static void validate_context(spu_context* ctx,int image) {
    if(ps3_native::PhasePool::in_task() && (image!=1014 || ctx->ls!=ctx->ls_store || ctx->policy_mode)) {
        fprintf(stderr,"[native-pool] incompatible execution context\n");abort();
    }
}
static void report() {
    const auto stats=pool->stats();
    fprintf(stderr,"[native-geometry-pool] workers=%u accepted=%llu declined=%llu batches=%llu admitted=%llu completed=%llu lease_waits=%llu peak_inflight=%u\n",
        worker_count,(unsigned long long)accepted.load(),(unsigned long long)declined.load(),
        (unsigned long long)stats.batches,(unsigned long long)stats.admitted,
        (unsigned long long)stats.completed,(unsigned long long)stats.lease_waits,stats.peak_inflight);
}
static void initialize() {
    const char* env=getenv("BLEACH_GEOMETRY_WORKERS");if(!env || !*env || !strcmp(env,"0"))return;
    char* end=nullptr;unsigned long count=strtoul(env,&end,10);
    if(!end || *end || !count || count>32)abort();
    worker_count=(unsigned)count;
    // Process-lifetime workers, like the existing SPURS services. Batch teardown
    // is synchronous; the host currently exits all services via process exit.
    pool=new ps3_native::PhasePool(worker_count);
    g_spu_shared_access_begin=shared_begin;
    g_spurs_native_batch_accept=accept;g_spurs_native_batch_execute=execute;
    atexit(report);
    fprintf(stderr,"[native-geometry-pool] enabled workers=%u publication=ordered-lane-leases\n",worker_count);
}
}
#endif
