/* Typed four-influence skinning. No guest context, allocation, cache or tracing.
 * Caller validates sizes/aliasing and owns RTZ+DAZ+FTZ and AVX2/FMA dispatch.
 * All serialized floats are big-endian. Palette matrices use cyclic diagonals:
 * [m00,m11,m22,tx], [m01,m12,m20,ty], [m02,m10,m21,tz].
 * Influence bytes are four (weight,index) pairs. Weight scale is the original
 * 0x3b808080; accumulation order and SPU finite/extended semantics are retained.
 * Position is float4. Frame output is three cyclic vectors with w=1,0,0.
 */
#ifndef PS3_SKINNING_CYCLIC4_H
#define PS3_SKINNING_CYCLIC4_H
#include "native_skinning_result.h"
#if defined(__FMA__)
#include "spu_native_float_x86.h"
static inline u128 ps3_skin4_load(const uint8_t* data) {
    u128 v;memcpy(&v,data,16);
    for(unsigned i=0;i<4;i++)v._u32[i]=__builtin_bswap32(v._u32[i]);
    return v;
}
static inline void ps3_skin4_store(uint8_t* data,u128 v) {
    for(unsigned i=0;i<4;i++)v._u32[i]=__builtin_bswap32(v._u32[i]);
    memcpy(data,&v,16);
}
static inline u128 ps3_skin4_splat(u128 v,unsigned lane) {
    u128 out;for(unsigned i=0;i<4;i++)out._u32[i]=v._u32[lane];return out;
}
static inline u128 ps3_skin4_bounded_mul(u128 a,u128 b) {
    __m128i r=_mm_castps_si128(_mm_mul_ps(_mm_castsi128_ps(spu__x8_ld(a)),_mm_castsi128_ps(spu__x8_ld(b))));
    return spu__x8_st(spu_native_fp_clean(r));
}
static inline u128 ps3_skin4_bounded_madd(u128 a,u128 b,u128 c) {
    __m128i r=_mm_castps_si128(_mm_fmadd_ps(_mm_castsi128_ps(spu__x8_ld(a)),
        _mm_castsi128_ps(spu__x8_ld(b)),_mm_castsi128_ps(spu__x8_ld(c))));
    return spu__x8_st(spu_native_fp_clean(r));
}
#define PS3_SKIN4_RUN ps3_skin4_checked
#define PS3_SKIN4_MUL spu_native_fp_fm
#define PS3_SKIN4_MADD spu_native_fp_fma
#include "spu_skinning_cyclic4.inl"
#undef PS3_SKIN4_RUN
#undef PS3_SKIN4_MUL
#undef PS3_SKIN4_MADD
#define PS3_SKIN4_RUN ps3_skin4_bounded
#define PS3_SKIN4_MUL ps3_skin4_bounded_mul
#define PS3_SKIN4_MADD ps3_skin4_bounded_madd
#include "spu_skinning_cyclic4.inl"
#undef PS3_SKIN4_RUN
#undef PS3_SKIN4_MUL
#undef PS3_SKIN4_MADD
static int ps3_skin4_span_bounded(const uint8_t* bytes,unsigned size) {
    __m128i maximum=_mm_setzero_si128();
    for(unsigned i=0;i<size;i+=16) {
        __m128i v=_mm_and_si128(spu__x8_ld(ps3_skin4_load(bytes+i)),_mm_set1_epi32(0x7fffffff));
        maximum=_mm_max_epi32(maximum,v);
    }
    return !_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(maximum,_mm_set1_epi32(0x5d800000))));
}
/* With |matrix|,|position| <= B=2^60 and 0<=weight<=1, each blended
 * component is bounded by4B and every transform accumulator by16B^2=2^124.
 * No operation reaches IEEE overflow or SPU extended exponents. RTZ, DAZ,
 * FTZ and positive-zero normalization after EVERY operation remain identical.
 * Validate once per batch; retain the checked path for any other inputs. */
static int ps3_skin4_run(const ps3_skin4_batch* batch) {
    if(batch->allow_bounded && ps3_skin4_span_bounded(batch->palette,batch->palette_entries*48) &&
       ps3_skin4_span_bounded(batch->positions,batch->vertices*16)) {
        ps3_skin4_bounded(batch);return 1;
    }
    ps3_skin4_checked(batch);return 0;
}
#endif
#endif
