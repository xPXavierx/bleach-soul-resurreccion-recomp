"""Dependency/barrier checks and whole-state differential execution of packing."""
import argparse,subprocess,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from spu_native_pairs import pack_body,pack_source

PURE={'private_load','spu_ori','spu_xor'}
def pair_count(body):return len(pack_body(body,PURE)[1])
first='    r3 = spu_fm(r0, r1);\n'
last='    r4 = spu_fm(r2, r5);\n'
assert pair_count(first+last)==1
assert pair_count(first+'    r8 = private_load(ls, r14._u32[0]);\n'+last)==1
for barrier in ('r0 = spu_ori(r8, 0);','r3 = spu_ori(r8, 0);','r8 = spu_xor(r3, r2);',
                'private_store(ls, 0, r8);','unknown(ctx);','label:',
                'if (r8._u32[0]) goto label;','return;',
                'r8 = private_load(ls, r3._u32[0]);','r8 = *ls;',
                'r8 = volatile_read();','r8 = private_load(ls, ctx->pc);'):
    assert pair_count(first+'    '+barrier+'\n'+last)==0,barrier
assert pair_count(first+'    r4 = spu_fm(r3, r5);\n')==0
assert pair_count(first+'    r3 = spu_fm(r2, r5);\n')==0
assert pair_count('    r0 = spu_fm(r1, r2);\n    r1 = spu_fm(r4, r5);\n')==1

body='\n'.join(f'    u128 r{i}=ctx->gpr[{i}];' for i in range(16))+'\n    uint8_t* ls=ctx->ls;\n'
body+=first+'    r8 = private_load(ls, r14._u32[0]);\n'+last
body+='''    r6 = spu_fma(r3, r8, r2);
    r7 = spu_fma(r4, r5, r1);
    r9 = spu_fm(r6, r2);
    r6 = spu_ori(r8, 0);
    r10 = spu_fm(r6, r8);
    r3 = spu_fa(r9, r10);
    observe(ctx, r3);
    r4 = spu_fa(r10, r9);
    r5 = spu_fs(r7, r2);
    private_store(ls, 0, r5);
    r8 = spu_fs(r3, r6);
    r0 = spu_fm(r1, r2);
    r1 = spu_fm(r4, r5);
    r15._u32[0] = 3;
loop:
    r6 = spu_fms(r6, r8, r3);
    r7 = spu_fms(r7, r9, r4);
    r15._u32[0] = r15._u32[0] - 1;
    if (r15._u32[0]) goto loop;
    r8 = spu_fnms(r6, r2, r3);
    r9 = spu_fnms(r7, r1, r4);
'''
body+='\n'.join(f'    ctx->gpr[{i}]=r{i};' for i in range(16))+'\n'
fixture='static void fixture_native(spu_context* ctx) {\n'+body+'}\n'
packed,report=pack_source(fixture,PURE)
assert len(report)==1 and len(report[0]['pairs'])==5,report
assert pack_source(fixture.replace('fixture_native','fixture_reference'),PURE)[1]==[]

PRELUDE=r'''
#include "spu_native_pairs_x86.h"
#include <assert.h>
#include <stdio.h>
#define spu_fa spu_native_fp_fa
#define spu_fs spu_native_fp_fs
#define spu_fm spu_native_fp_fm
#define spu_fma spu_native_fp_fma
#define spu_fms spu_native_fp_fms
#define spu_fnms spu_native_fp_fnms
static uint32_t trace;
static u128 private_load(uint8_t*ls,uint32_t address){u128 r;memcpy(&r,ls+(address&240),16);return r;}
static void private_store(uint8_t*ls,uint32_t address,u128 v){memcpy(ls+(address&240),&v,16);}
static void observe(spu_context*ctx,u128 v){trace=trace*31+v._u32[0];ctx->ls[255]^=(uint8_t)trace;}
static spu_context original,paired;
static uint32_t rng=918273;
static uint32_t next(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
'''
MAIN=r'''
int main(void){
    unsigned saved=_mm_getcsr();
    _mm_setcsr((saved&~_MM_ROUND_MASK)|_MM_ROUND_TOWARD_ZERO|_MM_MASK_MASK|_MM_FLUSH_ZERO_ON|0x40);
    for(unsigned n=0;n<32768;n++) {
        for(unsigned i=0;i<128;i++)for(unsigned lane=0;lane<4;lane++)original.gpr[i]._u32[lane]=next();
        for(unsigned i=0;i<256;i++)original.ls_store[i]=(uint8_t)next();
        paired=original;original.ls=original.ls_store;paired.ls=paired.ls_store;
        trace=0;fixture_reference(&original);uint32_t a=trace;
        trace=0;fixture_native(&paired);assert(a==trace);
        assert(!memcmp(original.gpr,paired.gpr,sizeof(original.gpr)));
        assert(!memcmp(original.ls,paired.ls,SPU_LS_SIZE));
    }
    _mm_setcsr(saved);
    puts("PASS: 32768 complete register/local-store comparisons, same I/O transcript; loops and alias dependencies preserved");
}
'''
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--compiler',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();sdk=Path(__file__).resolve().parents[2]
    source=args.output.with_suffix('.c')
    source.write_text(PRELUDE+fixture.replace('fixture_native','fixture_reference')+packed+MAIN)
    subprocess.run([str(args.compiler),'-std=gnu17','-O2','-mavx2','-mfma','-ffp-contract=off','-frounding-math',
       '-I'+str(sdk/'include'),'-I'+str(sdk/'runtime/spu'),str(source),'-o',str(args.output)],check=True)
    subprocess.run([str(args.output.resolve())],check=True)
    print('PASS: dependency and architectural-barrier rejection cases')
