#ifndef PS3_NATIVE_SKINNING_RESULT_H
#define PS3_NATIVE_SKINNING_RESULT_H
#include <stdint.h>
/* Typed buffer contract. The adapter validates every span and palette index;
 * writes must not alias either read-only input or each other. Positions are
 * in/out. Pure compute must return normally, not read sticky FP flags, and
 * preserve FP control bits. All FP exceptions must be masked by the caller. */
typedef struct ps3_skin4_batch {
    const uint8_t* palette;
    const uint8_t* influences;
    uint8_t* positions;
    uint8_t* frames;
    unsigned vertices;
    unsigned palette_entries,allow_bounded;
} ps3_skin4_batch;
typedef struct ps3_skin4_result_counters {
    uint64_t hits,misses,stored,declined,evicted,allocations,bytes,inflight;
    uint64_t hit_vertices,miss_vertices;
} ps3_skin4_result_counters;
#ifdef __cplusplus
extern "C" {
#endif
int ps3_skin4_cached_run(const ps3_skin4_batch*,uint64_t,int(*)(const ps3_skin4_batch*));
void ps3_skin4_result_stats(ps3_skin4_result_counters*);
/* Tests/shutdown only, with no live calls. */
void ps3_skin4_result_reset(void);
#ifdef __cplusplus
}
#endif
#endif
