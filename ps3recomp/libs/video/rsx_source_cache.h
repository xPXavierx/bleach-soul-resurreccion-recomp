/* Exact, bounded CPU snapshots for texture mutation checks. Single owner. */
#ifndef RSX_SOURCE_CACHE_H
#define RSX_SOURCE_CACHE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif
typedef struct rsx_source_snapshot {
    uint8_t* bytes;
    uint32_t size;
} rsx_source_snapshot;

static inline int rsx_source_compare(const rsx_source_snapshot* s,
                                     const void* source, uint32_t size)
{
    if (!s->bytes || !source || s->size != size) return -1;
#if defined(__SSE2__) || defined(_M_X64)
    /* Equality needs no lexicographic byte selection. Compare whole cache
     * lines with baseline SSE2, including every byte and a bounded tail. */
    const uint8_t* a = s->bytes;
    const uint8_t* b = (const uint8_t*)source;
    uint32_t i = 0;
    for (; size - i >= 64; i += 64) {
        __m128i diff = _mm_setzero_si128();
        for (unsigned j = 0; j < 64; j += 16)
            diff = _mm_or_si128(diff, _mm_xor_si128(
                _mm_loadu_si128((const __m128i*)(a+i+j)),
                _mm_loadu_si128((const __m128i*)(b+i+j))));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(diff, _mm_setzero_si128())) != 65535)
            return 1;
    }
    return memcmp(a+i, b+i, size-i) != 0;
#else
    return memcmp(s->bytes, source, size) != 0;
#endif
}
static inline void rsx_source_release(rsx_source_snapshot* s, size_t* used)
{
    if (s->bytes) { *used -= s->size; free(s->bytes); }
    s->bytes = NULL; s->size = 0;
}
static inline rsx_source_snapshot rsx_source_capture(const void* source,
    uint32_t size, size_t* used, size_t limit)
{
    rsx_source_snapshot s = {0};
    if (!source || !size || *used > limit || size > limit - *used) return s;
    s.bytes = (uint8_t*)malloc(size);
    if (s.bytes) { memcpy(s.bytes, source, size); s.size = size; *used += size; }
    return s;
}
#endif
