#include "rsx_native_program.h"
#include "rsx_test_programs.h"
#include <assert.h>
#include <stdio.h>
static rsx_np_cache fp, vp;
static rsx_fp_constant_block actual, expected;
static u8 bytes[0x10000];
static rsx_np_span span={bytes,sizeof(bytes)};
static rsx_np_record* lookup(u64 address)
{ return rsx_np_fp(&fp,address,rsx_np_span_resolve,&span); }
static void oracle(rsx_np_record* r)
{
    assert(r);
    assert(r->size==rsx_fp_program_size(bytes,span.size));
    rsx_np_constants(r,bytes,&actual);
    assert(rsx_fp_collect_constants(bytes,span.size,&expected)>=0);
    assert(actual.count==expected.count);
    assert(!memcmp(actual.values,expected.values,actual.count*16));
    assert(r->sampled_units==rsx_fp_texture_units(bytes,span.size));
    for(u64 seed=123;seed<134;seed++)
        assert(rsx_np_fp_hash(r,seed)==rsx_fp_structural_hash(bytes,span.size,seed));
}
static void constant_program(void)
{
    memset(bytes,0,sizeof(bytes));
    rsx_test_fp_instr(bytes,RSX_TEST_FP_OPC(1)|RSX_TEST_FP_MASK_XYZW|1,2|RSX_TEST_FP_SWZ_IDENT);
    for(u32 j=0;j<4;j++)rsx_test_fp_put_word(bytes+16+j*4,0x3f800000u);
}
int main(void)
{
    constant_program(); oracle(lookup(0)); u64 builds=fp.builds;
    for(u32 i=0;i<1000;i++) {
        rsx_test_fp_put_word(bytes+16,(i&1)?0x7fc00000:0x3f000000+i);
        oracle(lookup(i%3==0 ? 0 : (u64)i*32));
    }
    assert(fp.builds==builds); puts("PASS: changing inline payloads/addresses share code and match legacy constants/hashes");
    rsx_test_fp_tex_r0_tc0(bytes,7); oracle(lookup(0));
    assert(lookup(0)->size==16 && lookup(0)->sampled_units==(1u<<7));
    constant_program(); rsx_test_fp_put_word(bytes,rsx_fp_read_word(bytes)&~1u);
    rsx_test_fp_tex_r0_tc0(bytes+32,3); oracle(lookup(0));
    assert(lookup(0)->size==48 && lookup(0)->sampled_units==(1u<<3));
    puts("PASS: same-address shorter/longer code and texture metadata invalidate correctly");
    constant_program(); rsx_test_fp_put_word(bytes+8,0x80000000u);
    oracle(lookup(0)); builds=fp.builds;
    rsx_test_fp_tex_r0_tc0(bytes+16,5); oracle(lookup(0));
    assert(fp.builds>builds); puts("PASS: branch programs conservatively validate inline bytes");
    for(u32 i=0;i<RSX_NP_RECORDS+40;i++) {
        rsx_test_fp_instr(bytes,RSX_TEST_FP_OPC(1)|1|(i<<1),RSX_TEST_FP_SRC_INPUT);
        oracle(lookup(100000+(u64)i*32));
    }
    constant_program(); oracle(lookup(0));
    puts("PASS: eviction generations cannot reuse stale address hints");
    span.size=16; assert(!lookup(0));
    span.size=31; assert(!lookup(0));
    span.size=32; oracle(lookup(0));
    memset(bytes,0,sizeof(bytes)); span.size=sizeof(bytes); assert(!lookup(0));
    puts("PASS: unmapped/truncated payloads and missing END are rejected");
    rsx_test_vp_mov_out(bytes,0,RSX_TEST_VP_SWZ_IDENT,0,1);
    rsx_np_record* r=rsx_np_vp(&vp,0,bytes,16); assert(r && r->input_mask==1 && r->size==16);
    rsx_test_vp_txl_out(bytes,8,RSX_TEST_VP_SWZ_IDENT,2,0,1);
    r=rsx_np_vp(&vp,0,bytes,16); assert(r && r->input_mask==(1u<<8) && r->txl_mask==4);
    assert(rsx_np_vp(&vp,0,bytes,15)==NULL);
    puts("PASS: VP input/TXL metadata changes with code and respects bounds");
    rsx_np_destroy(&fp); rsx_np_destroy(&vp);
    assert(!fp.builds && !vp.builds); puts("PASS: cache lifetime resets cleanly"); return 0;
}
