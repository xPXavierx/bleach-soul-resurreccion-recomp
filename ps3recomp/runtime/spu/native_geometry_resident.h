#ifndef PS3_NATIVE_GEOMETRY_RESIDENT_H
#define PS3_NATIVE_GEOMETRY_RESIDENT_H
#include <stddef.h>
#include <stdint.h>
/* Retained, view-independent geometry values. The adapter supplies ALL pure
 * computation inputs, including format and padding dependencies. Addresses,
 * draw order and frame counters are not identities. Hashes select candidates;
 * full byte equality authorizes reuse. This is NOT temporal instance history.
 * A lease pins immutable data until publication into the current consumer.
 * No guest registers, LS snapshots, DMA or GPU resources are retained here. */
typedef struct ps3_geo_resident_input { const void* data; size_t bytes; } ps3_geo_resident_input;
typedef struct ps3_geo_resident_output { void* data; size_t bytes; } ps3_geo_resident_output;
typedef struct ps3_geo_resident_lease ps3_geo_resident_lease;
typedef struct ps3_geo_resident_stats {
    uint64_t hits,misses,declined,published,evicted,input_bytes,output_bytes,bytes;
} ps3_geo_resident_stats;
#ifdef __cplusplus
extern "C" {
#endif
ps3_geo_resident_lease* ps3_geo_resident_acquire(const ps3_geo_resident_input*,unsigned,
    const ps3_geo_resident_output*,unsigned,int* hit);
int ps3_geo_resident_restore(ps3_geo_resident_lease*,const ps3_geo_resident_output*,unsigned);
int ps3_geo_resident_publish(ps3_geo_resident_lease*,const ps3_geo_resident_output*,unsigned);
void ps3_geo_resident_release(ps3_geo_resident_lease*);
void ps3_geo_resident_get_stats(ps3_geo_resident_stats*);
/* Only after all producers/consumers have stopped. */
void ps3_geo_resident_reset(void);
#ifdef __cplusplus
}
#endif
#endif
