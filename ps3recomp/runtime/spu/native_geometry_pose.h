#ifndef PS3_NATIVE_GEOMETRY_POSE_H
#define PS3_NATIVE_GEOMETRY_POSE_H
#include "native_geometry_decode.h"
#include "spu_skinning_cyclic4.h"
#if defined(__FMA__)
/* Complete four-influence pose stage. Palette starts as row-major3x4 and is
 * converted in place to cyclic diagonals. Positions/directions are in/out.
 * Scratch is caller-owned, reused by chunks; its final contents are retained.
 * There is no guest context, allocation, dispatch or snapshot cache here. */
typedef struct ps3_geometry_pose_job {
    ps3_geometry_output palette,positions,directions[3],frames;
    ps3_geometry_input influences;
    unsigned palette_entries,vertices,normalize_vertices,chunk_vertices,direction_count;
    /* Optional final four vertices have explicit matrix sources. Legacy SIMD
     * padding can reference adjacent storage rather than real palette entries.
     * These sources may alias outputs: snapshot both vertices of a pair before
     * publishing that pair. Keep this compatibility tail outside the main loop. */
    unsigned tail_start;
    ps3_geometry_input tail_sources[4][4];
} ps3_geometry_pose_job;

static int ps3_geometry_pose_valid(const ps3_geometry_pose_job* j) {
    if(!j || !j->palette_entries || j->palette_entries>256 || !j->vertices ||
       j->vertices>4096 || (j->vertices&3) || j->direction_count>3 ||
       j->normalize_vertices<j->vertices || j->normalize_vertices>j->vertices+4 ||
       (j->normalize_vertices&7) || !j->chunk_vertices || (j->chunk_vertices&3) ||
       j->chunk_vertices>j->vertices || (j->tail_start!=j->vertices && j->tail_start!=j->vertices-4) || !j->influences.data ||
       j->influences.bytes<(size_t)j->vertices*8)return 0;
    ps3_geometry_output spans[6]={j->palette,j->positions,j->frames};
    size_t needed[6]={(size_t)j->palette_entries*48,(size_t)j->vertices*16,(size_t)j->chunk_vertices*48};
    for(unsigned i=0;i<j->direction_count;i++) {
        spans[3+i]=j->directions[i];needed[3+i]=(size_t)j->normalize_vertices*16;
    }
    for(unsigned i=0;i<3+j->direction_count;i++) {
        if(!spans[i].data || spans[i].bytes<needed[i] ||
           ps3_geo_overlap(spans[i].data,needed[i],j->influences.data,j->influences.bytes))return 0;
        for(unsigned k=0;k<i;k++)if(ps3_geo_overlap(spans[i].data,needed[i],spans[k].data,needed[k]))return 0;
    }
    for(unsigned i=1;i<j->tail_start*8;i+=2)if(j->influences.data[i]>=j->palette_entries)return 0;
    for(unsigned v=j->tail_start;v<j->vertices;v++)for(unsigned k=0;k<4;k++) {
        const ps3_geometry_input* s=&j->tail_sources[v-j->tail_start][k];
        if(!s->data || s->bytes<48)return 0;
    }
    return 1;
}

static u128 ps3_pose_rotate3(u128 v,unsigned by) {
    u128 r=v;for(unsigned k=0;k<3;k++)r._u32[k]=v._u32[(k+by)%3];return r;
}
static void ps3_pose_normalize(uint8_t* data,unsigned vertices) {
    /* Four independent lengths in SIMD lanes. Preserve original x*x then
     * FMA(y,y,sum), FMA(z,z,sum), reciprocal estimate/interpolation and w. */
    for(unsigned at=0;at<vertices;at+=4) {
        u128 v[4],x,y,z;
        for(unsigned k=0;k<4;k++) {
            v[k]=ps3_skin4_load(data+(at+k)*16);
            x._u32[k]=v[k]._u32[0];y._u32[k]=v[k]._u32[1];z._u32[k]=v[k]._u32[2];
        }
        u128 length=spu_native_fp_fm(x,x);
        length=spu_native_fp_fma(y,y,length);length=spu_native_fp_fma(z,z,length);
        const u128 inverse=spu_fi(length,spu_frsqest(length));
        for(unsigned k=0;k<4;k++) {
            u128 scale=ps3_skin4_splat(inverse,k);scale._u32[3]=0x3f800000;
            ps3_skin4_store(data+(at+k)*16,spu_native_fp_fm(v[k],scale));
        }
    }
}
static int ps3_geometry_pose(const ps3_geometry_pose_job* j) {
    if(!ps3_geometry_pose_valid(j))return 0;
    static const unsigned perm[12]={0,5,10,3,1,6,8,7,2,4,9,11};
    for(unsigned i=0;i<j->palette_entries;i++) {
        uint32_t original[12],cyclic[12];memcpy(original,j->palette.data+i*48,48);
        for(unsigned k=0;k<12;k++)cyclic[k]=original[perm[k]];
        memcpy(j->palette.data+i*48,cyclic,48);
    }
    for(unsigned first=0;first<j->vertices;) {
        unsigned count=j->vertices-first;if(count>j->chunk_vertices)count=j->chunk_vertices;
        unsigned fast=count;
        if(first+fast>j->tail_start)fast=j->tail_start-first;
        if(fast) {
            ps3_skin4_batch skin={j->palette.data,j->influences.data+first*8,
                j->positions.data+first*16,j->frames.data,fast,j->palette_entries,1};
            ps3_skin4_run(&skin);
        }
        for(unsigned v=fast;v<count;v+=2) {
            uint8_t palette[384],influences[16],positions[32],frames[96];
            memcpy(influences,j->influences.data+(first+v)*8,16);
            memcpy(positions,j->positions.data+(first+v)*16,32);
            for(unsigned pair=0;pair<2;pair++)for(unsigned k=0;k<4;k++) {
                memcpy(palette+(pair*4+k)*48,j->tail_sources[first+v+pair-j->tail_start][k].data,48);
                influences[pair*8+k*2+1]=(uint8_t)(pair*4+k);
            }
            ps3_skin4_batch skin={palette,influences,positions,frames,2,8,0};
            ps3_skin4_checked(&skin);
            memcpy(j->frames.data+v*48,frames,96);
            memcpy(j->positions.data+(first+v)*16,positions,32);
        }
        for(unsigned v=0;v<count;v++) {
            const u128 a=ps3_skin4_load(j->frames.data+v*48),
                       b=ps3_skin4_load(j->frames.data+v*48+16),
                       c=ps3_skin4_load(j->frames.data+v*48+32);
            for(unsigned d=0;d<j->direction_count;d++) {
                uint8_t* dst=j->directions[d].data+(first+v)*16;
                u128 value=ps3_skin4_load(dst),result=spu_native_fp_fm(value,a);
                result=spu_native_fp_fma(ps3_pose_rotate3(value,1),b,result);
                result=spu_native_fp_fma(ps3_pose_rotate3(value,2),c,result);
                ps3_skin4_store(dst,result);
            }
        }
        first+=count;
    }
    for(unsigned d=0;d<j->direction_count;d++)ps3_pose_normalize(j->directions[d].data,j->normalize_vertices);
    return 1;
}
#endif
#endif
