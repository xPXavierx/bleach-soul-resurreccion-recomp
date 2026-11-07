#ifndef PS3_NATIVE_GEOMETRY_TRACE_H
#define PS3_NATIVE_GEOMETRY_TRACE_H
#include "ps3emu/ps3_ftrace.h"
/* Diagnostic wall-clock spans, nested inside SPURS jobs. They overlap across
 * threads and must never be summed as serial frame latency. No observer is
 * installed: resident geometry and all normal side effects stay enabled.
 * kinds: 100 prepare, 101 acquire (includes 108/109/110), 102 rebind/between,
 * 103 restore, 104 miss compute, 105 cache publish, 106 release, 107 total,
 * 108 input hash, 109 acquire lock wait, 110 exact candidate comparison.
 * a=vertices where known; b=hit (1), miss (0), or declined (2).
 */
static inline uint64_t ps3_geo_trace_begin(void) {
    if (!PS3_FT_ON()) return 0;
    const uint64_t t=ps3_ft_now();
    return t>=g_ps3_ft.start && t<g_ps3_ft.end ? t : 0;
}
static inline void ps3_geo_trace_end(unsigned kind,unsigned a,unsigned b,uint64_t start) {
    if (!start) return;
#ifdef _WIN32
    const uint32_t who=GetCurrentThreadId();
#else
    const uint32_t who=0; /* Windows host thread attribution in this diagnostic. */
#endif
    ps3_ft_span((int)kind,who,a,b,start,ps3_ft_now());
}
#endif
