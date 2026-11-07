#include "spu_native_pairs_x86.h"
#include <stdio.h>
static uint32_t rng=1234567;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static uint64_t checked;
static void check(u128 a[2],u128 b[2],u128 c[2]) {
    spu_native_fp_pair results[]={
        spu_native_pair_fa(a[0],b[0],a[1],b[1]),spu_native_pair_fs(a[0],b[0],a[1],b[1]),
        spu_native_pair_fm(a[0],b[0],a[1],b[1]),spu_native_pair_fma(a[0],b[0],c[0],a[1],b[1],c[1]),
        spu_native_pair_fms(a[0],b[0],c[0],a[1],b[1],c[1]),spu_native_pair_fnms(a[0],b[0],c[0],a[1],b[1],c[1])};
    for(unsigned h=0;h<2;h++)for(unsigned i=0;i<4;i++) {
        uint32_t x=a[h]._u32[i],y=b[h]._u32[i],z=c[h]._u32[i];
        uint32_t refs[]={spu__fa_lane(x,y),spu__fa_lane(x,y^0x80000000),spu__fm_lane(x,y),
            spu__fma_lane(x,y,z,0,0),spu__fma_lane(x,y,z,0,0x80000000),spu__fma_lane(x,y,z,0x80000000,0)};
        for(unsigned op=0;op<6;op++) {
            uint32_t got=(h?results[op].hi:results[op].lo)._u32[i];
            if(got!=refs[op]) {fprintf(stderr,"FAIL op=%u half=%u lane=%u %08x,%08x,%08x got=%08x expected=%08x\n",op,h,i,x,y,z,got,refs[op]);abort();}
            checked++;
        }
    }
}
int main(void) {
    const unsigned saved=_mm_getcsr();
    _mm_setcsr((saved&~_MM_ROUND_MASK)|_MM_ROUND_TOWARD_ZERO|_MM_MASK_MASK|_MM_FLUSH_ZERO_ON|0x40);
    u128 a[2],b[2],c[2];
    const uint32_t edge[]={0,1,0x7fffff,0x800000,0x800001,0x3f000000,0x3f7fffff,
                          0x3f800000,0x3f800001,0x40000000,0x7f7ffffe,0x7f7fffff,0x7f800000,0x7fffffff};
    for(unsigned x=0;x<14;x++)for(unsigned y=0;y<14;y++)for(unsigned z=0;z<14;z++)for(unsigned signs=0;signs<8;signs++) {
        for(unsigned h=0;h<2;h++)for(unsigned i=0;i<4;i++) {
            a[h]._u32[i]=edge[(x+h*i)%14]|((signs&1)?0x80000000:0);
            b[h]._u32[i]=edge[(y+h*i)%14]|((signs&2)?0x80000000:0);
            c[h]._u32[i]=edge[(z+h*i)%14]|((signs&4)?0x80000000:0);
        } check(a,b,c);
    }
    for(unsigned n=0;n<2000000;n++) {
        for(unsigned h=0;h<2;h++)for(unsigned i=0;i<4;i++) {
            a[h]._u32[i]=next();b[h]._u32[i]=next();c[h]._u32[i]=next();
        } check(a,b,c);
        if(n%4==0) {
            for(unsigned h=0;h<2;h++) {c[h]=a[h];for(unsigned i=0;i<4;i++)b[h]._u32[i]=0xbf800000;}
            check(a,b,c);
        }
    }
    _mm_setcsr(saved);
    printf("PASS: %llu exact eight-lane operation comparisons against integer reference\n",(unsigned long long)checked);
}
