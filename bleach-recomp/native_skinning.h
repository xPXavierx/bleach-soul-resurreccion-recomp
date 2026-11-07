#ifndef BLEACH_NATIVE_SKINNING_H
#define BLEACH_NATIVE_SKINNING_H
#include "spu_context.h"
#include "spu_skinning_cyclic4.h"
extern int bleach_native_skinning_mode;
extern unsigned bleach_native_skinning_stride;
extern void bleach_native_skinning_record(unsigned,int);
extern void bleach_native_skinning_verified(void);
static SPU_THREAD_LOCAL int bleach_skin4_fast;
#if defined(__FMA__)
static int bleach_skin4_vec(const spu_context* c,unsigned r,uint32_t a,uint32_t b,uint32_t d,uint32_t e) {
    const uint32_t* v=c->gpr[r]._u32;return v[0]==a && v[1]==b && v[2]==d && v[3]==e;
}
static int bleach_skin4_range(uint32_t at,uint32_t bytes) {return at<=SPU_LS_SIZE && bytes<=SPU_LS_SIZE-at;}
static int bleach_skin4_disjoint(uint32_t a,uint32_t as,uint32_t b,uint32_t bs) {return a+as<=b || b+bs<=a;}
/* Recreate the software pipeline's current pair. The final original pair
 * reconstructs every observable temporary, including prefetched next inputs. */
