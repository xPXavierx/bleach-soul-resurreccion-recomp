#include "../native_skinning_result.h"
#include <vector>
#include <thread>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <xmmintrin.h>
static std::atomic<unsigned> calls{0};
struct Buffers {
    unsigned n;
    std::vector<uint8_t> positions,frames,weights,palette;
    explicit Buffers(unsigned count):n(count),positions(n*16),frames(n*48),weights(n*8),palette(256*48){}
    void fresh(unsigned seed){
        for(unsigned i=0;i<positions.size();i++)positions[i]=(i*17+seed)%251;
        for(unsigned i=0;i<weights.size();i++)weights[i]=(i*13+seed)%251;
        for(unsigned i=0;i<palette.size();i++)palette[i]=(i*3+seed)%251;
        memcpy(palette.data(),&seed,4);memset(frames.data(),0xcc,frames.size());
    }
    ps3_skin4_batch batch(){return {palette.data(),weights.data(),positions.data(),frames.data(),n,256,1};}
};
static int calculate(const ps3_skin4_batch* b) {
    ++calls;unsigned sum=0;
    for(unsigned i=0;i<b->vertices*16;i++)sum+=b->positions[i];
    for(unsigned i=0;i<b->vertices*8;i++)sum+=b->influences[i];
    for(unsigned i=0;i<b->palette_entries*48;i++)sum+=b->palette[i];
    for(unsigned i=0;i<b->vertices*16;i++)b->positions[i]^=(uint8_t)sum;
    for(unsigned i=0;i<b->vertices*48;i++)b->frames[i]=(uint8_t)(sum+i);
    // Model fixed, masked arithmetic exception effects, independent of flags.
    _mm_setcsr(_mm_getcsr()|0x22);return 1;
}
static void run(Buffers& b,uint64_t stage=1){auto v=b.batch();assert(ps3_skin4_cached_run(&v,stage,calculate)==1);}
static auto stats(){ps3_skin4_result_counters c;ps3_skin4_result_stats(&c);return c;}
int main() {
    _mm_setcsr(0xffc0);ps3_skin4_result_reset();Buffers a(128),expected(128);
    a.fresh(1);expected.fresh(1);auto eb=expected.batch();calculate(&eb);run(a);
    assert(a.positions==expected.positions && a.frames==expected.frames);
    auto count=calls.load();
    for(unsigned flags=0;flags<64;flags++) {
        a.fresh(1);_mm_setcsr(0xffc0|flags);run(a);assert(calls==count);
        assert(a.positions==expected.positions && a.frames==expected.frames && _mm_getcsr()==(0xffc0|flags|0x22));
    }
    puts("PASS owned outputs and all incoming sticky flag combinations reuse identical results");
    for(unsigned input=0;input<3;input++) {
        a.fresh(1);auto& bytes=input==0?a.positions:(input==1?a.weights:a.palette);
        bytes.back()^=0x40;count=calls;run(a);assert(calls==count+1);
    }
    puts("PASS mutation at the end of every complete input span invalidates reuse");
    a.fresh(1);_mm_setcsr(0xbfc0);count=calls;run(a);assert(calls==count+1);
    _mm_setcsr(0xffc0);a.fresh(1);count=calls;run(a,2);assert(calls==count+1);
    puts("PASS FP controls and stage version have separate identities");
    std::vector<std::thread> workers;
    for(unsigned t=0;t<8;t++)workers.emplace_back([t]{
        _mm_setcsr(0xffc0);Buffers data(128),oracle(128);
        for(unsigned i=0;i<250;i++){unsigned seed=t*16+i%16;data.fresh(seed);oracle.fresh(seed);
            auto ob=oracle.batch();calculate(&ob);run(data);
            assert(data.positions==oracle.positions && data.frames==oracle.frames);}
    });
    for(auto& w:workers)w.join();assert(stats().inflight==0);
    puts("PASS eight concurrent workers preserve private inputs and outputs");
    Buffers large(4096);
    for(unsigned i=0;i<600;i++){large.fresh(i);run(large,100+i);}
    auto c=stats();assert(c.bytes<=64*1024*1024 && c.evicted>0 && c.inflight==0);
    puts("PASS in-flight reservations and resident buffers remain within 64 MiB under eviction");
    ps3_skin4_result_reset();assert(stats().bytes==0);puts("PASS quiescent reset");
}
