/* Data-level projection. Caller owns AVX2/FMA dispatch and MXCSR RTZ/DAZ/FTZ.
 * Inputs and outputs remain live and immutable in size throughout the call. */
#ifndef PS3_NATIVE_GEOMETRY_PROJECTION_H
#define PS3_NATIVE_GEOMETRY_PROJECTION_H
#include "native_geometry_decode.h"
#include "spu_skinning_cyclic4.h"
#if defined(__FMA__)

typedef struct ps3_geometry_projection_job {
    ps3_geometry_input positions;
    ps3_geometry_output projected;
    unsigned vertices;
    u128 world_view[3],projection[4],viewport_scale,viewport_offset;
    u128 minimum,maximum;
} ps3_geometry_projection_job;

static int ps3_geometry_project_valid(const ps3_geometry_projection_job* j) {
    if(!j || !j->vertices || j->vertices>4096 || (j->vertices&3) ||
       !j->positions.data || !j->projected.data ||
       j->positions.bytes<(size_t)j->vertices*16 ||
       j->projected.bytes<(size_t)j->vertices*16 ||
       ps3_geo_overlap(j->positions.data,(size_t)j->vertices*16,
                       j->projected.data,(size_t)j->vertices*16))return 0;
    if(ps3_geo_overlap(j,sizeof(*j),j->projected.data,(size_t)j->vertices*16))return 0;
    return 1;
}
/* Internal entry: only after complete preflight of the enclosing geometry job. */
static void ps3_geometry_project_unchecked(const ps3_geometry_projection_job* j) {
    u128 transform[4];
    u128 last=spu_zero();last._u32[3]=0x3f800000;
    for(unsigned r=0;r<4;r++) {
        u128 t=spu_native_fp_fm(ps3_skin4_splat(j->projection[r],0),j->world_view[0]);
        t=spu_native_fp_fma(ps3_skin4_splat(j->projection[r],1),j->world_view[1],t);
        t=spu_native_fp_fma(ps3_skin4_splat(j->projection[r],2),j->world_view[2],t);
        transform[r]=spu_native_fp_fma(ps3_skin4_splat(j->projection[r],3),last,t);
    }
    const u128 one=spu_ilhu(0x3f80),zero=spu_zero();
    for(unsigned at=0;at<j->vertices;at+=4) {
        u128 x,y,z;
        for(unsigned k=0;k<4;k++) {
            const u128 v=ps3_skin4_load(j->positions.data+(at+k)*16);
            x._u32[k]=v._u32[0];y._u32[k]=v._u32[1];z._u32[k]=v._u32[2];
        }
        u128 clip[4],screen[3],flags=spu_zero();
        for(unsigned r=0;r<4;r++) {
            u128 t=spu_native_fp_fma(x,ps3_skin4_splat(transform[r],0),ps3_skin4_splat(transform[r],3));
            t=spu_native_fp_fma(y,ps3_skin4_splat(transform[r],1),t);
            clip[r]=spu_native_fp_fma(z,ps3_skin4_splat(transform[r],2),t);
        }
        u128 inverse=spu_fi(clip[3],spu_frest(clip[3]));
        const u128 correction=spu_native_fp_fnms(inverse,clip[3],one);
        inverse=spu_native_fp_fma(correction,inverse,inverse);
        const u128 sign=spu_and(clip[3],spu_ilhu(0x8000));
        const u128 positive_w=spu_fcgt(clip[3],zero);
        for(unsigned k=0;k<4;k++)flags._u32[k]=positive_w._u32[k]&2;
        for(unsigned r=0;r<3;r++) {
            const u128 normalized=spu_xor(spu_native_fp_fm(clip[r],inverse),sign);
            screen[r]=spu_native_fp_fma(normalized,ps3_skin4_splat(j->viewport_scale,r),
                                       ps3_skin4_splat(j->viewport_offset,r));
            const u128 hi=spu_fcgt(screen[r],ps3_skin4_splat(j->maximum,r));
            const u128 lo=spu_fcgt(ps3_skin4_splat(j->minimum,r),screen[r]);
            for(unsigned k=0;k<4;k++)flags._u32[k]|=(hi._u32[k]&(128u>>(r*2)))|
                                                                 (lo._u32[k]&(64u>>(r*2)));
        }
        for(unsigned k=0;k<4;k++) {
            u128 out=spu_zero();out._u32[0]=screen[0]._u32[k];out._u32[1]=screen[1]._u32[k];
            out._u32[2]=flags._u32[k];ps3_skin4_store(j->projected.data+(at+k)*16,out);
        }
    }
}
static int ps3_geometry_project(const ps3_geometry_projection_job* j) {
    if(!ps3_geometry_project_valid(j))return 0;
    ps3_geometry_project_unchecked(j);return 1;
}
#endif
#endif
