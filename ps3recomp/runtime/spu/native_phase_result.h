#ifndef PS3_NATIVE_PHASE_RESULT_H
#define PS3_NATIVE_PHASE_RESULT_H
#include "spu_context.h"
#include <string.h>
/* A capture belongs to the executing host worker. Only private, deterministic
 * phases may use this API. Every memory access and register write is covered. */
typedef struct ps3_phase_capture {
    uint64_t reads[256],writes[256],register_writes[2];
    uint8_t input[SPU_LS_SIZE];
    int aborted;
} ps3_phase_capture;
#ifdef __cplusplus
extern "C" {
#endif
extern SPU_THREAD_LOCAL ps3_phase_capture* g_ps3_phase_capture;
void ps3_phase_result_run(spu_context*,uint64_t,const uint8_t*,unsigned,
                         void(*)(spu_context*),uint64_t);
typedef struct ps3_phase_result_counters {
    uint64_t hits,misses,stored,declined,evicted,bytes;
} ps3_phase_result_counters;
void ps3_phase_result_stats(ps3_phase_result_counters*);
/* Diagnostic only, scoped to one invocation on this host worker. Durations
 * are service time, not frame-critical time. Never nest probes. */
typedef struct ps3_phase_result_probe {
    uint64_t lookup_ns,setup_ns,compute_ns,collect_ns,publish_ns,apply_ns,total_ns;
    uint64_t input_bytes,output_bytes;
    unsigned hit,valid;
} ps3_phase_result_probe;
uint64_t ps3_phase_result_clock_ns(void);
void ps3_phase_result_probe_begin(void);
void ps3_phase_result_probe_end(ps3_phase_result_probe*);
/* Test/host shutdown only: callers must have stopped all phase execution. */
void ps3_phase_result_reset(void);
#ifdef __cplusplus
}
#endif
static inline void ps3_phase_result_abort(void)
{if(g_ps3_phase_capture)g_ps3_phase_capture->aborted=1;}
static inline void ps3_phase_result_mark_gprs(uint64_t low,uint64_t high)
{
    ps3_phase_capture* c=g_ps3_phase_capture;
    if(c){c->register_writes[0]|=low;c->register_writes[1]|=high;}
}
static inline u128 ps3_phase_result_load(const uint8_t* ls,uint32_t address)
{
    address&=SPU_LS_MASK&~15u;
    ps3_phase_capture* c=g_ps3_phase_capture;
    if(c){unsigned block=address>>4,word=block>>6;uint64_t bit=UINT64_C(1)<<(block&63);
        if(!((c->reads[word]|c->writes[word])&bit)) {
            c->reads[word]|=bit;memcpy(c->input+address,ls+address,16);
        }}
    u128 result;memcpy(&result,ls+address,16);
    for(unsigned i=0;i<4;i++)result._u32[i]=__builtin_bswap32(result._u32[i]);
    return result;
}
static inline void ps3_phase_result_store(uint8_t* ls,uint32_t address,u128 value)
{
    address&=SPU_LS_MASK&~15u;
    ps3_phase_capture* c=g_ps3_phase_capture;
    if(c){unsigned block=address>>4;c->writes[block>>6]|=UINT64_C(1)<<(block&63);}
    for(unsigned i=0;i<4;i++)value._u32[i]=__builtin_bswap32(value._u32[i]);
    memcpy(ls+address,&value,16);
}
#endif
