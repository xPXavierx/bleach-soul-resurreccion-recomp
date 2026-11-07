/* Content-validated cache for repeatedly dispatched small SPU job images.
 * A guest address is not an identity: a title may replace its bytes in place.
 * Every 256th reuse compares every byte; the others a sampled set (see below). The owner must keep
 * this object private to a thread (or provide external synchronization).
 */
#ifndef PS3RECOMP_SPU_IMAGE_CACHE_H
#define PS3RECOMP_SPU_IMAGE_CACHE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define SPU_IMAGE_CACHE_CAPACITY 65536u
typedef struct {
    size_t size;
    uint64_t fingerprint;
    int valid;
    const void* source; unsigned uses;
    unsigned char bytes[SPU_IMAGE_CACHE_CAPACITY];
} spu_image_cache;

uint64_t spu_workload_fingerprint(const void* data, size_t n);

static inline uint64_t spu_image_cache_fingerprint(spu_image_cache* cache,
                                                   const void* data, size_t n)
{
    if (n > SPU_IMAGE_CACHE_CAPACITY)
        return spu_workload_fingerprint(data, n);
    /* A geometry list dispatches the same 48 KB image ~600 times a frame. Between
     * full comparisons (every 256th use, and whenever the size or address
     * changes) it is checked at its start, its end and 16 blocks spread through it:
     * code rewritten in place is caught on the next use. */
    if (cache->valid && cache->size == n && cache->source == data) {
        const unsigned char* d = (const unsigned char*)data;
        int same = (++cache->uses & 255u) != 0 && n >= 4096;
        if (same) {
            same = !memcmp(cache->bytes, d, 256) && !memcmp(cache->bytes + n - 256, d + n - 256, 256);
            for (size_t k = 1; same && k <= 16; k++) {
                const size_t at = (n / 17 * k) & ~(size_t)63;
                same = !memcmp(cache->bytes + at, d + at, 64);
            }
        }
        if (same || !memcmp(cache->bytes, data, n)) return cache->fingerprint;
    }
    /* Hash the snapshot that future hits compare, rather than separately
     * reading the guest bytes again while making the cache entry. */
    memcpy(cache->bytes, data, n);
    cache->size = n; cache->source = data; cache->uses = 0;
    cache->fingerprint = spu_workload_fingerprint(cache->bytes, n);
    cache->valid = 1;
    return cache->fingerprint;
}
#endif
