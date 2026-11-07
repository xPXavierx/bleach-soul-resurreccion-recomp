/* CPU recovery for an owned visibility packet. Retains a fixed index count:
 * selected triangles first, followed by zero-area triangles. No guest context. */
#ifndef PS3_GEOMETRY_VISIBILITY_FALLBACK_H
#define PS3_GEOMETRY_VISIBILITY_FALLBACK_H
#include "native_geometry_visibility_input.h"
#if defined(__FMA__)
static uint32_t ps3_visibility_read32(const unsigned char* p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static u128 ps3_visibility_read_vector(const unsigned char* p) {
    u128 v;for(unsigned k=0;k<4;k++)v._u32[k]=ps3_visibility_read32(p+k*4);return v;
}
static unsigned ps3_visibility_cpu_fallback(const void* data,size_t bytes,void* output,size_t capacity) {
    const unsigned char* p=data;if(!p || bytes<192 || !output)return UINT32_MAX;
    unsigned count=ps3_visibility_read32(p)*3,pos=ps3_visibility_read32(p+4),idx=ps3_visibility_read32(p+8);
    unsigned settings=ps3_visibility_read32(p+12);
    if(!count || count>65535 || pos!=192 || idx<192 || (idx-192)%16 ||
       idx-192>65536 || idx+(size_t)count*4!=bytes || capacity<count*2u || settings>6)return UINT32_MAX;
    static SPU_THREAD_LOCAL unsigned char positions[65536],projected[65536],indices[131072];
    for(unsigned k=0;k<idx-192;k+=4)ps3_geo_write_be32(positions+k,ps3_visibility_read32(p+192+k));
    for(unsigned k=0;k<count;k++) {
        unsigned v=ps3_visibility_read32(p+idx+k*4);if(v>65535)return UINT32_MAX;
        indices[k*2]=(unsigned char)(v>>8);indices[k*2+1]=(unsigned char)v;
    }
    ps3_geometry_triangles_job j={0};j.index_count=count;j.cull=settings&3;j.quantization_scale=(settings&4)?2:1;
    j.indices=(ps3_geometry_output){indices,sizeof indices};j.projection.vertices=(idx-192)/16;
    j.projection.positions=(ps3_geometry_input){positions,idx-192};j.projection.projected=(ps3_geometry_output){projected,idx-192};
    for(unsigned k=0;k<3;k++)j.projection.world_view[k]=ps3_visibility_read_vector(p+16+k*16);
    for(unsigned k=0;k<4;k++)j.projection.projection[k]=ps3_visibility_read_vector(p+64+k*16);
    j.projection.viewport_scale=ps3_visibility_read_vector(p+128);j.projection.viewport_offset=ps3_visibility_read_vector(p+144);
    j.projection.minimum=ps3_visibility_read_vector(p+160);j.projection.maximum=ps3_visibility_read_vector(p+176);
    unsigned old=_mm_getcsr();_mm_setcsr(0xffc0);
    ps3_geometry_triangles_result result;int ok=ps3_geometry_triangles(&j,&result);_mm_setcsr(old);
    if(!ok)return UINT32_MAX;
    memcpy(output,indices,result.index_count*2);memset((unsigned char*)output+result.index_count*2,0,(count-result.index_count)*2);
    return result.index_count;
}
#endif
#endif
