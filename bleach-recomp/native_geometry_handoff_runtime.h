#ifndef BLEACH_NATIVE_GEOMETRY_HANDOFF_RUNTIME_H
#define BLEACH_NATIVE_GEOMETRY_HANDOFF_RUNTIME_H
#include "native_geometry_triangle_plan.h"
#include "native_geometry_visibility_input.h"
#include "native_geometry_visibility_fallback.h"
#include "native_geometry_marker.h"
extern int bleach_geometry_handoff_mode;
extern rsx_geometry_lease* bleach_geometry_handoff_begin(spu_context*);
extern rsx_geometry_lease* bleach_geometry_handoff_pending(spu_context*);
extern void bleach_geometry_handoff_finish(spu_context*,int);
extern unsigned char* bleach_geometry_handoff_destination(uint32_t,size_t);
static int bleach_geometry_handoff_capture(spu_context* ctx) {
#if defined(__FMA__)
    if(!bleach_geometry_handoff_mode)return 0;
    rsx_geometry_lease* lease=bleach_geometry_handoff_begin(ctx);
    bleach_triangle_plan plan;
    if(!lease || !lease->token)return 0;
    if(!bleach_kernel_regions_enabled(ctx) || g_ps3_phase_capture ||
       (_mm_getcsr()&0xffc0)!=0xffc0 ||
       !bleach_triangle_prepare(ctx->ls,SPU_LS_SIZE,ctx->gpr[3]._u32[0],
         ctx->gpr[4]._u32[0],ctx->gpr[5]._u32[0],ctx->gpr[1]._u32[0],&plan) ||
       !(lease->bytes=ps3_geometry_visibility_write(&plan.job,lease->data,lease->capacity))) {
        bleach_geometry_handoff_finish(ctx,0);return 0;
    }
    lease->vertices=plan.job.projection.vertices;lease->input_indices=plan.job.index_count;
    lease->cpu_fallback=ps3_visibility_cpu_fallback;
    if(bleach_geometry_handoff_mode==2) {
        lease->deferred=1;
        ps3_geo_write_be32(ctx->ls+plan.descriptor+0x58,plan.index_address+((plan.job.index_count*2+15)&~15u));
        ctx->gpr[3]=spu_zero();ctx->gpr[3]._u32[0]=plan.job.index_count;
        ctx->gpr[0]=spu_link(0x574);ctx->pc=0x574;g_spu_trampoline_fn=0;return 1;
    }
#else
    (void)ctx;
#endif
    return 0;
}
/* 0x2120 copies allocation word0 (EA) to the index publication descriptor,
 * emits a contiguous PUT, then advances the allocation. Capture before it. */
static void bleach_geometry_handoff_indices(spu_context* ctx) {
    if(!bleach_geometry_handoff_mode)return;
    rsx_geometry_lease* lease=bleach_geometry_handoff_pending(ctx);
    if(!lease || !lease->deferred)return;
    uint32_t alloc=ctx->gpr[5]._u32[0];if(alloc>SPU_LS_SIZE-16)abort();
    lease->index_destination=bleach_geometry_handoff_destination(bleach_marker_read(ctx->ls+alloc),lease->input_indices*2);
    if(!lease->index_destination)abort();
}
static void bleach_geometry_handoff_publish(spu_context* ctx,int nonempty) {
    if(!bleach_geometry_handoff_mode)return;
    rsx_geometry_lease* lease=bleach_geometry_handoff_pending(ctx);
    if(!lease || !lease->token)return;
    const uint32_t d=ctx->gpr[3]._u32[0];
    if(!nonempty || !lease->bytes || d>SPU_LS_SIZE-416) {
        bleach_geometry_handoff_finish(ctx,0);return;
    }
    lease->selected_indices=(unsigned)ctx->ls[d+0x190]*256+ctx->ls[d+0x191];
    if(!bleach_geometry_marker_insert(ctx->ls,SPU_LS_SIZE,d,ctx->gpr[4]._u32[0],
        ctx->gpr[5]._u32[0],ctx->gpr[7]._u32[0],lease)) {
        if(lease->deferred && (!lease->index_destination ||
           lease->cpu_fallback(lease->data,lease->bytes,lease->index_destination,lease->input_indices*2)==UINT32_MAX))abort();
        bleach_geometry_handoff_finish(ctx,0);return;
    }
    if(!rsx_geometry_handoff_publish(lease))abort(); /* reservation owns this slot */
    bleach_geometry_handoff_finish(ctx,1);
}
#endif
