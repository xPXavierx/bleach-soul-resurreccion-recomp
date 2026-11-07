#ifndef BLEACH_NATIVE_SPU_KERNELS_H
#define BLEACH_NATIVE_SPU_KERNELS_H
#include "spu_context.h"
#include "native_phase_result.h"
#include <stdlib.h>
#include <stdio.h>
#include <xmmintrin.h>
/* Optional read-only phase observer; installed before workers are started. */
extern void (*bleach_geometry_phase_observer)(spu_context*,unsigned,uint32_t,uint32_t,int);
extern int bleach_native_phase_mode;
extern unsigned bleach_native_phase_stride;
extern void bleach_native_phase_verified(void);
extern int bleach_native_phase_profile_active(void);
extern void bleach_native_phase_profile_sample(const ps3_phase_result_probe*,uint64_t,uint64_t);
static uint64_t bleach_phase_result_version(void)
{
#ifdef __FMA__
    return UINT64_C(0xde1065fc98e00002);
#else
    return UINT64_C(0xde1065fc98e00001);
#endif
}
static uint64_t bleach_phase_result_hint(const spu_context* ctx)
{
    uint64_t h=UINT64_C(0x9e3779b185ebca87);
    const uint32_t at[2]={ctx->gpr[85]._u32[0],ctx->gpr[90]._u32[0]};
    const uint32_t count=ctx->gpr[5]._u32[0];
    const uint32_t bytes[2]={16,count?(count==1?48u:96u):0u};
    // Only a lookup hint. The complete read dependency set is compared later.
    for(unsigned j=0;j<2;j++)if(at[j]<=SPU_LS_SIZE && bytes[j]<=SPU_LS_SIZE-at[j])
        for(unsigned i=0;i<bytes[j];i+=8){uint64_t v;memcpy(&v,ctx->ls+at[j]+i,8);
            h=(h^v)*UINT64_C(0xc2b2ae3d27d4eb4f);h=(h<<31)|(h>>33);}
    return h;
}
/* Private job LS has no register-file aliases; preserve quadword BE semantics. */
static inline u128 bleach_kernel_load(const uint8_t* ls,uint32_t address)
{
    u128 result;
    memcpy(&result,ls+(address & (SPU_LS_MASK & ~15u)),16);
    for (unsigned i=0;i<4;i++) result._u32[i]=__builtin_bswap32(result._u32[i]);
    return result;
}
static inline void bleach_kernel_store(uint8_t* ls,uint32_t address,u128 value)
{
    for (unsigned i=0;i<4;i++) value._u32[i]=__builtin_bswap32(value._u32[i]);
    memcpy(ls+(address & (SPU_LS_MASK & ~15u)),&value,16);
}
static int bleach_kernel_enabled(const spu_context* ctx)
{
    static SPU_THREAD_LOCAL int enabled=-1;
    if (enabled<0) {
        const char* value=getenv("BLEACH_NATIVE_REGISTERS");
        enabled=!value || strcmp(value,"0");
    }
    /* Keep the original path for active LS diagnostics and non-private LS. */
    return enabled && ctx->ls==ctx->ls_store && !ctx->policy_mode &&
        g_spu_ls_probe==0 && g_spu_smc_watch==0 && g_spu_ls_watch_n==0;
}
static unsigned bleach_kernel_verify_limit(void)
{
    static SPU_THREAD_LOCAL int limit=-1;
    if (limit<0) {
        const char* value=getenv("BLEACH_VERIFY_REGISTERS");
        limit=value ? atoi(value) : 0;
        if (limit<0) limit=0;
    }
    return (unsigned)limit;
}
static int bleach_phase_enabled(const spu_context* ctx)
{
    if (!bleach_native_phase_mode || !ctx->host_depth || !bleach_kernel_enabled(ctx)) return 0;
    const uint32_t sp=ctx->gpr[1]._u32[0],base=ctx->gpr[3]._u32[0];
    /* Header must not alias the phase's stack saves. A rejected call keeps the
     * complete original path, including its guards, mailbox and diagnostics. */
    if (sp<656 || sp>SPU_LS_SIZE-32 || (sp&15) || (base&15) ||
        base<sp+32 || base>SPU_LS_SIZE-16) return 0;
    const u128 header=bleach_kernel_load(ctx->ls,base);
    const u128 flag=spu_rotqbyi(header,13);
    if (spu_andi(flag,16)._u32[0]) return 1;
    const u128 zero_count=spu_sfi(spu_ceqi(ctx->gpr[5],0),0);
    const u128 format=spu_andi(spu_rotqbyi(header,3),15);
    const u128 zero_format=spu_sfi(spu_fsm(spu_ceqbi(format,0)),0);
    const u128 active=spu_xsbh(spu_ceqbi(spu_or(zero_count,zero_format),0));
    if (!active._u16[0]) return 1;
    /* Exactly the original guard controlling the sole opaque mailbox call.
     * No accepted call can execute that I/O twice during differential verify. */
    return !spu_xsbh(spu_cgtbi(flag,-1))._u16[0];
}
static int bleach_kernel_regions_enabled(const spu_context* ctx)
{
    static SPU_THREAD_LOCAL int enabled=-1;
    if (enabled<0) {
        const char* value=getenv("BLEACH_NATIVE_REGIONS");
        enabled=value && !strcmp(value,"1");
    }
    return enabled && bleach_kernel_enabled(ctx);
}
static int bleach_kernel_cfg_enabled(const spu_context* ctx)
{
    static SPU_THREAD_LOCAL int enabled=-1;
    if (enabled<0) {
        const char* value=getenv("BLEACH_NATIVE_CFG");
        enabled=value && !strcmp(value,"1");
    }
    return enabled && bleach_kernel_regions_enabled(ctx);
}
static void bleach_kernel_verify(spu_context* ctx,void(*native)(spu_context*),
    void(*reference)(spu_context*),const char* name,unsigned count)
{
    /* Bounded scratch per worker, only allocated in differential-test mode. */
    static SPU_THREAD_LOCAL spu_context* copy;
    if (!copy) copy=(spu_context*)malloc(sizeof(*copy));
    if (!copy) { fprintf(stderr,"[native-kernel] validation allocation failed\n"); abort(); }
    memcpy(copy,ctx,sizeof(*copy)); copy->ls=copy->ls_store;
    void(*incoming)(spu_context*)=g_spu_trampoline_fn;
    const unsigned fp_in=_mm_getcsr();
    const int profile=!strcmp(name,"closed-phase-98E0") && bleach_native_phase_profile_active();
    ps3_phase_result_probe sample;
    uint64_t begin=0,native_ns=0,reference_ns=0;
    if(profile){ps3_phase_result_probe_begin();begin=ps3_phase_result_clock_ns();}
    native(ctx);
    if(profile){native_ns=ps3_phase_result_clock_ns()-begin;ps3_phase_result_probe_end(&sample);}
    const unsigned fp_native=_mm_getcsr();
    void(*outgoing)(spu_context*)=g_spu_trampoline_fn;
    g_spu_trampoline_fn=incoming;
    _mm_setcsr(fp_in);
    if(profile)begin=ps3_phase_result_clock_ns();
    reference(copy);
    if(profile)reference_ns=ps3_phase_result_clock_ns()-begin;
    const int same=fp_native==_mm_getcsr() && outgoing==g_spu_trampoline_fn && ctx->pc==copy->pc &&
        ctx->status==copy->status && !memcmp(ctx->gpr,copy->gpr,sizeof(ctx->gpr)) &&
        !memcmp(ctx->ls,copy->ls,SPU_LS_SIZE);
    g_spu_trampoline_fn=outgoing;
    _mm_setcsr(fp_native);
    if (!same) { fprintf(stderr,"[native-kernel] MISMATCH %s call=%u\n",name,count); abort(); }
    if(profile)bleach_native_phase_profile_sample(&sample,native_ns,reference_ns);
    if (count==1 || count==bleach_kernel_verify_limit())
        fprintf(stderr,"[native-kernel] exact %s call=%u\n",name,count);
}
#endif
