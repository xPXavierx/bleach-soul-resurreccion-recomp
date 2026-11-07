/* Owned GPU vertex inputs. Source spans remain borrowed only until write().
 * The resulting immutable packet belongs to its GPU submission/fence. */
#ifndef PS3RECOMP_RSX_VERTEX_PULL_H
#define PS3RECOMP_RSX_VERTEX_PULL_H
#include "rsx_vertex_compact.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rsx_vertex_pull_attr {
    u32 offset, stride, first, last;
    u32 type, size, frequency, modulo;
    float defaults[4];
} rsx_vertex_pull_attr;
typedef struct rsx_vertex_pull_header {
    u32 version, count, mask, refs_offset;
    rsx_vertex_pull_attr attr[16];
} rsx_vertex_pull_header;
typedef struct rsx_vertex_pull_span {
    const u8* source;
    u32 size, offset;
} rsx_vertex_pull_span;
typedef struct rsx_vertex_pull_plan {
    rsx_vertex_pull_header header;
    const rsx_vertex_ref* refs;
    rsx_vertex_pull_span spans[16];
    u32 span_count, bytes;
} rsx_vertex_pull_plan;
/* Returns zero without changing output on an unsupported/unmapped/oversize
 * input. Caller then uses the existing CPU decoder, including its fallbacks. */
int rsx_vertex_pull_prepare(rsx_vertex_pull_plan* out,
    const rsx_vertex_fetch_plan* fetch, const rsx_vertex_ref* refs,
    u32 count, u32 capacity);
int rsx_vertex_pull_write(const rsx_vertex_pull_plan* plan, void* destination, u32 capacity);
/* GPU-generated indices address the original vertices directly. No CPU
 * index expansion/remapping or serialized refs table. Fetch spans must cover
 * [0,count) with base zero, frequency<=1 and no inline source. Ownership is
 * unchanged: write() copies attributes into the submission-owned packet. */
int rsx_vertex_pull_prepare_linear(rsx_vertex_pull_plan* out,
    const rsx_vertex_fetch_plan* fetch, u32 count, u32 capacity);
/* The same shader decoder is used by the production VS and the GPU oracle. */
const char* rsx_vertex_pull_hlsl(void);
int rsx_vertex_pull_wrap(char* shader, u32 capacity, u32 input_mask);
#ifdef __cplusplus
}
#endif
#endif
