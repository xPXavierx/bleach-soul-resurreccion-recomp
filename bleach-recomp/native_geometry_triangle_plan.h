#ifndef BLEACH_NATIVE_GEOMETRY_TRIANGLE_PLAN_H
#define BLEACH_NATIVE_GEOMETRY_TRIANGLE_PLAN_H
#include "native_geometry_decode_plan.h"
#include "native_geometry_triangles.h"
#if defined(__FMA__)
typedef struct bleach_triangle_plan {
    ps3_geometry_triangles_job job;
    uint32_t descriptor,index_address;
} bleach_triangle_plan;
static unsigned bleach_triangle_be16(const unsigned char* p){return (unsigned)p[0]*256+p[1];}
static int bleach_triangle_prepare(unsigned char* ls,size_t capacity,uint32_t descriptor,
    unsigned mode,unsigned vertex_offset,uint32_t stack,bleach_triangle_plan* p) {
    if((descriptor&15) || stack<384 || stack>capacity-32 || vertex_offset ||
       mode<1 || mode>3 || !bleach_decode_range(descriptor,416,capacity) ||
       ps3_geo_overlap(ls+descriptor,416,ls+stack-384,416))return 0;
    const unsigned char* d=ls+descriptor;
    const unsigned count=bleach_triangle_be16(d+8),indices=bleach_triangle_be16(d+0x190);
    if(!(d[0]&128) || ps3_geo_read_be32(d+0x184) || d[0x140]>4 ||
       !count || count>4096 || !indices || indices%3)return 0;
    memset(p,0,sizeof(*p));p->descriptor=descriptor;
    ps3_geometry_triangles_job* j=&p->job;
    j->index_count=indices;j->projection.vertices=(count+3)&~3u;
    j->quantization_scale=d[0x140]==3 || d[0x140]==4?2:1;
    const unsigned winding=d[6]>>4;
    const int reverse=(mode==3)^(winding==0 || winding==2);
    j->cull=mode==1?0:reverse?2:1;
    const uint32_t addresses[3]={ps3_geo_read_be32(d+0x5c),
        ps3_geo_read_be32(d+0x10+(d[0]&15)*4),ps3_geo_read_be32(d+0x50)};
    const size_t sizes[3]={(size_t)j->projection.vertices*16,(size_t)j->projection.vertices*16,
        ((size_t)indices*2+15)&~(size_t)15};
    for(unsigned k=0;k<3;k++) {
        if(!addresses[k] || (addresses[k]&15) || !bleach_decode_range(addresses[k],sizes[k],capacity) ||
           ps3_geo_overlap(ls+addresses[k],sizes[k],ls+descriptor,416) ||
           ps3_geo_overlap(ls+addresses[k],sizes[k],ls+stack-384,416))return 0;
    }
    j->projection.positions=(ps3_geometry_input){ls+addresses[0],sizes[0]};
    j->projection.projected=(ps3_geometry_output){ls+addresses[1],sizes[1]};
    j->indices=(ps3_geometry_output){ls+addresses[2],sizes[2]};p->index_address=addresses[2];
    for(unsigned k=0;k<3;k++)j->projection.world_view[k]=ps3_skin4_load(d+0xa0+k*16);
    for(unsigned k=0;k<4;k++)j->projection.projection[k]=ps3_skin4_load(d+0xe0+k*16);
    j->projection.viewport_scale=ps3_skin4_load(d+0x120);
    j->projection.viewport_offset=ps3_skin4_load(d+0x130);
    u128 bounds=spu_zero();bounds._u32[0]=bleach_triangle_be16(d+0xd0);bounds._u32[1]=bleach_triangle_be16(d+0xd2);
    bounds._u32[2]=(bounds._u32[0]+bleach_triangle_be16(d+0xd4))&65535;
    bounds._u32[3]=(bounds._u32[1]+bleach_triangle_be16(d+0xd6))&65535;
    bounds=spu_cuflt(bounds,155);
    j->projection.minimum._u32[0]=bounds._u32[0];j->projection.minimum._u32[1]=bounds._u32[1];
    j->projection.maximum._u32[0]=bounds._u32[2];j->projection.maximum._u32[1]=bounds._u32[3];
    u128 z0=spu_zero(),z1=spu_zero();z0._u32[0]=ps3_geo_read_be32(d+0xd8);z1._u32[0]=ps3_geo_read_be32(d+0xdc);
    const int swap=spu_fcgt(z0,z1)._u32[0]!=0;
    j->projection.minimum._u32[2]=swap?z1._u32[0]:z0._u32[0];
    j->projection.maximum._u32[2]=swap?z0._u32[0]:z1._u32[0];
    return ps3_geometry_triangles_valid(j);
}
static unsigned bleach_triangle_execute(unsigned char* ls,const bleach_triangle_plan* p) {
    ps3_geometry_triangles_result result;
    ps3_geometry_triangles_unchecked(&p->job,&result);
    ls[p->descriptor+0x190]=(unsigned char)(result.index_count>>8);
    ls[p->descriptor+0x191]=(unsigned char)result.index_count;
    ps3_geo_write_be32(ls+p->descriptor+0x58,p->index_address+result.aligned_index_bytes);
    return result.index_count;
}
#endif
#endif
