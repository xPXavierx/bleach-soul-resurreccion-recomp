#ifndef BLEACH_NATIVE_GEOMETRY_DECODE_PLAN_H
#define BLEACH_NATIVE_GEOMETRY_DECODE_PLAN_H
#include <stdlib.h>
#include "native_geometry_decode.h"
#if defined(__FMA__)
/* Title adapter only: describe a complete accepted 0x9828 stage in data terms.
 * Unknown stream formats/callbacks use the unmodified original before writes. */
typedef struct bleach_decode_plan {
    ps3_geometry_decode_job job;
    uint32_t descriptor;
    unsigned char result_descriptor[416];
} bleach_decode_plan;
static int bleach_decode_range(uint32_t p,size_t n,size_t capacity) {return p<=capacity && n<=capacity-p;}
static int bleach_decode_prepare(unsigned char* ls,size_t capacity,uint32_t descriptor,
    uint32_t primary,uint32_t secondary,uint32_t stack,bleach_decode_plan* p) {
    if((descriptor&15) || (primary&15) || !bleach_decode_range(descriptor,416,capacity) ||
       stack<1200 || stack>capacity-32)return 0;
    const unsigned char* d=ls+descriptor;
    const unsigned count=((unsigned)d[8]<<8)|d[9],rounded=(count+3)&~3u;
    // Bit4 selects a partial-attribute mode in the original generic decoder.
    // This plan represents complete streams only; preserve that alternate path.
    if(d[2]!=0 || (d[0]&16) || !count || count>4096)return 0;
    memset(p,0,sizeof(*p));p->descriptor=descriptor;memcpy(p->result_descriptor,d,416);
    p->job.vertices=rounded;p->job.output_big_endian=1;
    if(!bleach_decode_range(primary,(size_t)rounded*12,capacity))return 0;
    p->job.xyz_f32_be=(ps3_geometry_input){ls+primary,(size_t)rounded*12};
    unsigned tags[4]={1,0,0,0};uint32_t format_at=0;size_t format_bytes=0;
    if(secondary && d[3]!=6) {
        if(d[3]<=6 || ps3_geo_read_be32(d+0x80)!=0)return 0; /* no custom callback */
        const uint32_t fmt=ps3_geo_read_be32(d+0x70);
        if(!bleach_decode_range(fmt,8,capacity))return 0;
        const unsigned char* f=ls+fmt;
        const unsigned attributes=f[0],stride=f[1];
        if(!attributes || attributes>3 || f[2]!=attributes || f[3] ||
           ps3_geo_read_be32(f+4) || !bleach_decode_range(fmt,8+attributes*8,capacity) ||
           !bleach_decode_range(secondary,(size_t)rounded*stride,capacity))return 0;
        format_at=fmt;format_bytes=8+attributes*8;
        p->job.packed_count=attributes;p->job.packed_stride=stride;
        p->job.packed_vectors_be=(ps3_geometry_input){ls+secondary,(size_t)rounded*stride};
        for(unsigned a=0;a<attributes;a++) {
            const unsigned char* e=f+8+a*8;
            if(e[1]!=6 || e[2]!=1 || e[3]!=a+2 || e[4]!=4 || e[6] || e[7])return 0;
            p->job.packed_offset[a]=e[0];tags[a+1]=e[3];
        }
    }
    const size_t bytes=(size_t)rounded*16;
    for(unsigned a=0;a<=p->job.packed_count;a++) {
        unsigned slot=0;
        while(slot<16 && p->result_descriptor[0x160+slot])++slot;
        if(slot==16)return 0;
        const uint32_t dst=ps3_geo_read_be32(d+0x10+4*slot);
        if(!dst || (dst&15) || !bleach_decode_range(dst,bytes,capacity) ||
           ps3_geo_overlap(ls+dst,bytes,ls+descriptor,416) ||
           ps3_geo_overlap(ls+dst,bytes,ls+format_at,format_bytes) ||
           ps3_geo_overlap(ls+dst,bytes,ls+stack-1200,1232))return 0;
        p->job.vectors[a]=(ps3_geometry_output){ls+dst,bytes};
        p->result_descriptor[0x160+slot]=(unsigned char)tags[a];
        ps3_geo_write_be32(p->result_descriptor+0x58+4*tags[a],dst);
    }
    if(ps3_geo_overlap(ls+descriptor,416,ls+stack-1200,1232) ||
       ps3_geo_overlap(p->job.xyz_f32_be.data,p->job.xyz_f32_be.bytes,ls+stack-1200,1232) ||
       ps3_geo_overlap(p->job.packed_vectors_be.data,p->job.packed_vectors_be.bytes,ls+stack-1200,1232) ||
       ps3_geo_overlap(ls+descriptor,416,p->job.xyz_f32_be.data,p->job.xyz_f32_be.bytes) ||
       ps3_geo_overlap(ls+descriptor,416,p->job.packed_vectors_be.data,p->job.packed_vectors_be.bytes))return 0;
    ps3_geo_write_be32(p->result_descriptor+0x54,primary);
    ps3_geo_write_be32(p->result_descriptor+0x58,secondary && secondary<primary?secondary:primary);
    return ps3_geometry_decode_valid(&p->job);
}
static void bleach_decode_execute(unsigned char* ls,const bleach_decode_plan* p) {
    if(!ps3_geometry_decode(&p->job))abort();
    memcpy(ls+p->descriptor,p->result_descriptor,416);
}
#endif
#endif
