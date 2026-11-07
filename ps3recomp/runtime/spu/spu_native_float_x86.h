/* Native single-precision operations under a caller-owned MXCSR RTZ + DAZ + FTZ scope.
 * Float exponent 255 remains SPU extended-range, so exceptional lanes use the
 * integer reference. Subnormal inputs/results are zero, always positive.
 * Never use outside that scope, nor enable without AVX2/FMA CPU support.
 * DAZ supplies zero for subnormal inputs in hardware. Their zero sign cannot
 * affect a nonzero finite add/multiply/FMA result; every zero result is still
 * normalized to positive zero below. Extended-range inputs retain the exact
 * integer fallback. Do not remove the output normalization or overflow guard. */
#ifndef PS3RECOMP_SPU_NATIVE_FLOAT_X86_H
#define PS3RECOMP_SPU_NATIVE_FLOAT_X86_H
#include "spu_helpers.h"
#include <immintrin.h>
/* Keep extended-range integer arithmetic outside the hot geometry loops.
 * Inlining that rare path at every multiply/add bloats a promoted control-flow
 * region and forces its live vectors to spill. Only the bounded SIMD path is
 * always inlined; the exceptional lanes retain the exact integer semantics. */
#if defined(_MSC_VER)
#define SPU_NATIVE_FP_INLINE static __forceinline
#define SPU_NATIVE_FP_COLD static __declspec(noinline)
#else
#define SPU_NATIVE_FP_INLINE static inline __attribute__((always_inline))
#define SPU_NATIVE_FP_COLD static __attribute__((noinline,cold))
#endif
SPU_NATIVE_FP_COLD u128 spu_native_fp_add_exception(u128 a,u128 b,uint32_t neg,u128 out,unsigned mask)
{
    for(unsigned i=0;i<4;i++) if (!(mask>>i&1))
        out._u32[i]=spu__fa_lane(a._u32[i],b._u32[i]^neg);
    return out;
}
SPU_NATIVE_FP_COLD u128 spu_native_fp_mul_exception(u128 a,u128 b,u128 out,unsigned mask)
{
    for(unsigned i=0;i<4;i++) if (!(mask>>i&1))
        out._u32[i]=spu__fm_lane(a._u32[i],b._u32[i]);
    return out;
}
SPU_NATIVE_FP_COLD u128 spu_native_fp_madd_exception(u128 a,u128 b,u128 c,
    uint32_t negp,uint32_t negc,u128 out,unsigned mask)
{
    for(unsigned i=0;i<4;i++) if (!(mask>>i&1))
        out._u32[i]=spu__fma_lane(a._u32[i],b._u32[i],c._u32[i],negp,negc);
    return out;
}
SPU_NATIVE_FP_INLINE __m128i spu_native_fp_clean(__m128i x)
{
    __m128i exp=_mm_and_si128(x,_mm_set1_epi32(0x7f800000));
    return _mm_andnot_si128(_mm_cmpeq_epi32(exp,_mm_setzero_si128()),x);
}
SPU_NATIVE_FP_INLINE __m128i spu_native_fp_regular(__m128i x)
{
    return _mm_cmpgt_epi32(_mm_set1_epi32(0x7f800000),
        _mm_and_si128(x,_mm_set1_epi32(0x7fffffff)));
}
SPU_NATIVE_FP_INLINE __m128i spu_native_fp_result_ok(__m128i x)
{
    /* RTZ saturates IEEE overflow to max-finite: that boundary is ambiguous. */
    return _mm_cmpgt_epi32(_mm_set1_epi32(0x7f7fffff),
        _mm_and_si128(x,_mm_set1_epi32(0x7fffffff)));
}
SPU_NATIVE_FP_INLINE u128 spu_native_fp_add(u128 a,u128 b,uint32_t neg)
{
    __m128i x=spu__x8_ld(a),y=_mm_xor_si128(spu__x8_ld(b),_mm_set1_epi32(neg));
    __m128i r=_mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(x),
                                          _mm_castsi128_ps(y)));
    const __m128i ok=_mm_and_si128(spu_native_fp_result_ok(r),
        _mm_and_si128(spu_native_fp_regular(x),spu_native_fp_regular(y)));
    u128 out=spu__x8_st(spu_native_fp_clean(r));
    unsigned mask=(unsigned)_mm_movemask_ps(_mm_castsi128_ps(ok));
    if (mask!=15) return spu_native_fp_add_exception(a,b,neg,out,mask);
    return out;
}
SPU_NATIVE_FP_INLINE u128 spu_native_fp_fa(u128 a,u128 b) { return spu_native_fp_add(a,b,0); }
SPU_NATIVE_FP_INLINE u128 spu_native_fp_fs(u128 a,u128 b) { return spu_native_fp_add(a,b,0x80000000); }
SPU_NATIVE_FP_INLINE u128 spu_native_fp_fm(u128 a,u128 b)
{
    __m128i x=spu__x8_ld(a),y=spu__x8_ld(b);
    __m128i r=_mm_castps_si128(_mm_mul_ps(_mm_castsi128_ps(x),
                                          _mm_castsi128_ps(y)));
    const __m128i ok=_mm_and_si128(spu_native_fp_result_ok(r),
        _mm_and_si128(spu_native_fp_regular(x),spu_native_fp_regular(y)));
    u128 out=spu__x8_st(spu_native_fp_clean(r));
    unsigned mask=(unsigned)_mm_movemask_ps(_mm_castsi128_ps(ok));
    if(mask!=15) return spu_native_fp_mul_exception(a,b,out,mask);
    return out;
}
SPU_NATIVE_FP_INLINE u128 spu_native_fp_madd(u128 a,u128 b,u128 c,uint32_t negp,uint32_t negc)
{
    __m128i x=_mm_xor_si128(spu__x8_ld(a),_mm_set1_epi32(negp)),y=spu__x8_ld(b);
    __m128i z=_mm_xor_si128(spu__x8_ld(c),_mm_set1_epi32(negc));
    __m128i r=_mm_castps_si128(_mm_fmadd_ps(_mm_castsi128_ps(x),
        _mm_castsi128_ps(y),_mm_castsi128_ps(z)));
    const __m128i ok=_mm_and_si128(spu_native_fp_result_ok(r),_mm_and_si128(
        spu_native_fp_regular(z),_mm_and_si128(spu_native_fp_regular(x),spu_native_fp_regular(y))));
    u128 out=spu__x8_st(spu_native_fp_clean(r));
    unsigned mask=(unsigned)_mm_movemask_ps(_mm_castsi128_ps(ok));
    if(mask!=15) return spu_native_fp_madd_exception(a,b,c,negp,negc,out,mask);
    return out;
}
SPU_NATIVE_FP_INLINE u128 spu_native_fp_fma(u128 a,u128 b,u128 c) {return spu_native_fp_madd(a,b,c,0,0);}
SPU_NATIVE_FP_INLINE u128 spu_native_fp_fms(u128 a,u128 b,u128 c) {return spu_native_fp_madd(a,b,c,0,0x80000000);}
SPU_NATIVE_FP_INLINE u128 spu_native_fp_fnms(u128 a,u128 b,u128 c) {return spu_native_fp_madd(a,b,c,0x80000000,0);}
#undef SPU_NATIVE_FP_INLINE
#undef SPU_NATIVE_FP_COLD
#endif
