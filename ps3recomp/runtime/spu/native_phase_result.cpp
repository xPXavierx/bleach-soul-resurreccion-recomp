#include "native_phase_result.h"
#include <array>
#include <vector>
#include <mutex>
#include <algorithm>
#include <memory>
#include <chrono>
#include <xmmintrin.h>

extern "C" {SPU_THREAD_LOCAL ps3_phase_capture* g_ps3_phase_capture=nullptr;}
namespace {
constexpr unsigned shard_count=16,ways=16;
constexpr size_t shard_budget=4*1024*1024;
thread_local bool probing=false;
thread_local ps3_phase_result_probe probe{};
struct Span {uint32_t at,size,data;};
struct Packet {
    bool valid=false;uint64_t key=0,stage=0,age=0;
    unsigned fp_control=0,raised_flags=0;
    std::vector<uint8_t> reg_ids,input,output;
    std::vector<u128> reg_inputs;
    std::vector<Span> reads,writes;
    std::array<u128,128> reg_output{};
    uint64_t reg_mask[2]{};
    size_t bytes() const {
        return reg_ids.capacity()+input.capacity()+output.capacity()+reg_inputs.capacity()*sizeof(u128)
            +(reads.capacity()+writes.capacity())*sizeof(Span)+sizeof(*this);
    }
};
struct Shard {
    std::mutex mutex;std::array<Packet,ways> entries;
    ps3_phase_result_counters stats{};uint64_t age=0;
};
struct Cache {std::array<Shard,shard_count> shards;};
// Process-lifetime storage: worker services can still exist during atexit.
Cache& cache(){static Cache* value=new Cache;return *value;}
uint64_t mix(uint64_t h,const void* source,size_t n) {
    const uint8_t* p=static_cast<const uint8_t*>(source);
    while(n>=8){uint64_t v;memcpy(&v,p,8);h^=v*UINT64_C(0xc2b2ae3d27d4eb4f);
        h=(h<<31)|(h>>33);h*=UINT64_C(0x9e3779b185ebca87);p+=8;n-=8;}
    uint64_t tail=0;if(n)memcpy(&tail,p,n);return (h^tail)*UINT64_C(0x9e3779b185ebca87);
}
bool inputs_equal(const Packet& p,const spu_context* ctx,const uint8_t* ids,unsigned count) {
    if(p.reg_ids.size()!=count || memcmp(p.reg_ids.data(),ids,count))return false;
    for(unsigned i=0;i<count;i++)if(memcmp(&p.reg_inputs[i],&ctx->gpr[ids[i]],16))return false;
    for(const auto& s:p.reads)if(memcmp(p.input.data()+s.data,ctx->ls+s.at,s.size))return false;
    return true;
}
void apply(const Packet& p,spu_context* ctx) {
    for(const auto& s:p.writes)memcpy(ctx->ls+s.at,p.output.data()+s.data,s.size);
    for(unsigned w=0;w<2;w++) {
        uint64_t bits=p.reg_mask[w];
        while(bits){unsigned bit=__builtin_ctzll(bits);ctx->gpr[w*64+bit]=p.reg_output[w*64+bit];bits&=bits-1;}
    }
    _mm_setcsr(p.raised_flags);
}
void collect(const uint64_t* mask,const uint8_t* source,std::vector<Span>& spans,std::vector<uint8_t>& bytes) {
    uint32_t first=0,last=0;bool open=false;
    auto close=[&](){if(open){spans.push_back({first,last-first,0});open=false;}};
    for(unsigned w=0;w<256;w++) {
        uint64_t bits=mask[w];
        while(bits){unsigned bit=__builtin_ctzll(bits);uint32_t at=(w*64+bit)*16;
            if(open && at!=last)close();
            if(!open){first=at;open=true;}
            last=at+16;bits&=bits-1;
        }
    }
    close();size_t total=0;for(auto& s:spans){s.data=static_cast<uint32_t>(total);total+=s.size;}
    bytes.resize(total);for(const auto& s:spans)memcpy(bytes.data()+s.data,source+s.at,s.size);
}
}
extern "C" uint64_t ps3_phase_result_clock_ns(void) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" void ps3_phase_result_probe_begin(void) {probe={};probing=true;}
extern "C" void ps3_phase_result_probe_end(ps3_phase_result_probe* out) {*out=probe;probing=false;}
extern "C" void ps3_phase_result_run(spu_context* ctx,uint64_t stage,const uint8_t* ids,unsigned count,
                                     void(*compute)(spu_context*),uint64_t hint) {
    if(ctx->ls!=ctx->ls_store || count>128 || g_ps3_phase_capture) {compute(ctx);return;}
    for(unsigned i=0;i<count;i++)if(ids[i]>=128){compute(ctx);return;}
    const bool sample=probing;
    const uint64_t started=sample?ps3_phase_result_clock_ns():0;
    uint64_t checkpoint=started;
    // Sticky flags are included too: a hit reproduces the full FP environment
    // from an identical incoming environment, without guessing raised flags.
    const unsigned control=_mm_getcsr();
    uint64_t key=mix(stage^hint,&control,sizeof(control));key=mix(key,ids,count);
    for(unsigned i=0;i<count;i++)key=mix(key,&ctx->gpr[ids[i]],16);
    key^=key>>33;key*=UINT64_C(0xff51afd7ed558ccd);key^=key>>33;
    Shard& shard=cache().shards[key&(shard_count-1)];
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for(auto& p:shard.entries)if(p.valid && p.key==key && p.stage==stage &&
            p.fp_control==control && inputs_equal(p,ctx,ids,count)) {
            p.age=++shard.age;++shard.stats.hits;
            if(sample){checkpoint=ps3_phase_result_clock_ns();probe.lookup_ns=checkpoint-started;}
            apply(p,ctx);
            if(sample){const auto done=ps3_phase_result_clock_ns();probe.apply_ns=done-checkpoint;
                probe.total_ns=done-started;probe.input_bytes=p.input.size();probe.output_bytes=p.output.size();
                probe.hit=1;probe.valid=1;}
            return;
        }
        ++shard.stats.misses;
    }
    if(sample){checkpoint=ps3_phase_result_clock_ns();probe.lookup_ns=checkpoint-started;}
    // No cache lock is held during calculation. The adapter must exclude
    // nonlocal exits: this API requires compute to return normally.
    static thread_local std::unique_ptr<ps3_phase_capture> owned;
    if(!owned)owned.reset(new ps3_phase_capture);
    auto& c=*owned;memset(c.reads,0,sizeof(c.reads));memset(c.writes,0,sizeof(c.writes));
    c.register_writes[0]=c.register_writes[1]=0;c.aborted=0;
    Packet result;result.stage=stage;result.key=key;result.fp_control=control;
    result.reg_ids.assign(ids,ids+count);result.reg_inputs.reserve(count);
    for(unsigned i=0;i<count;i++)result.reg_inputs.push_back(ctx->gpr[ids[i]]);
    g_ps3_phase_capture=&c;
    if(sample){auto now=ps3_phase_result_clock_ns();probe.setup_ns=now-checkpoint;checkpoint=now;}
    compute(ctx);g_ps3_phase_capture=nullptr;
    if(sample){auto now=ps3_phase_result_clock_ns();probe.compute_ns=now-checkpoint;checkpoint=now;}
    if(c.aborted){std::lock_guard<std::mutex> lock(shard.mutex);++shard.stats.declined;return;}
    collect(c.reads,c.input,result.reads,result.input);collect(c.writes,ctx->ls,result.writes,result.output);
    memcpy(result.reg_output.data(),ctx->gpr,sizeof(ctx->gpr));
    memcpy(result.reg_mask,c.register_writes,sizeof(result.reg_mask));
    result.raised_flags=_mm_getcsr();result.valid=true;
    const size_t needed=result.bytes();
    if(sample){auto now=ps3_phase_result_clock_ns();probe.collect_ns=now-checkpoint;checkpoint=now;
        probe.input_bytes=result.input.size();probe.output_bytes=result.output.size();}
    std::lock_guard<std::mutex> lock(shard.mutex);
    if(needed>shard_budget){++shard.stats.declined;return;}
    auto oldest=[&](){return std::min_element(shard.entries.begin(),shard.entries.end(),
        [](const Packet& a,const Packet& b){return (a.valid?a.age:0)<(b.valid?b.age:0);});};
    auto evict=[&](Packet& p){if(p.valid){shard.stats.bytes-=p.bytes();++shard.stats.evicted;}p=Packet{};};
    // Byte budget includes vector capacities, not just the populated payload.
    while(shard.stats.bytes+needed>shard_budget) {
        auto p=std::min_element(shard.entries.begin(),shard.entries.end(),[](const Packet& a,const Packet& b){
            return (a.valid?a.age:UINT64_MAX)<(b.valid?b.age:UINT64_MAX);});
        if(p==shard.entries.end() || !p->valid){++shard.stats.declined;return;}
        evict(*p);
    }
    auto slot=oldest();evict(*slot);result.age=++shard.age;*slot=std::move(result);
    shard.stats.bytes+=needed;++shard.stats.stored;
    if(sample){auto done=ps3_phase_result_clock_ns();probe.publish_ns=done-checkpoint;
        probe.total_ns=done-started;probe.valid=1;}
}
extern "C" void ps3_phase_result_stats(ps3_phase_result_counters* out) {
    *out={};for(auto& s:cache().shards){std::lock_guard<std::mutex> lock(s.mutex);
        out->hits+=s.stats.hits;out->misses+=s.stats.misses;out->stored+=s.stats.stored;
        out->declined+=s.stats.declined;out->evicted+=s.stats.evicted;out->bytes+=s.stats.bytes;}
}
extern "C" void ps3_phase_result_reset(void) {
    for(auto& s:cache().shards){std::lock_guard<std::mutex> lock(s.mutex);
        for(auto& p:s.entries)p=Packet{};s.stats={};s.age=0;}
}
