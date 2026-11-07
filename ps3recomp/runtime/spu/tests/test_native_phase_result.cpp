#include "native_phase_result.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <xmmintrin.h>
static std::atomic<unsigned> executions{0};
static const uint8_t inputs[]={3,4,6};
static void put(spu_context* c,unsigned at,u128 value){ps3_phase_result_store(c->ls,at,value);}
static u128 get(spu_context* c,unsigned at){return ps3_phase_result_load(c->ls,at);}
static void fresh(spu_context* c,unsigned seed=1) {
    memset(c,0,sizeof(*c));c->ls=c->ls_store;c->gpr[3]._u32[0]=0x100;c->gpr[4]._u32[0]=0x300;
    c->gpr[6]._u32[1]=123;c->gpr[7]._u32[0]=456;
    u128 v{};v._u32[0]=seed;v._u32[1]=seed+11;v._u32[2]=seed+19;put(c,0x100,v);
    v._u32[0]=31;put(c,0x120,v);v._u32[0]=53;put(c,0x140,v);
}
static void calculation(spu_context* c) {
    ++executions;u128 x=get(c,c->gpr[3]._u32[0]);
    if(x._u32[0]==0xffff)ps3_phase_result_abort();
    u128 v=get(c,(x._u32[0]&1)?0x120:0x140);
    v._u32[0]+=x._u32[1];put(c,0x200,v);
    u128 scratch=get(c,0x200);put(c,c->gpr[4]._u32[0],scratch);
    x._u32[2]^=7;put(c,0x100,x); // in/out dependency uses the pre-write bytes
    ps3_phase_result_mark_gprs(UINT64_C(1)<<5,0);c->gpr[5]=scratch;
    if(x._u32[0]&1) {
        ps3_phase_result_mark_gprs(UINT64_C(1)<<6,0);c->gpr[6]._u32[0]=scratch._u32[0];
    } else {
        ps3_phase_result_mark_gprs(UINT64_C(1)<<7,0);c->gpr[7]=scratch;
    }
}
static void run(spu_context* c,uint64_t stage=42) {
    ps3_phase_result_run(c,stage,inputs,sizeof(inputs),calculation,0);
}
static void check(spu_context* c,unsigned seed) {
    unsigned expected=(seed&1?31:53)+seed+11;
    assert(get(c,0x300)._u32[0]==expected && c->gpr[5]._u32[0]==expected);
    assert(get(c,0x100)._u32[2]==((seed+19)^7));
}
static ps3_phase_result_counters stats(){ps3_phase_result_counters c;ps3_phase_result_stats(&c);return c;}
int main() {
    const unsigned fp=_mm_getcsr();_mm_setcsr(0x1f80);
    auto c=std::make_unique<spu_context>();ps3_phase_result_reset();
    fresh(c.get());run(c.get());check(c.get(),1);unsigned calls=executions.load();
    fresh(c.get());c->gpr[7]._u32[0]=999;c->gpr[80]._u32[0]=0xdeadbeef;
    memset(c->ls+0x200,0xaa,16);memset(c->ls+0x800,0xcc,16);run(c.get());
    assert(executions==calls);check(c.get(),1);
    assert(c->gpr[7]._u32[0]==999 && c->gpr[80]._u32[0]==0xdeadbeef && c->ls[0x800]==0xcc);
    puts("PASS hit owns only actual writes; unrelated state and write-before-read bytes preserved");
    for(unsigned byte=0;byte<16;byte++) {
        fresh(c.get());c->ls[0x100+byte]^=0x10;calls=executions;run(c.get());assert(executions==calls+1);
    }
    puts("PASS every input byte invalidates despite an unchanged lookup hint");
    fresh(c.get(),2);run(c.get());check(c.get(),2);
    fresh(c.get(),2);c->ls[0x120]^=0x4;calls=executions;run(c.get());assert(executions==calls);
    fresh(c.get(),2);c->ls[0x140]^=0x4;calls=executions;run(c.get());assert(executions==calls+1);
    puts("PASS branch-dependent read footprints distinguish used and unused inputs");
    fresh(c.get());c->gpr[6]._u32[1]=321;calls=executions;run(c.get());assert(executions==calls+1);
    assert(c->gpr[6]._u32[1]==321);
    puts("PASS partial-register output retains keyed incoming lanes");
    fresh(c.get());_mm_setcsr(0x3f80);calls=executions;run(c.get());assert(executions==calls+1);
    _mm_setcsr(0x1f80);fresh(c.get());run(c.get());calls=executions;
    _mm_setcsr(0x1fa0);fresh(c.get());run(c.get());assert(executions==calls+1 && _mm_getcsr()==0x1fa0);
    _mm_setcsr(0x1f80);puts("PASS floating-point control and sticky flags are explicit dependencies");
    fresh(c.get(),0xffff);run(c.get());calls=executions;
    fresh(c.get(),0xffff);run(c.get());assert(executions==calls+1 && stats().declined>=2);
    puts("PASS aborted captures execute and never publish reusable results");
    std::vector<std::thread> threads;
    for(unsigned t=0;t<8;t++)threads.emplace_back([t](){
        _mm_setcsr(0x1f80);auto local=std::make_unique<spu_context>();
        for(unsigned i=0;i<300;i++){unsigned seed=t*32+i%8;fresh(local.get(),seed);run(local.get());check(local.get(),seed);}
    });
    for(auto& t:threads)t.join();puts("PASS eight workers with private inputs and shared owned packets");
    for(unsigned i=0;i<1000;i++){fresh(c.get(),i%17);run(c.get(),1000+i);check(c.get(),i%17);}
    assert(stats().evicted>0);puts("PASS eviction preserves computation and output ownership");
    ps3_phase_result_reset();
    const uint8_t large_inputs[]={3};
    auto large=[](spu_context* state){for(unsigned at=0;at<SPU_LS_SIZE;at+=16)
        ps3_phase_result_store(state->ls,at,state->gpr[3]);};
    for(unsigned i=0;i<600;i++){fresh(c.get());c->gpr[3]._u32[0]=i;
        ps3_phase_result_run(c.get(),999,large_inputs,1,large,0);}
    auto totals=stats();assert(totals.bytes<=64*1024*1024 && totals.evicted>0);
    puts("PASS allocated packet capacities respect the 64 MiB budget");
    ps3_phase_result_reset();assert(stats().bytes==0);
    ps3_phase_result_probe miss{},hit{};
    fresh(c.get());ps3_phase_result_probe_begin();run(c.get());ps3_phase_result_probe_end(&miss);
    fresh(c.get());ps3_phase_result_probe_begin();run(c.get());ps3_phase_result_probe_end(&hit);
    assert(miss.valid && !miss.hit && hit.valid && hit.hit);
    assert(miss.total_ns==miss.lookup_ns+miss.setup_ns+miss.compute_ns+miss.collect_ns+miss.publish_ns);
    assert(hit.total_ns==hit.lookup_ns+hit.apply_ns && hit.compute_ns==0);
    assert(hit.input_bytes==miss.input_bytes && hit.output_bytes==miss.output_bytes);
    assert(hit.input_bytes==32 && hit.output_bytes==48);check(c.get(),1);
    puts("PASS scoped probes account for hit/miss service time and exact payload sizes");
    ps3_phase_result_reset();_mm_setcsr(fp);
    puts("PASS quiescent reset");return 0;
}
