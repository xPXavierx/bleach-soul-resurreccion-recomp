#ifndef PS3RECOMP_SPURS_NATIVE_BATCH_H
#define PS3RECOMP_SPURS_NATIVE_BATCH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional host executor for explicitly verified independent job lists.
 * Accept is read-only, before reservation. Execute is synchronous and must
 * drain every admitted job before returning. The runtime reserves the listed
 * SPU identities against other chains/policies throughout execute, then releases
 * them. More host workers must NOT imply more concurrent users of one identity.
 * Unknown batches must be declined before any guest-visible effect. */
typedef struct spurs_native_batch {
    uint32_t chain, array, count, stride, descriptor_size, first_index;
    uint32_t lane_count, lanes[6];
    void* runtime;
    void (*run_job)(void* runtime, uint32_t index, uint32_t lane);
    int (*cancelled)(void* runtime);
} spurs_native_batch;
extern int (*g_spurs_native_batch_accept)(const spurs_native_batch*);
extern void (*g_spurs_native_batch_execute)(const spurs_native_batch*);
struct spu_context;
/* Called BEFORE command issue or a potentially external non-DMA channel.
 * May block to acquire publication ownership. Installed before workers, NULL
 * by default. Unlike diagnostic observers this is an execution boundary. */
extern void (*g_spu_shared_access_begin)(struct spu_context*);
/* Optional lane extension for a verified job list. Read-only like accept;
 * returns the total number of concurrent lanes the list may use (0 keeps the
 * guest's maxContention). Lanes past the instance's SPUs are job-only host
 * lanes numbered from the instance SPU count upward. The adapter that returns
 * more lanes must provide every per-SPU resource those numbers index. */
extern unsigned (*g_spurs_lane_extension)(const spurs_native_batch*);
/* Optional translation of a lock-line atomic's effective address, applied
 * before the reservation is taken. Lets an adapter give extra lanes private
 * copies of per-SPU control lines. Returns the EA to use (ea when unchanged). */
extern uint32_t (*g_spu_atomic_ea_remap)(struct spu_context*, uint32_t ea);
#ifdef __cplusplus
}
#endif
#endif
