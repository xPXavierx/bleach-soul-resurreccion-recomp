/* Owned geometry crossing producer -> command stream -> render preparation.
 * A token travels in a non-incrementing NV4097 NOP (magic, hi, lo). Addresses
 * alone never identify a job. One renderer, multiple producers; initialize
 * before starting them and destroy only after joining every participant.
 * A consumed lease must be copied to fence-owned GPU storage before release.
 */
#ifndef RSX_GEOMETRY_HANDOFF_H
#define RSX_GEOMETRY_HANDOFF_H
#include <stddef.h>
#include <stdint.h>
#define RSX_GEOMETRY_MARKER_MAGIC UINT32_C(0x47454f31)
#define RSX_GEOMETRY_MARKER_HEADER UINT32_C(0x400c0100)
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rsx_geometry_lease {
    uint64_t token;
    unsigned char* data;
    size_t capacity, bytes;
    uint32_t command_ea, vertices, input_indices, selected_indices;
    uint32_t deferred;
    unsigned char* index_destination;
    unsigned (*cpu_fallback)(const void*,size_t,void*,size_t);
} rsx_geometry_lease;
int rsx_geometry_handoff_init(unsigned slots,size_t bytes_per_slot);
void rsx_geometry_handoff_destroy(void);
rsx_geometry_lease rsx_geometry_handoff_reserve(void);
int rsx_geometry_handoff_publish(const rsx_geometry_lease* lease);
int rsx_geometry_handoff_claim(uint64_t token,uint32_t command_ea,rsx_geometry_lease* out);
void rsx_geometry_handoff_release(uint64_t token);
void rsx_geometry_handoff_cancel(uint64_t token); /* writer only, before publish */
/* Renderer-thread operations. A new marker drops an unconsumed prior marker.
 * The marker's original EA and generation must both match the publication. */
void rsx_geometry_handoff_marker(uint64_t token,uint32_t command_ea);
int rsx_geometry_handoff_command(uint32_t header,uint32_t command_ea,const void* be_args);
rsx_geometry_lease rsx_geometry_handoff_take(void);
void rsx_geometry_handoff_shadow_check(const rsx_geometry_lease*,uint32_t primitive,uint32_t indices,int contiguous);
void rsx_geometry_handoff_report(void);
#ifdef __cplusplus
}
#endif
#endif
