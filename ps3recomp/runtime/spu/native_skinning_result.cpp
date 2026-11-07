#include "native_skinning_result.h"
#include <array>
#include <memory>
#include <mutex>
#include <cstring>
#include <new>
#include <cassert>
#include <xmmintrin.h>
namespace {
constexpr unsigned shard_count=32,ways=64;
constexpr size_t shard_budget=2*1024*1024;
struct Entry {
    bool valid=false,busy=false;
    uint64_t key=0,stage=0,age=0;
    unsigned vertices=0,palette_entries=0,allow_bounded=0,control=0,raised=0;
    int bounded=0;
    size_t capacity=0;
    std::unique_ptr<uint8_t[]> storage;
};
struct Shard {
    std::mutex mutex;std::array<Entry,ways> entries;
    uint64_t age=0;ps3_skin4_result_counters stats{};
};
struct Cache {std::array<Shard,shard_count> shards;};
Cache& cache(){static Cache* c=new Cache;return *c;}
uint64_t hash(uint64_t h,const void* source,size_t count) {
    const auto* p=static_cast<const uint8_t*>(source);
    uint64_t a=h,b=h^UINT64_C(0x9e3779b185ebca87),c=~h,d=h+UINT64_C(0xc2b2ae3d27d4eb4f);
    constexpr uint64_t prime=UINT64_C(0x9e3779b185ebca87);
    while(count>=32){uint64_t x[4];memcpy(x,p,32);a=(a^x[0])*prime;b=(b^x[1])*prime;
        c=(c^x[2])*prime;d=(d^x[3])*prime;p+=32;count-=32;}
    h=a^((b<<13)|(b>>51))^((c<<29)|(c>>35))^((d<<47)|(d>>17));
    while(count>=8){uint64_t x;memcpy(&x,p,8);h=(h^x)*prime;p+=8;count-=8;}
    uint64_t tail=0;if(count)memcpy(&tail,p,count);return (h^tail)*prime;
}
}
extern "C" int ps3_skin4_cached_run(const ps3_skin4_batch* batch,uint64_t stage,int(*compute)(const ps3_skin4_batch*)) {
    const unsigned incoming=_mm_getcsr(),control=incoming&~63u;
    if(!batch->vertices || batch->vertices>4096 || !batch->palette_entries ||
       batch->palette_entries>256 || (control&0x1f80)!=0x1f80)return compute(batch);
    const size_t pbytes=batch->vertices*16,wbytes=batch->vertices*8,mbytes=batch->palette_entries*48;
    const size_t input_bytes=pbytes+wbytes+mbytes,output_bytes=pbytes+batch->vertices*48;
    const size_t desired=(input_bytes+output_bytes+4095)&~size_t(4095);
    const unsigned layout[]={batch->vertices,batch->palette_entries,batch->allow_bounded,control};
    uint64_t key=hash(stage,layout,sizeof(layout));
    key=hash(key,batch->positions,pbytes);key=hash(key,batch->influences,wbytes);key=hash(key,batch->palette,mbytes);
    key^=key>>33;key*=UINT64_C(0xff51afd7ed558ccd);key^=key>>33;
    Shard& shard=cache().shards[key&(shard_count-1)];Entry* chosen=nullptr;
    auto publish=[&](const Entry& e) {
        const uint8_t* output=e.storage.get()+input_bytes;
        memcpy(batch->positions,output,pbytes);memcpy(batch->frames,output+pbytes,batch->vertices*48);
        _mm_setcsr(incoming|e.raised);
    };
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for(auto& e:shard.entries)if(e.valid && !e.busy && e.key==key && e.stage==stage &&
            e.vertices==batch->vertices && e.palette_entries==batch->palette_entries &&
            e.allow_bounded==batch->allow_bounded && e.control==control &&
            !memcmp(e.storage.get(),batch->positions,pbytes) &&
            !memcmp(e.storage.get()+pbytes,batch->influences,wbytes) &&
            !memcmp(e.storage.get()+pbytes+wbytes,batch->palette,mbytes)) {
                ++shard.stats.hits;shard.stats.hit_vertices+=batch->vertices;e.age=++shard.age;
                publish(e);return e.bounded;
            }
        ++shard.stats.misses;shard.stats.miss_vertices+=batch->vertices;
        for(auto& e:shard.entries)if(!e.busy && (!chosen ||
            (e.valid?e.age:0)<(chosen->valid?chosen->age:0)))chosen=&e;
        auto evict=[&](Entry& e,bool release){
            if(e.valid){e.valid=false;++shard.stats.evicted;}
            if(release){shard.stats.bytes-=e.capacity;e.capacity=0;e.storage.reset();}
        };
        if(chosen && desired<=shard_budget) {
            evict(*chosen,false);chosen->busy=true;++shard.stats.inflight;
            if(chosen->capacity<desired) {
                evict(*chosen,true);
                while(shard.stats.bytes+desired>shard_budget) {
                    Entry* oldest=nullptr;
                    for(auto& e:shard.entries)if(!e.busy && e.capacity && (!oldest || e.age<oldest->age))oldest=&e;
                    if(!oldest)break;
                    evict(*oldest,true);
                }
                if(shard.stats.bytes+desired<=shard_budget){chosen->capacity=desired;shard.stats.bytes+=desired;}
                else {chosen->busy=false;--shard.stats.inflight;chosen=nullptr;}
            }
        } else chosen=nullptr;
        if(!chosen)++shard.stats.declined;
    }
    if(!chosen)return compute(batch);
    Entry& result=*chosen; // Exclusive lease: no other caller can read/evict it.
    if(!result.storage) {
        result.storage.reset(new(std::nothrow) uint8_t[result.capacity]);
        const bool allocated=!!result.storage;
        {std::lock_guard<std::mutex> lock(shard.mutex);
            if(!allocated){shard.stats.bytes-=result.capacity;result.capacity=0;result.busy=false;
                --shard.stats.inflight;++shard.stats.declined;}
            else ++shard.stats.allocations;}
        if(!allocated)return compute(batch);
    }
    uint8_t* input=result.storage.get();uint8_t* output=input+input_bytes;
    memcpy(input,batch->positions,pbytes);memcpy(input+pbytes,batch->influences,wbytes);
    memcpy(input+pbytes+wbytes,batch->palette,mbytes);memcpy(output,input,pbytes);
    const ps3_skin4_batch owned={input+pbytes+wbytes,input+pbytes,output,output+pbytes,
        batch->vertices,batch->palette_entries,batch->allow_bounded};
    // Sticky flags cannot affect this pure calculation. Collect the flags it
    // raises, then OR them with this invocation's incoming flags on every use.
    _mm_setcsr(control);const int bounded=compute(&owned);const unsigned after=_mm_getcsr();
    result.key=key;result.stage=stage;result.vertices=batch->vertices;result.palette_entries=batch->palette_entries;
    result.allow_bounded=batch->allow_bounded;result.control=control;result.raised=after&63;result.bounded=bounded;
    publish(result);
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        result.busy=false;--shard.stats.inflight;
        if((after&~63u)==control){result.valid=true;result.age=++shard.age;++shard.stats.stored;}
        else {result.valid=false;++shard.stats.declined;_mm_setcsr(after|(incoming&63));}
    }
    return bounded;
}
extern "C" void ps3_skin4_result_stats(ps3_skin4_result_counters* out) {
    *out={};for(auto& s:cache().shards){std::lock_guard<std::mutex> lock(s.mutex);
#define ADD(f) out->f+=s.stats.f
        ADD(hits);ADD(misses);ADD(stored);ADD(declined);ADD(evicted);ADD(allocations);ADD(bytes);ADD(inflight);
        ADD(hit_vertices);ADD(miss_vertices);
#undef ADD
    }
}
extern "C" void ps3_skin4_result_reset(void) {
    for(auto& s:cache().shards){std::lock_guard<std::mutex> lock(s.mutex);assert(!s.stats.inflight);
        for(auto& e:s.entries)e=Entry{};s.age=0;s.stats={};}
}
