#ifndef PS3_NATIVE_GEOMETRY_DECODE_H
#define PS3_NATIVE_GEOMETRY_DECODE_H
/* Data-level geometry decoding. No SPU context, local-store addresses, register
 * reconstruction, allocation, callbacks or publication live in this kernel.
 * The caller owns the input/output spans for the entire call. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#if defined(__FMA__)
#include <immintrin.h>
typedef struct ps3_geometry_input { const unsigned char* data; size_t bytes; } ps3_geometry_input;
typedef struct ps3_geometry_output { unsigned char* data; size_t bytes; } ps3_geometry_output;
typedef struct ps3_geometry_decode_job {
    ps3_geometry_input xyz_f32_be,packed_vectors_be;
    ps3_geometry_output vectors[4]; /* position followed by optional directions */
    unsigned vertices,packed_stride,packed_count,packed_offset[3];
    int output_big_endian;
} ps3_geometry_decode_job;
static inline uint32_t ps3_geo_read_be32(const unsigned char* p) {
    uint32_t v;memcpy(&v,p,4);return __builtin_bswap32(v);
}
static inline void ps3_geo_write_be32(unsigned char* p,uint32_t v) {
    v=__builtin_bswap32(v);memcpy(p,&v,4);
}
static inline int ps3_geo_overlap(const void* a,size_t an,const void* b,size_t bn) {
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return an && bn && (x<=y?y-x<an:x-y<bn);
}
static inline int ps3_geometry_decode_valid(const ps3_geometry_decode_job* j) {
    if(!j || !j->vertices || j->vertices>65536 || j->packed_count>3 ||
       !j->xyz_f32_be.data || j->xyz_f32_be.bytes<(size_t)j->vertices*12)return 0;
    const size_t out_bytes=(size_t)j->vertices*16;
    if(j->packed_count && (!j->packed_vectors_be.data || j->packed_stride<4 ||
       j->packed_stride>256 || j->packed_vectors_be.bytes<(size_t)j->vertices*j->packed_stride))return 0;
    for(unsigned k=0;k<j->packed_count;k++)if(j->packed_offset[k]>j->packed_stride-4)return 0;
    for(unsigned k=0;k<=j->packed_count;k++) {
        const ps3_geometry_output* o=&j->vectors[k];
        if(!o->data || o->bytes<out_bytes || ps3_geo_overlap(o->data,out_bytes,j->xyz_f32_be.data,j->xyz_f32_be.bytes) ||
           ps3_geo_overlap(o->data,out_bytes,j->packed_vectors_be.data,j->packed_vectors_be.bytes))return 0;
        for(unsigned t=0;t<k;t++)if(ps3_geo_overlap(o->data,out_bytes,j->vectors[t].data,out_bytes))return 0;
    }
    return 1;
}
static inline int ps3_geometry_decode(const ps3_geometry_decode_job* j) {
    if(!ps3_geometry_decode_valid(j))return 0; /* no output before full preflight */
    const __m128i endian=_mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    const __m128 inverse=_mm_setr_ps(0x1p-10f,0x1p-10f,0x1p-9f,0);
    const __m128 scale=_mm_castsi128_ps(_mm_setr_epi32(0x3f801002,0x3f801002,0x3f802008,0));
    const __m128 bias=_mm_castsi128_ps(_mm_setr_epi32(0x3a001002,0x3a001002,0x3a802008,0x3f800000));
    for(unsigned i=0;i<j->vertices;i++) {
        const unsigned char* in=j->xyz_f32_be.data+(size_t)i*12;
        unsigned char* out=j->vectors[0].data+(size_t)i*16;
        if(j->output_big_endian) {
            memcpy(out,in,12);ps3_geo_write_be32(out+12,0x3f800000);
        } else {
            uint32_t values[4]={ps3_geo_read_be32(in),ps3_geo_read_be32(in+4),ps3_geo_read_be32(in+8),0x3f800000};
            memcpy(out,values,16);
        }
    }
    for(unsigned k=0;k<j->packed_count;k++)for(unsigned i=0;i<j->vertices;i++) {
        const uint32_t word=ps3_geo_read_be32(j->packed_vectors_be.data+(size_t)i*j->packed_stride+j->packed_offset[k]);
        const int32_t x=(int32_t)(word<<21)>>21,y=(int32_t)(word<<10)>>21,z=(int32_t)word>>22;
        const __m128 v=_mm_mul_ps(_mm_cvtepi32_ps(_mm_setr_epi32(x,y,z,0)),inverse);
        __m128i bits=_mm_castps_si128(_mm_fmadd_ps(v,scale,bias));
        if(j->output_big_endian)bits=_mm_shuffle_epi8(bits,endian);
        _mm_storeu_si128((__m128i*)(j->vectors[k+1].data+(size_t)i*16),bits);
    }
    return 1;
}
#endif
#endif
