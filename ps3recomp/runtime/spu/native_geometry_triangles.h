#ifndef PS3_NATIVE_GEOMETRY_TRIANGLES_H
#define PS3_NATIVE_GEOMETRY_TRIANGLES_H
#include "native_geometry_projection.h"
#if defined(__FMA__)
/* Complete projection + stable triangle selection + aligned publication tail.
 * Indices are BE16 triangle-list data, compacted in place. No allocation,
 * guest context, address map, callbacks or cached snapshots are required. */
typedef struct ps3_geometry_triangles_job {
    ps3_geometry_projection_job projection;
    ps3_geometry_output indices;
    unsigned index_count;
    unsigned cull; /* 0 none; 1 reject positive area; 2 reject negative area */
    unsigned quantization_scale; /* 1 or 2 */
} ps3_geometry_triangles_job;
typedef struct ps3_geometry_triangles_result {
    unsigned index_count,aligned_index_bytes,written_index_bytes;
} ps3_geometry_triangles_result;

static int ps3_geometry_triangles_valid(const ps3_geometry_triangles_job* j) {
    if(!j || !ps3_geometry_project_valid(&j->projection) || !j->index_count ||
       j->index_count>65535 || j->index_count%3 || j->cull>2 ||
       (j->quantization_scale!=1 && j->quantization_scale!=2))return 0;
    const size_t bytes=((size_t)j->index_count*2+15)&~(size_t)15;
    const size_t projected_bytes=(size_t)j->projection.vertices*16;
    if(!j->indices.data || j->indices.bytes<bytes ||
       ps3_geo_overlap(j,sizeof(*j),j->indices.data,bytes) ||
       ps3_geo_overlap(j,sizeof(*j),j->projection.projected.data,projected_bytes) ||
       ps3_geo_overlap(j->indices.data,bytes,j->projection.positions.data,projected_bytes) ||
       ps3_geo_overlap(j->indices.data,bytes,j->projection.projected.data,projected_bytes))return 0;
    for(unsigned k=0;k<j->index_count;k++) {
        const unsigned index=(unsigned)j->indices.data[k*2]*256+j->indices.data[k*2+1];
        if(index>=j->projection.vertices)return 0;
    }
    return 1;
}
/* Internal entry for an immutable, already validated plan. */
static void ps3_geometry_triangles_unchecked(const ps3_geometry_triangles_job* j,ps3_geometry_triangles_result* result) {
    ps3_geometry_project_unchecked(&j->projection);
    const u128 zero=spu_zero(),half=spu_ilhu(0x3f00);
    const u128 scale=spu_ilhu(j->quantization_scale==2?0x4000:0x3f80);
    unsigned kept=0,last_group_kept=0;
    for(unsigned first=0;first<j->index_count;first+=12) {
        unsigned lanes=(j->index_count-first)/3;if(lanes>4)lanes=4;
        unsigned char original[24];memcpy(original,j->indices.data+first*2,lanes*6);
        u128 x[3]={0},y[3]={0},clip[3]={0},qx[3],qy[3];
        for(unsigned lane=0;lane<lanes;lane++)for(unsigned k=0;k<3;k++) {
            const unsigned at=lane*6+k*2;
            const unsigned index=(unsigned)original[at]*256+original[at+1];
            const u128 vertex=ps3_skin4_load(j->projection.projected.data+index*16);
            x[k]._u32[lane]=vertex._u32[0];y[k]._u32[lane]=vertex._u32[1];clip[k]._u32[lane]=vertex._u32[2];
        }
        const u128 dx1=spu_native_fp_fs(x[1],x[0]),dx2=spu_native_fp_fs(x[2],x[0]);
        const u128 dy1=spu_native_fp_fs(y[1],y[0]),dy2=spu_native_fp_fs(y[2],y[0]);
        u128 area=spu_native_fp_fms(dx1,dy2,spu_native_fp_fm(dy1,dx2));
        if(j->cull==2)area=spu_xor(area,spu_ilhu(0x8000));
        const u128 backface=spu_fcgt(area,zero);
        for(unsigned k=0;k<3;k++) {
            qx[k]=spu_cflts(spu_native_fp_fma(x[k],scale,half),173);
            qy[k]=spu_cflts(spu_native_fp_fma(y[k],scale,half),173);
        }
        last_group_kept=0;
        for(unsigned lane=0;lane<lanes;lane++) {
            const unsigned all=clip[0]._u32[lane]&clip[1]._u32[lane]&clip[2]._u32[lane];
            const unsigned any=clip[0]._u32[lane]|clip[1]._u32[lane]|clip[2]._u32[lane];
            int reject=(all&252)!=0;
            if((all&2) && !(any&4)) {
                if(j->cull && backface._u32[lane])reject=1;
                if((qx[0]._u32[lane]==qx[1]._u32[lane] && qx[1]._u32[lane]==qx[2]._u32[lane]) ||
                   (qy[0]._u32[lane]==qy[1]._u32[lane] && qy[1]._u32[lane]==qy[2]._u32[lane]))reject=1;
            }
            if(!reject){
                memcpy(j->indices.data+kept*2,original+lane*6,6);
                kept+=3;last_group_kept+=3;
            }
        }
    }
    /* The original packer emits three aligned 16-byte words for each group.
     * Its third shuffle reads the second packed word twice, so its unused
     * suffix contains a rotated copy, not necessarily zeros. Preserve both
     * the declared output and this bounded write footprint until consumers
     * use owned native buffers. Input bounds redirect extra writes to scratch
     * in the original routine; never reproduce them beyond this allocation. */
    const unsigned prefix_bytes=(kept-last_group_kept)*2;
    unsigned char last_group[32]={0};
    memcpy(last_group,j->indices.data+prefix_bytes,last_group_kept*2);
    const unsigned tail_word=(prefix_bytes&~15u)+32;
    const unsigned alignment=prefix_bytes&15u;
    unsigned written=(prefix_bytes&~15u)+48;
    const unsigned capacity=(j->index_count*2+15)&~15u;
    if(written>capacity)written=capacity;
    memset(j->indices.data+kept*2,0,written-kept*2);
    for(unsigned byte=0;byte<16 && tail_word+byte<written;byte++)
        j->indices.data[tail_word+byte]=last_group[16+((byte+16-alignment)&15u)];
    *result=(ps3_geometry_triangles_result){kept,(kept*2+15)&~15u,written};
}
static int ps3_geometry_triangles(const ps3_geometry_triangles_job* j,ps3_geometry_triangles_result* result) {
    if(!result || !ps3_geometry_triangles_valid(j))return 0;
    if(ps3_geo_overlap(result,sizeof(*result),j,sizeof(*j)) ||
       ps3_geo_overlap(result,sizeof(*result),j->indices.data,j->indices.bytes) ||
       ps3_geo_overlap(result,sizeof(*result),j->projection.positions.data,j->projection.positions.bytes) ||
       ps3_geo_overlap(result,sizeof(*result),j->projection.projected.data,j->projection.projected.bytes))return 0;
    ps3_geometry_triangles_unchecked(j,result);return 1;
}
#endif
#endif
