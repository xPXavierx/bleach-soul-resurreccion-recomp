#include "spu_native_float_x86.h"
#include <stdio.h>
#include <assert.h>
static uint32_t rng=1234567;
static uint32_t next(void) {rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng;}
static uint64_t checked;
static void check(u128 a,u128 b,u128 c)
{
    u128 results[]={spu_native_fp_fa(a,b),spu_native_fp_fs(a,b),spu_native_fp_fm(a,b),
        spu_native_fp_fma(a,b,c),spu_native_fp_fms(a,b,c),spu_native_fp_fnms(a,b,c)};
    for(unsigned i=0;i<4;i++) {
        uint32_t x=a._u32[i],y=b._u32[i],z=c._u32[i];
        uint32_t refs[]={spu__fa_lane(x,y),spu__fa_lane(x,y^0x80000000),spu__fm_lane(x,y),
            spu__fma_lane(x,y,z,0,0),spu__fma_lane(x,y,z,0,0x80000000),spu__fma_lane(x,y,z,0x80000000,0)};
        for(unsigned op=0;op<6;op++) {
            if(results[op]._u32[i]!=refs[op]) {
                fprintf(stderr,"FAIL op=%u x=%08x y=%08x z=%08x got=%08x expected=%08x\n",
                    op,x,y,z,results[op]._u32[i],refs[op]); abort();
            }
            checked++;
        }
    }
}
int main(void)
{
    unsigned saved=_mm_getcsr();
    _mm_setcsr((saved & ~_MM_ROUND_MASK)|_MM_ROUND_TOWARD_ZERO|_MM_FLUSH_ZERO_ON|0x40);
    u128 a,b,c;
    const uint32_t edge[]={0,1,0x007fffff,0x00800000,0x00800001,0x3f000000,
        0x3f7fffff,0x3f800000,0x3f800001,0x40000000,0x7f7ffffe,0x7f7fffff,0x7f800000,0x7fffffff};
    for(unsigned x=0;x<14;x++)for(unsigned y=0;y<14;y++)for(unsigned z=0;z<14;z++)
      for(unsigned signs=0;signs<8;signs++) {
        for(unsigned i=0;i<4;i++) {
            a._u32[i]=edge[x] | ((signs&1)?0x80000000:0);
            b._u32[i]=edge[y] | ((signs&2)?0x80000000:0);
            c._u32[i]=edge[z] | ((signs&4)?0x80000000:0);
        } check(a,b,c);
      }
    for(unsigned n=0;n<2000000;n++) {
        for(unsigned i=0;i<4;i++) {
            a._u32[i]=next(); b._u32[i]=next(); c._u32[i]=next();
        }
        check(a,b,c);
        if(n%4==0) { c=a; for(unsigned i=0;i<4;i++)b._u32[i]=0xbf800000; check(a,b,c); }
    }
    _mm_setcsr(saved);
    printf("PASS: %llu exact operation lanes, RTZ native arithmetic vs integer reference\n",(unsigned long long)checked);
}
