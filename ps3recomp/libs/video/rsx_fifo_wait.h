#ifndef RSX_FIFO_WAIT_H
#define RSX_FIFO_WAIT_H
#include <stdint.h>
/* A serialized FIFO consumer arms its actual unmet four-byte dependency.
 * GCM_FIFO_PUBLISH_WAKE=1 signals the supplied auto-reset event after a covered
 * producer commit satisfies it, or when an arm/recheck finds it already ready.
 * This signals availability only. The consumer must re-read and validate guest
 * commands and preserve all real GPU completion/resource lifetime conditions.
 * Untracked writers still require the existing bounded wait timeout.
 * Optional audit window: GCM_FIFO_WAIT_AUDIT=start_ms:duration_ms:CSV_path. */
enum { RSX_WAIT_PUT=1, RSX_WAIT_PARK=2, RSX_WAIT_LABEL=3 };
void rsx_fifo_wait_init(void (*wake)(void));
void rsx_fifo_wait_resume(uint32_t frame);
void rsx_fifo_wait_arm(uint32_t kind, uint32_t ea, uint32_t value,
                             int equal_ready, uint32_t frame);
uint64_t rsx_fifo_wait_begin(void);
void rsx_fifo_wait_end(uint64_t begin, int timed_out);
#endif
