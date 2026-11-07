/* Eight-lane native arithmetic. Requires the same caller-owned AVX2/FMA and
 * MXCSR RTZ/DAZ/FTZ/masked-exception contract as spu_native_float_x86.h.
 * Pairs contain independent original operations, not algebraic reassociation. */
#ifndef PS3RECOMP_SPU_NATIVE_PAIRS_X86_H
#define PS3RECOMP_SPU_NATIVE_PAIRS_X86_H
#include "spu_native_float_x86.h"
typedef struct {u128 lo,hi;} spu_native_fp_pair;
#define SPU_PAIR_INLINE static inline __attribute__((always_inline))
#define SPU_PAIR_COLD static __attribute__((noinline,cold))
SPU_PAIR_INLINE __m256i spu_pair_pack(u128 lo,u128 hi) {
    return _mm256_inserti128_si256(_mm256_castsi128_si256(spu__x8_ld(lo)),spu__x8_ld(hi),1);
}
SPU_PAIR_INLINE spu_native_fp_pair spu_pair_unpack(__m256i value) {
    spu_native_fp_pair out={spu__x8_st(_mm256_castsi256_si128(value)),
                            spu__x8_st(_mm256_extracti128_si256(value,1))};return out;
}
SPU_PAIR_INLINE __m256i spu_pair_regular(__m256i x) {
    return _mm256_cmpgt_epi32(_mm256_set1_epi32(0x7f800000),
                              _mm256_and_si256(x,_mm256_set1_epi32(0x7fffffff)));
}
SPU_PAIR_INLINE __m256i spu_pair_result_ok(__m256i x) {
    return _mm256_cmpgt_epi32(_mm256_set1_epi32(0x7f7fffff),
                              _mm256_and_si256(x,_mm256_set1_epi32(0x7fffffff)));
}
SPU_PAIR_INLINE __m256i spu_pair_clean(__m256i x) {
    return _mm256_andnot_si256(_mm256_cmpeq_epi32(
        _mm256_and_si256(x,_mm256_set1_epi32(0x7f800000)),_mm256_setzero_si256()),x);
}
SPU_PAIR_COLD spu_native_fp_pair spu_pair_fix(spu_native_fp_pair a,spu_native_fp_pair b,
    spu_native_fp_pair c,spu_native_fp_pair out,unsigned mask,unsigned op,uint32_t negp,uint32_t negc) {
    u128 *aa[2]={&a.lo,&a.hi},*bb[2]={&b.lo,&b.hi},*cc[2]={&c.lo,&c.hi},*rr[2]={&out.lo,&out.hi};
    for(unsigned h=0;h<2;h++)for(unsigned lane=0;lane<4;lane++)if(!(mask>>(h*4+lane)&1)) {
        uint32_t x=aa[h]->_u32[lane],y=bb[h]->_u32[lane],z=cc[h]->_u32[lane];
        rr[h]->_u32[lane]=op==0?spu__fa_lane(x,y^negc):op==1?spu__fm_lane(x,y):spu__fma_lane(x,y,z,negp,negc);
    }
    return out;
}
SPU_PAIR_INLINE spu_native_fp_pair spu_pair_binary(u128 a0,u128 b0,u128 a1,u128 b1,unsigned multiply,uint32_t neg) {
    const __m256i x=spu_pair_pack(a0,a1),y=_mm256_xor_si256(spu_pair_pack(b0,b1),_mm256_set1_epi32(neg));
    const __m256i r=_mm256_castps_si256(multiply?
        _mm256_mul_ps(_mm256_castsi256_ps(x),_mm256_castsi256_ps(y)):
        _mm256_add_ps(_mm256_castsi256_ps(x),_mm256_castsi256_ps(y)));
    const __m256i ok=_mm256_and_si256(spu_pair_result_ok(r),_mm256_and_si256(spu_pair_regular(x),spu_pair_regular(y)));
    const unsigned mask=(unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(ok));
    spu_native_fp_pair out=spu_pair_unpack(spu_pair_clean(r));
    if(mask!=255) {
        spu_native_fp_pair a={a0,a1},b={b0,b1},c={0};
        return spu_pair_fix(a,b,c,out,mask,multiply,0,neg);
    }
    return out;
}
SPU_PAIR_INLINE spu_native_fp_pair spu_native_pair_fa(u128 a0,u128 b0,u128 a1,u128 b1) {return spu_pair_binary(a0,b0,a1,b1,0,0);}
SPU_PAIR_INLINE spu_native_fp_pair spu_native_pair_fs(u128 a0,u128 b0,u128 a1,u128 b1) {return spu_pair_binary(a0,b0,a1,b1,0,0x80000000);}
SPU_PAIR_INLINE spu_native_fp_pair spu_native_pair_fm(u128 a0,u128 b0,u128 a1,u128 b1) {return spu_pair_binary(a0,b0,a1,b1,1,0);}
SPU_PAIR_INLINE spu_native_fp_pair spu_pair_madd(u128 a0,u128 b0,u128 c0,u128 a1,u128 b1,u128 c1,uint32_t negp,uint32_t negc) {
    const __m256i x=_mm256_xor_si256(spu_pair_pack(a0,a1),_mm256_set1_epi32(negp)),y=spu_pair_pack(b0,b1),
                   z=_mm256_xor_si256(spu_pair_pack(c0,c1),_mm256_set1_epi32(negc));
    const __m256i r=_mm256_castps_si256(_mm256_fmadd_ps(_mm256_castsi256_ps(x),_mm256_castsi256_ps(y),_mm256_castsi256_ps(z)));
    const __m256i ok=_mm256_and_si256(spu_pair_result_ok(r),_mm256_and_si256(spu_pair_regular(x),
                      _mm256_and_si256(spu_pair_regular(y),spu_pair_regular(z))));
    const unsigned mask=(unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(ok));
    spu_native_fp_pair out=spu_pair_unpack(spu_pair_clean(r));
    if(mask!=255) {
        spu_native_fp_pair a={a0,a1},b={b0,b1},c={c0,c1};
        return spu_pair_fix(a,b,c,out,mask,2,negp,negc);
    }
    return out;
}
SPU_PAIR_INLINE spu_native_fp_pair spu_native_pair_fma(u128 a0,u128 b0,u128 c0,u128 a1,u128 b1,u128 c1) {return spu_pair_madd(a0,b0,c0,a1,b1,c1,0,0);}
SPU_PAIR_INLINE spu_native_fp_pair spu_native_pair_fms(u128 a0,u128 b0,u128 c0,u128 a1,u128 b1,u128 c1) {return spu_pair_madd(a0,b0,c0,a1,b1,c1,0,0x80000000);}
SPU_PAIR_INLINE spu_native_fp_pair spu_native_pair_fnms(u128 a0,u128 b0,u128 c0,u128 a1,u128 b1,u128 c1) {return spu_pair_madd(a0,b0,c0,a1,b1,c1,0x80000000,0);}
#undef SPU_PAIR_INLINE
#undef SPU_PAIR_COLD
#endif
