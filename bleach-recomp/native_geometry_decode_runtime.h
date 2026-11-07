#ifndef BLEACH_NATIVE_GEOMETRY_DECODE_RUNTIME_H
#define BLEACH_NATIVE_GEOMETRY_DECODE_RUNTIME_H
#include "native_geometry_decode_plan.h"
extern int bleach_native_decode_mode;
extern unsigned bleach_native_decode_stride;
extern void bleach_native_decode_record(unsigned,unsigned,int);
#if defined(__FMA__)
static int bleach_geometry_decode_try(spu_context* ctx,void(*reference)(spu_context*)) {
    if(!bleach_native_decode_mode)return 0;
    bleach_decode_plan p;
    if(!bleach_kernel_regions_enabled(ctx) || g_ps3_phase_capture ||
       (_mm_getcsr()&0xffc0)!=0xffc0 || !bleach_decode_prepare(ctx->ls,SPU_LS_SIZE,
          ctx->gpr[3]._u32[0],ctx->gpr[4]._u32[0],ctx->gpr[6]._u32[0],ctx->gpr[1]._u32[0],&p)) {
        bleach_native_decode_record(0,0,0);return 0;
    }
    static SPU_THREAD_LOCAL unsigned calls;
    const int verify=bleach_native_decode_stride && ++calls%bleach_native_decode_stride==0;
    static SPU_THREAD_LOCAL spu_context* original;
    void(*incoming)(spu_context*)=g_spu_trampoline_fn;
    const unsigned fp_in=_mm_getcsr();
    if(verify) {
        if(!original)original=(spu_context*)malloc(sizeof(*original));
        if(!original)abort();
        memcpy(original,ctx,sizeof(*ctx));original->ls=original->ls_store;
    }
    bleach_decode_execute(ctx->ls,&p);
    // The only replaced call site immediately reloads r3/r4/r5 and calls7F58.
    // Callee-saved registers, stack pointer and return link are the live ABI.
    ctx->gpr[0]=spu_link(0x408);ctx->pc=0x408;g_spu_trampoline_fn=0;
    if(verify) {
        const unsigned fp_native=_mm_getcsr();
        g_spu_trampoline_fn=incoming;_mm_setcsr(fp_in);
        const int image=(int)original->image_id;
        original->gpr[0]=spu_link(0x408);original->host_depth++;
        reference(original);spu_drain_call(original,0x408);original->host_depth--;spu_img_restore(original,image);
        const uint32_t sp=ctx->gpr[1]._u32[0];
        const int same=ctx->pc==original->pc && ctx->status==original->status &&
            ctx->host_depth==original->host_depth && ctx->image_id==original->image_id &&
            !memcmp(&ctx->gpr[0],&original->gpr[0],2*sizeof(u128)) &&
            !memcmp(&ctx->gpr[80],&original->gpr[80],48*sizeof(u128)) &&
            !memcmp(ctx->ls,original->ls,sp-1200) &&
            !memcmp(ctx->ls+sp+32,original->ls+sp+32,SPU_LS_SIZE-sp-32) &&
            (_mm_getcsr()&0xffc0)==(fp_native&0xffc0) && !g_spu_trampoline_fn;
        g_spu_trampoline_fn=0;_mm_setcsr(fp_native);
        if(!same){fprintf(stderr,"[native-decode] MISMATCH call=%u sp=%x vertices=%u packed=%u\n",calls,sp,p.job.vertices,p.job.packed_count);abort();}
    }
    bleach_native_decode_record(p.job.vertices,p.job.packed_count,verify);return 1;
}
#else
static int bleach_geometry_decode_try(spu_context* ctx,void(*reference)(spu_context*)) {(void)ctx;(void)reference;return 0;}
#endif
#endif
