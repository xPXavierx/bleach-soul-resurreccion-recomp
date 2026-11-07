/* Owned input for GPU visibility. This serializes data, never guest pointers.
 * Caller leases output storage until GPU completion. No allocation or global
 * state; write before the source descriptor/local-store buffers are reused.
 * Does not authorize skipping CPU consumers: publication/draw identity is a
 * separate contract, still required by the title integration. */
#ifndef PS3_NATIVE_GEOMETRY_VISIBILITY_INPUT_H
#define PS3_NATIVE_GEOMETRY_VISIBILITY_INPUT_H
#include "native_geometry_triangles.h"
#if defined(__FMA__)
static void ps3_visibility_le32(unsigned char* p,uint32_t x) {
    p[0]=(unsigned char)x;p[1]=(unsigned char)(x>>8);p[2]=(unsigned char)(x>>16);p[3]=(unsigned char)(x>>24);
}
static void ps3_visibility_vector(unsigned char* p,u128 v) {
    for(unsigned c=0;c<4;c++)ps3_visibility_le32(p+c*4,v._u32[c]);
}
static size_t ps3_geometry_visibility_write(const ps3_geometry_triangles_job* j,
    void* output,size_t capacity) {
    if(!ps3_geometry_triangles_valid(j) || !output)return 0;
    const size_t positions=(size_t)j->projection.vertices*16;
    const size_t bytes=192+positions+(size_t)j->index_count*4;
    if(bytes>capacity || ps3_geo_overlap(output,bytes,j,sizeof(*j)) ||
       ps3_geo_overlap(output,bytes,j->projection.positions.data,positions) ||
       ps3_geo_overlap(output,bytes,j->indices.data,j->index_count*2))return 0;
    unsigned char* p=(unsigned char*)output;
    ps3_visibility_le32(p,j->index_count/3);ps3_visibility_le32(p+4,192);
    ps3_visibility_le32(p+8,(uint32_t)(192+positions));
    ps3_visibility_le32(p+12,j->cull|(j->quantization_scale==2?4:0));
    for(unsigned i=0;i<3;i++)ps3_visibility_vector(p+16+i*16,j->projection.world_view[i]);
    for(unsigned i=0;i<4;i++)ps3_visibility_vector(p+64+i*16,j->projection.projection[i]);
    ps3_visibility_vector(p+128,j->projection.viewport_scale);ps3_visibility_vector(p+144,j->projection.viewport_offset);
    ps3_visibility_vector(p+160,j->projection.minimum);ps3_visibility_vector(p+176,j->projection.maximum);
    for(unsigned i=0;i<j->projection.vertices;i++)
        ps3_visibility_vector(p+192+i*16,ps3_skin4_load(j->projection.positions.data+i*16));
    for(unsigned i=0;i<j->index_count;i++)
        ps3_visibility_le32(p+192+positions+i*4,(unsigned)j->indices.data[i*2]*256+j->indices.data[i*2+1]);
    return bytes;
}
#endif
#endif
