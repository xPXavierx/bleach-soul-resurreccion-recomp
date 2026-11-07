/* Native vector lowering for the geometry image's integer/data-flow operations.
 * Keep vectors intact across promoted regions instead of relying on the C
 * compiler to reconstruct SIMD from unions and sixteen scalar byte accesses.
 * These entry points are selected only in the AVX2 image, never the baseline.
 */
#ifndef BLEACH_NATIVE_SPU_INTEGER_H
#define BLEACH_NATIVE_SPU_INTEGER_H
#include "spu_helpers.h"
#include <immintrin.h>
#define BI_INLINE static inline __attribute__((always_inline))
BI_INLINE __m128i bleach_int_load(u128 a) { return _mm_loadu_si128((const __m128i*)&a); }
BI_INLINE u128 bleach_int_store(__m128i a) { u128 r; _mm_storeu_si128((__m128i*)&r,a); return r; }
#define BI_BINARY(name,expression) \
BI_INLINE u128 bleach_int_##name(u128 a,u128 b) { \
    const __m128i x=bleach_int_load(a),y=bleach_int_load(b); \
    return bleach_int_store(expression); }
#define BI_IMMEDIATE(name,expression) \
BI_INLINE u128 bleach_int_##name(u128 a,int32_t imm) { \
    const __m128i x=bleach_int_load(a),y=_mm_set1_epi32(imm); \
    return bleach_int_store(expression); }
BI_BINARY(a,_mm_add_epi32(x,y))
BI_BINARY(sf,_mm_sub_epi32(y,x))
BI_BINARY(and,_mm_and_si128(x,y))
BI_BINARY(or,_mm_or_si128(x,y))
BI_BINARY(xor,_mm_xor_si128(x,y))
BI_BINARY(nor,_mm_xor_si128(_mm_or_si128(x,y),_mm_set1_epi32(-1)))
BI_BINARY(andc,_mm_andnot_si128(y,x))
BI_BINARY(ceq,_mm_cmpeq_epi32(x,y))
BI_BINARY(cgt,_mm_cmpgt_epi32(x,y))
BI_BINARY(clgt,_mm_cmpgt_epi32(_mm_xor_si128(x,_mm_set1_epi32((int)0x80000000u)),
                             _mm_xor_si128(y,_mm_set1_epi32((int)0x80000000u))))
BI_IMMEDIATE(ai,_mm_add_epi32(x,y))
BI_IMMEDIATE(sfi,_mm_sub_epi32(y,x))
BI_IMMEDIATE(andi,_mm_and_si128(x,y))
BI_IMMEDIATE(ori,_mm_or_si128(x,y))
BI_IMMEDIATE(xori,_mm_xor_si128(x,y))
BI_IMMEDIATE(ceqi,_mm_cmpeq_epi32(x,y))
BI_IMMEDIATE(cgti,_mm_cmpgt_epi32(x,y))
BI_IMMEDIATE(clgti,_mm_cmpgt_epi32(_mm_xor_si128(x,_mm_set1_epi32((int)0x80000000u)),
                                 _mm_xor_si128(y,_mm_set1_epi32((int)0x80000000u))))
BI_INLINE u128 bleach_int_selb(u128 a,u128 b,u128 c)
{
    const __m128i x=bleach_int_load(a),y=bleach_int_load(b),mask=bleach_int_load(c);
    return bleach_int_store(_mm_xor_si128(x,_mm_and_si128(_mm_xor_si128(x,y),mask)));
}
BI_INLINE u128 bleach_int_shufb(u128 a,u128 b,u128 c)
{
    /* Host byte j represents SPU byte j^3. The control is already in the
     * destination's host order: only its selected source index needs ^3.
     * PSHUFB zeros selectors with bit7, leaving special values to the LUT.
     * Bit4 chooses b; shifting each 16-bit pair by 3 preserves that byte bit. */
    const __m128i x=bleach_int_load(a),y=bleach_int_load(b),control=bleach_int_load(c);
    const __m128i index=_mm_xor_si128(control,_mm_set1_epi8(3));
    const __m128i data=_mm_blendv_epi8(_mm_shuffle_epi8(x,index),_mm_shuffle_epi8(y,index),
                                     _mm_slli_epi16(control,3));
    const __m128i code=_mm_and_si128(_mm_srli_epi16(control,5),_mm_set1_epi8(7));
    const __m128i values=_mm_setr_epi8(0,0,0,0,0,0,(char)0xff,(char)0x80,0,0,0,0,0,0,0,0);
    return bleach_int_store(_mm_or_si128(data,_mm_shuffle_epi8(values,code)));
}
BI_INLINE u128 bleach_int_shlqbyi(u128 a,int shift)
{
    const __m128i positions=_mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    const __m128i index=_mm_add_epi8(positions,_mm_set1_epi8((char)(shift&31)));
    const __m128i mask=_mm_or_si128(_mm_xor_si128(index,_mm_set1_epi8(3)),
                                  _mm_cmpgt_epi8(index,_mm_set1_epi8(15)));
    return bleach_int_store(_mm_shuffle_epi8(bleach_int_load(a),mask));
}
BI_INLINE u128 bleach_int_rotqbyi(u128 a,int shift)
{
    const __m128i positions=_mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    const __m128i index=_mm_and_si128(_mm_add_epi8(positions,_mm_set1_epi8((char)(shift&15))),
                                    _mm_set1_epi8(15));
    return bleach_int_store(_mm_shuffle_epi8(bleach_int_load(a),_mm_xor_si128(index,_mm_set1_epi8(3))));
}
BI_INLINE u128 bleach_int_shli(u128 a,int shift)
{
    return bleach_int_store(_mm_sll_epi32(bleach_int_load(a),_mm_cvtsi32_si128(shift&63)));
}
BI_INLINE u128 bleach_int_rotmi(u128 a,int shift)
{
    return bleach_int_store(_mm_srl_epi32(bleach_int_load(a),_mm_cvtsi32_si128((0u-(unsigned)shift)&63)));
}
BI_INLINE u128 bleach_int_rotmai(u128 a,int shift)
{
    return bleach_int_store(_mm_sra_epi32(bleach_int_load(a),_mm_cvtsi32_si128((0u-(unsigned)shift)&63)));
}
BI_INLINE u128 bleach_int_roti(u128 a,int shift)
{
    const __m128i x=bleach_int_load(a);
    const unsigned amount=(unsigned)shift&31;
    return bleach_int_store(_mm_or_si128(_mm_sll_epi32(x,_mm_cvtsi32_si128(amount)),
                                       _mm_srl_epi32(x,_mm_cvtsi32_si128(32-amount))));
}
#undef BI_BINARY
#undef BI_IMMEDIATE
#undef BI_INLINE
#endif
