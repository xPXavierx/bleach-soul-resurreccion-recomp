#ifndef BLEACH_NATIVE_GEOMETRY_TRIANGLES_RUNTIME_H
#define BLEACH_NATIVE_GEOMETRY_TRIANGLES_RUNTIME_H
#include "native_geometry_triangle_plan.h"
extern int bleach_native_triangle_mode;
extern unsigned bleach_native_triangle_stride;
extern void bleach_native_triangle_record(unsigned,unsigned,int);
#if defined(__FMA__)
static int bleach_geometry_triangle_try(spu_context* ctx,void(*reference)(spu_context*)) {
    if(!bleach_native_triangle_mode)return 0;
    bleach_triangle_plan p;
    if(!bleach_kernel_regions_enabled(ctx) || g_ps3_phase_capture ||
       (_mm_getcsr()&0xffc0)!=0xffc0 || !bleach_triangle_prepare(ctx->ls,SPU_LS_SIZE,
          ctx->gpr[3]._u32[0],ctx->gpr[4]._u32[0],ctx->gpr[5]._u32[0],ctx->gpr[1]._u32[0],&p)) {
        bleach_native_triangle_record(0,0,0);return 0;
    }
    static SPU_THREAD_LOCAL unsigned calls;
    const int verify=bleach_native_triangle_stride && ++calls%bleach_native_triangle_stride==0;
    static SPU_THREAD_LOCAL spu_context* original;
    static SPU_THREAD_LOCAL spu_context* before;
    void(*incoming)(spu_context*)=g_spu_trampoline_fn;
    const unsigned fp_in=_mm_getcsr();
    if(verify) {
        if(!original)original=(spu_context*)malloc(sizeof(*original));
        if(!before)before=(spu_context*)malloc(sizeof(*before));
        if(!original || !before)abort();
        memcpy(before,ctx,sizeof(*ctx));
        memcpy(original,ctx,sizeof(*ctx));original->ls=original->ls_store;
    }
    const unsigned kept=bleach_triangle_execute(ctx->ls,&p);
    ctx->gpr[3]=spu_zero();ctx->gpr[3]._u32[0]=kept;
    // The only replaced call site consumes scalar r3, then reloads r3/r4.
    // Callee-saved registers, stack pointer and return link are the live ABI.
    ctx->gpr[0]=spu_link(0x574);ctx->pc=0x574;g_spu_trampoline_fn=0;
    if(verify) {
        const unsigned fp_native=_mm_getcsr();
        g_spu_trampoline_fn=incoming;_mm_setcsr(fp_in);
        const int image=(int)original->image_id;
        original->gpr[0]=spu_link(0x574);original->host_depth++;
        reference(original);spu_drain_call(original,0x574);original->host_depth--;spu_img_restore(original,image);
        const uint32_t sp=ctx->gpr[1]._u32[0];
        const int same=ctx->pc==original->pc && ctx->status==original->status &&
            ctx->host_depth==original->host_depth && ctx->image_id==original->image_id &&
            !memcmp(&ctx->gpr[0],&original->gpr[0],2*sizeof(u128)) &&
            ctx->gpr[3]._u32[0]==original->gpr[3]._u32[0] &&
            !memcmp(&ctx->gpr[80],&original->gpr[80],48*sizeof(u128)) &&
            !memcmp(ctx->ls,original->ls,sp-384) &&
            !memcmp(ctx->ls+sp+32,original->ls+sp+32,SPU_LS_SIZE-sp-32) &&
            (_mm_getcsr()&0xffc0)==(fp_native&0xffc0) && !g_spu_trampoline_fn;
        g_spu_trampoline_fn=0;_mm_setcsr(fp_native);
        if(!same){
            fprintf(stderr,"[native-triangle] MISMATCH call=%u sp=%x vertices=%u packed=%u kept=%u original_kept=%u\n",calls,sp,p.job.projection.vertices,p.job.index_count,kept,original->gpr[3]._u32[0]);
            unsigned shown=0;for(unsigned at=0;at<SPU_LS_SIZE;at++) {
                if(at>=sp-384 && at<sp+32)continue;
                if(ctx->ls[at]!=original->ls[at] && shown++<8)fprintf(stderr,"[native-triangle-diff] at=%x native=%02x original=%02x\n",at,ctx->ls[at],original->ls[at]);
            }
            const char* path=getenv("BLEACH_TRIANGLE_FAILURE_CAPTURE");
            if(path && *path) {
                FILE* f=fopen(path,"wb");if(!f)abort();
                const unsigned header[2]={528432,1};
                const unsigned record[12]={0,0x5110,0x574,(unsigned)before->image_id,fp_in,(unsigned)before->status,(unsigned)original->status,0,0,0,0,0};
                fwrite("GEOCTR1",8,1,f);fwrite(header,sizeof(header),1,f);fwrite(record,sizeof(record),1,f);
                fwrite(before->gpr,sizeof(before->gpr),1,f);fwrite(before->ls_store,SPU_LS_SIZE,1,f);
                fwrite(original->gpr,sizeof(original->gpr),1,f);fwrite(original->ls,SPU_LS_SIZE,1,f);fclose(f);
            }
            abort();
        }
    }
    bleach_native_triangle_record(p.job.projection.vertices,p.job.index_count,verify);return 1;
}
#else
static int bleach_geometry_triangle_try(spu_context* ctx,void(*reference)(spu_context*)) {(void)ctx;(void)reference;return 0;}
#endif
#endif
