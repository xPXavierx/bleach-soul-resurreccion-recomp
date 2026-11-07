#ifndef BLEACH_NATIVE_GEOMETRY_POSE_PLAN_H
#define BLEACH_NATIVE_GEOMETRY_POSE_PLAN_H
#include "native_geometry_decode_plan.h"
#include "native_geometry_pose.h"
#if defined(__FMA__)
typedef struct bleach_pose_plan {
    ps3_geometry_pose_job job;
    uint32_t descriptor,first_free;
    unsigned char local_frames[192];
} bleach_pose_plan;
static int bleach_pose_span(unsigned char* ls,size_t capacity,uint32_t address,size_t bytes,
    uint32_t descriptor,uint32_t stack,ps3_geometry_output* span) {
    if(!address || (address&15) || !bleach_decode_range(address,bytes,capacity) ||
       ps3_geo_overlap(ls+address,bytes,ls+descriptor,416) ||
       ps3_geo_overlap(ls+address,bytes,ls+stack-1200,1232))return 0;
    *span=(ps3_geometry_output){ls+address,bytes};return 1;
}
static int bleach_pose_prepare_descriptor(unsigned char* ls,size_t capacity,uint32_t descriptor,
    uint32_t palette,unsigned matrices,uint32_t influences,uint32_t stack,bleach_pose_plan* p,
    const unsigned char* d) {
    if((descriptor&15) || stack<1200 || stack>capacity-32 ||
       !bleach_decode_range(descriptor,416,capacity) ||
       ps3_geo_overlap(ls+descriptor,416,ls+stack-1200,1232))return 0;
    const unsigned count=((unsigned)d[8]<<8)|d[9],rounded=(count+3)&~3u;
    // Full enabled skinning, four influences/direct normal transform with
    // normalization (mode2), row-major3x4 palette. Other modes stay original.
    if((d[0]&0x90)!=0x80 || (d[6]&15)!=2 || d[7]!=0 || !count || count>4096 ||
       !matrices || matrices>256)return 0;
    memset(p,0,sizeof(*p));p->descriptor=descriptor;
    p->first_free=palette<influences?palette:influences;
    ps3_geometry_pose_job* j=&p->job;
    j->palette_entries=matrices;j->vertices=rounded;j->normalize_vertices=(count+7)&~7u;
    j->chunk_vertices=rounded>12?(rounded/3)&~3u:4;
    if(!bleach_pose_span(ls,capacity,palette,(size_t)matrices*48,descriptor,stack,&j->palette) ||
       !bleach_pose_span(ls,capacity,ps3_geo_read_be32(d+0x5c),(size_t)rounded*16,descriptor,stack,&j->positions))return 0;
    ps3_geometry_output influence_span;
    if(!bleach_pose_span(ls,capacity,influences,(size_t)rounded*8,descriptor,stack,&influence_span))return 0;
    j->influences=(ps3_geometry_input){influence_span.data,influence_span.bytes};
    j->tail_start=count==rounded?rounded:rounded-4;
    for(unsigned v=0;v<count;v++)for(unsigned k=0;k<4;k++)
        if(j->influences.data[v*8+k*2+1]>=matrices)return 0;
    for(unsigned v=j->tail_start;v<rounded;v++)for(unsigned k=0;k<4;k++) {
        uint32_t at=palette+j->influences.data[v*8+k*2+1]*48;
        if(!bleach_decode_range(at,48,capacity) ||
           ps3_geo_overlap(ls+at,48,ls+stack-1200,1232))return 0;
        j->tail_sources[v-j->tail_start][k]=(ps3_geometry_input){ls+at,48};
    }
    for(unsigned i=0;i<3;i++) {
        uint32_t address=ps3_geo_read_be32(d+0x60+i*4);
        if(address && !bleach_pose_span(ls,capacity,address,(size_t)j->normalize_vertices*16,
            descriptor,stack,&j->directions[j->direction_count++]))return 0;
    }
    if(rounded>12) {
        uint32_t frames=ps3_geo_read_be32(d+0x10+4*(d[0]&15));
        if(!bleach_pose_span(ls,capacity,frames,(size_t)j->chunk_vertices*48,descriptor,stack,&j->frames))return 0;
    } else j->frames=(ps3_geometry_output){p->local_frames,sizeof(p->local_frames)};
    return ps3_geometry_pose_valid(j);
}
static int bleach_pose_prepare(unsigned char* ls,size_t capacity,uint32_t descriptor,
    uint32_t palette,unsigned matrices,uint32_t influences,uint32_t stack,bleach_pose_plan* p) {
    if(!bleach_decode_range(descriptor,416,capacity))return 0;
    return bleach_pose_prepare_descriptor(ls,capacity,descriptor,palette,matrices,influences,stack,p,ls+descriptor);
}
static void bleach_pose_execute(unsigned char* ls,const bleach_pose_plan* p) {
    if(!ps3_geometry_pose(&p->job))abort();
    ps3_geo_write_be32(ls+p->descriptor+0x58,p->first_free);
}
#endif
#endif
