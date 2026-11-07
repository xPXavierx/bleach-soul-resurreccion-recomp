/* Exact integer-to-SPU-float conversion using native AVX2 lanes. Include after
 * spu_helpers.h. No host rounding-mode dependency and no fast-math. */
#ifndef BLEACH_NATIVE_SPU_CONVERSIONS_H
#define BLEACH_NATIVE_SPU_CONVERSIONS_H
#if defined(__AVX2__)
#include <immintrin.h>
static inline __m128i bleach_spu_pack_integer_pd(__m256d d, int scale)
{
    const __m256i bits = _mm256_castpd_si256(d);
    const __m256i exponent = _mm256_add_epi64(
        _mm256_and_si256(_mm256_srli_epi64(bits,52),_mm256_set1_epi64x(2047)),
        _mm256_set1_epi64x((scale & 255) - 155 + 127 - 1023));
    const __m256i sign = _mm256_and_si256(_mm256_srli_epi64(bits,32),
                                         _mm256_set1_epi64x(0x80000000u));
    __m256i packed = _mm256_or_si256(sign, _mm256_or_si256(
        _mm256_slli_epi64(exponent,23),
        _mm256_and_si256(_mm256_srli_epi64(bits,29),_mm256_set1_epi64x(0x7fffff))));
    const __m256i overflow = _mm256_cmpgt_epi64(exponent,_mm256_set1_epi64x(255));
    packed = _mm256_blendv_epi8(packed,
        _mm256_or_si256(sign,_mm256_set1_epi64x(0x7fffffff)),overflow);
    packed = _mm256_and_si256(packed,
        _mm256_cmpgt_epi64(exponent,_mm256_setzero_si256()));
    return _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(packed,
        _mm256_setr_epi32(0,2,4,6,0,2,4,6)));
}
static inline u128 bleach_spu_csflt_avx2(u128 a, int scale)
{
    const __m128i ints = _mm_loadu_si128((const __m128i*)a._u32);
    const __m128i packed = bleach_spu_pack_integer_pd(_mm256_cvtepi32_pd(ints),scale);
    u128 result; _mm_storeu_si128((__m128i*)result._u32,packed); return result;
}
static inline u128 bleach_spu_cuflt_avx2(u128 a, int scale)
{
    const __m128i ints = _mm_loadu_si128((const __m128i*)a._u32);
    const __m256i negative = _mm256_cvtepi32_epi64(_mm_srai_epi32(ints,31));
    const __m256d d = _mm256_add_pd(_mm256_cvtepi32_pd(ints),
        _mm256_and_pd(_mm256_castsi256_pd(negative),_mm256_set1_pd(4294967296.0)));
    u128 result;
    _mm_storeu_si128((__m128i*)result._u32,bleach_spu_pack_integer_pd(d,scale));
    return result;
}
#endif
#endif