static void bleach_skin4_prime(spu_context* c) {
    u128 weights=bleach_kernel_load(c->ls,c->gpr[51]._u32[0]-16);
    u128 indices=spu_andhi(weights,255);
    c->gpr[6]=bleach_int_a(bleach_int_shli(indices,4),bleach_int_shli(indices,5));
    c->gpr[11]=bleach_int_shlqbyi(c->gpr[6],4);c->gpr[7]=bleach_int_shlqbyi(c->gpr[6],8);
    c->gpr[8]=bleach_int_shlqbyi(c->gpr[6],12);
    c->gpr[4]=bleach_int_roti(c->gpr[6],16);c->gpr[12]=bleach_int_roti(c->gpr[11],16);
    c->gpr[3]=bleach_int_roti(c->gpr[7],16);c->gpr[9]=bleach_int_roti(c->gpr[8],16);
    u128 a=bleach_int_shufb(weights,c->gpr[81],c->gpr[77]);
    u128 b=bleach_int_shufb(weights,c->gpr[81],c->gpr[102]);
    a=spu_native_fp_fm(bleach_spu_cuflt_avx2(a,155),c->gpr[91]);
    b=spu_native_fp_fm(bleach_spu_cuflt_avx2(b,155),c->gpr[91]);
    const unsigned masks[4]={81,95,94,93},first[4]={41,42,43,45},second[4]={33,40,38,44};
    for(unsigned i=0;i<4;i++){c->gpr[first[i]]=bleach_int_shufb(a,c->gpr[81],c->gpr[masks[i]]);
        c->gpr[second[i]]=bleach_int_shufb(b,c->gpr[81],c->gpr[masks[i]]);}
    c->gpr[46]=bleach_kernel_load(c->ls,c->gpr[86]._u32[0]);
    c->gpr[47]=bleach_kernel_load(c->ls,c->gpr[86]._u32[0]+16);
    c->gpr[54]=bleach_int_shufb(c->gpr[46],c->gpr[81],c->gpr[36]);
    c->gpr[55]=bleach_int_shufb(c->gpr[47],c->gpr[81],c->gpr[36]);
    c->gpr[53]=bleach_int_shufb(c->gpr[46],c->gpr[81],c->gpr[82]);
    c->gpr[52]=bleach_int_shufb(c->gpr[47],c->gpr[81],c->gpr[82]);
}
static unsigned bleach_skin4_bulk(spu_context* c) {
    bleach_skin4_fast=0;
    if(!bleach_native_skinning_mode || c->ls!=c->ls_store || g_ps3_phase_capture ||
       !bleach_kernel_regions_enabled(c) || (_mm_getcsr()&0xffc0)!=0xffc0)return 0;
    const unsigned n=c->gpr[48]._u32[0],pos=c->gpr[86]._u32[0],frame=c->gpr[49]._u32[0],
        palette=c->gpr[37]._u32[0],weights=c->gpr[51]._u32[0]-16;
    if(n<4 || n>4096 || (n&1) || ((pos|frame|palette|weights)&15) ||
       !bleach_skin4_range(pos,n*16+32) || !bleach_skin4_range(frame,n*48) ||
       !bleach_skin4_range(weights,n*8+16) || c->gpr[50]._u32[0]!=pos+32 ||
       c->gpr[35]._u32[0]!=palette+16 || c->gpr[34]._u32[0]!=palette+32)return 0;
    if(!bleach_skin4_vec(c,36,0x04050607,0x08090a0b,0x00010203,0x0c0d0e0f) ||
       !bleach_skin4_vec(c,82,0x08090a0b,0x00010203,0x04050607,0x0c0d0e0f) ||
       !bleach_skin4_vec(c,77,0x80808000,0x80808002,0x80808004,0x80808006) ||
       !bleach_skin4_vec(c,102,0x80808008,0x8080800a,0x8080800c,0x8080800e) ||
       !bleach_skin4_vec(c,69,0x1c1d1e1f,0x04050607,0x08090a0b,0x80808080) ||
       !bleach_skin4_vec(c,70,0x80808080,0x0c0d0e0f,0x1c1d1e1f,0x80808080) ||
       !bleach_skin4_vec(c,84,~0u,~0u,~0u,0) ||
       !bleach_skin4_vec(c,91,0x3b808080,0x3b808080,0x3b808080,0x3b808080) ||
       !bleach_skin4_vec(c,97,0x3f800000,0x3f800000,0x3f800000,0x3f800000) ||
       !bleach_skin4_vec(c,81,0x00010203,0x00010203,0x00010203,0x00010203) ||
       !bleach_skin4_vec(c,95,0x04050607,0x04050607,0x04050607,0x04050607) ||
       !bleach_skin4_vec(c,94,0x08090a0b,0x08090a0b,0x08090a0b,0x08090a0b) ||
       !bleach_skin4_vec(c,93,0x0c0d0e0f,0x0c0d0e0f,0x0c0d0e0f,0x0c0d0e0f))return 0;
    unsigned maximum=0;for(unsigned i=0;i<n*8;i+=2)if(c->ls[weights+i+1]>maximum)maximum=c->ls[weights+i+1];
    const unsigned palette_bytes=(maximum+1)*48;
    if(!bleach_skin4_range(palette,palette_bytes) ||
       !bleach_skin4_disjoint(pos,n*16+32,frame,n*48) ||
       !bleach_skin4_disjoint(pos,n*16+32,weights,n*8+16) ||
       !bleach_skin4_disjoint(pos,n*16+32,palette,palette_bytes) ||
       !bleach_skin4_disjoint(frame,n*48,weights,n*8+16) ||
       !bleach_skin4_disjoint(frame,n*48,palette,palette_bytes))return 0;
    const unsigned bulk=n-2;
    const ps3_skin4_batch batch={c->ls+palette,c->ls+weights,c->ls+pos,c->ls+frame,bulk,maximum+1,bleach_native_skinning_mode>=2};
    bleach_skin4_fast=bleach_native_skinning_mode==3?
        ps3_skin4_cached_run(&batch,UINT64_C(0x534b494e34000001),ps3_skin4_run):ps3_skin4_run(&batch);
    c->gpr[48]=bleach_int_ai(c->gpr[48],-(int)bulk);
    c->gpr[49]=bleach_int_ai(c->gpr[49],bulk*48);
    c->gpr[50]=bleach_int_ai(c->gpr[50],bulk*16);
    c->gpr[51]=bleach_int_ai(c->gpr[51],bulk*8);
    c->gpr[86]=bleach_int_ai(c->gpr[86],bulk*16);
    bleach_skin4_prime(c);return bulk;
}
#else
static unsigned bleach_skin4_bulk(spu_context* c) {(void)c;return 0;}
#endif
#endif
