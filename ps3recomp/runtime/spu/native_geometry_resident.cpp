#include "native_geometry_resident.h"
#include "native_geometry_trace.h"
#include <array>
#include <cstring>
#include <mutex>
#include <memory>
#include <cstdlib>
namespace {
constexpr unsigned shards=64,ways=64,max_spans=16;
constexpr size_t shard_budget=2*1024*1024,max_record=512*1024;
}
struct ps3_geo_resident_lease {
    uint64_t hash=0,age=0; unsigned pins=0,shard=0,inputs=0,outputs=0;
    bool ready=false;
    size_t in_sizes[max_spans]{},out_sizes[max_spans]{},input_bytes=0,output_bytes=0;
    std::unique_ptr<unsigned char[]> bytes;
};
namespace {
struct Shard {
    std::mutex mutex; std::array<ps3_geo_resident_lease,ways> entries;
    ps3_geo_resident_stats stats{}; uint64_t clock=0;
};
// Process lifetime: the host may exit while unrelated service threads exist.
std::array<Shard,shards>& table(){static auto* p=new std::array<Shard,shards>;return *p;}
uint64_t mix(uint64_t h,uint64_t v){h^=v*0xc2b2ae3d27d4eb4full;h=(h<<31)|(h>>33);return h*0x9e3779b185ebca87ull;}
// Four independent chains over 32-byte blocks: one chain is bound by the latency of its
// multiplies. Only a key for this process's cache; a hit is still compared byte for byte.
uint64_t hash_data(uint64_t h,const void* data,size_t n){
    auto* p=static_cast<const unsigned char*>(data);h=mix(h,n);
    if(n>=32){
        uint64_t a=h,b=h^0x9e3779b97f4a7c15ull,c=h^0xbf58476d1ce4e5b9ull,d=h^0x94d049bb133111ebull;
        while(n>=32){
            uint64_t v[4];memcpy(v,p,32);
            a=mix(a,v[0]);b=mix(b,v[1]);c=mix(c,v[2]);d=mix(d,v[3]);p+=32;n-=32;
        }
        h=mix(mix(mix(mix(h,a),b),c),d);
    }
    while(n>=8){uint64_t v;memcpy(&v,p,8);h=mix(h,v);p+=8;n-=8;}
    uint64_t tail=0;if(n)memcpy(&tail,p,n);return mix(h,tail);
}
bool shape(const ps3_geo_resident_lease* e,const ps3_geo_resident_output* out,unsigned n){
    if(n!=e->outputs)return false;
    for(unsigned i=0;i<n;i++)if(out[i].bytes!=e->out_sizes[i] || (out[i].bytes&&!out[i].data))return false;
    return true;
}
void discard(Shard& s,ps3_geo_resident_lease& e){
    if(e.pins)abort();
    if(e.bytes){s.stats.bytes-=e.input_bytes+e.output_bytes;++s.stats.evicted;}
    e.bytes.reset();e.ready=false;e.hash=0;e.age=0;e.input_bytes=e.output_bytes=0;
}
}
extern "C" ps3_geo_resident_lease* ps3_geo_resident_acquire(const ps3_geo_resident_input* in,unsigned ni,
    const ps3_geo_resident_output* out,unsigned no,int* hit){
    if(!hit)return nullptr;*hit=0;
    if(!in || !out || !ni || ni>max_spans || !no || no>max_spans)return nullptr;
    uint64_t trace_stage=ps3_geo_trace_begin();
    size_t ib=0,ob=0;uint64_t h=mix(0x4752455349440001ull,ni);
    for(unsigned i=0;i<ni;i++){
        if((in[i].bytes&&!in[i].data) || in[i].bytes>max_record-ib)return nullptr;
        ib+=in[i].bytes;h=hash_data(h,in[i].data,in[i].bytes);
    }
    ps3_geo_trace_end(108,0,0,trace_stage);
    h=mix(h,no);
    for(unsigned i=0;i<no;i++){
        if((out[i].bytes&&!out[i].data) || out[i].bytes>max_record-ib-ob)return nullptr;
        ob+=out[i].bytes;h=mix(h,out[i].bytes);
    }
    if(!ob)return nullptr;
    h^=h>>33;h*=0xff51afd7ed558ccdull;h^=h>>33;
    const unsigned bucket=h&(shards-1);auto& s=table()[bucket];
    trace_stage=trace_stage?ps3_ft_now():0;
    std::lock_guard<std::mutex> guard(s.mutex);
    ps3_geo_trace_end(109,0,0,trace_stage);
    ++s.clock;s.stats.input_bytes+=ib;
    ps3_geo_resident_lease* victim=nullptr;
    for(auto& e:s.entries){
        if(e.ready && e.hash==h && e.inputs==ni && shape(&e,out,no)){
            trace_stage=trace_stage?ps3_ft_now():0;
            bool equal=true;size_t at=0;
            for(unsigned i=0;i<ni;i++){
                if(e.in_sizes[i]!=in[i].bytes || (in[i].bytes&&memcmp(e.bytes.get()+at,in[i].data,in[i].bytes))){equal=false;break;}
                at+=in[i].bytes;
            }
            ps3_geo_trace_end(110,0,equal?1:0,trace_stage);
            if(equal){++e.pins;e.age=s.clock;++s.stats.hits;*hit=1;return &e;}
        }
        if(!e.pins && (!victim || e.age<victim->age))victim=&e;
    }
    if(!victim){++s.stats.declined;return nullptr;}
    discard(s,*victim);
    while(s.stats.bytes+ib+ob>shard_budget){
        ps3_geo_resident_lease* old=nullptr;
        for(auto& e:s.entries)if(&e!=victim && !e.pins && e.bytes && (!old || e.age<old->age))old=&e;
        if(!old){++s.stats.declined;return nullptr;}discard(s,*old);
    }
    auto& e=*victim;e.bytes.reset(new(std::nothrow) unsigned char[ib+ob]);
    if(!e.bytes){++s.stats.declined;return nullptr;}
    e.hash=h;e.age=s.clock;e.shard=bucket;e.pins=1;e.inputs=ni;e.outputs=no;
    e.input_bytes=ib;e.output_bytes=ob;size_t at=0;
    for(unsigned i=0;i<ni;i++){e.in_sizes[i]=in[i].bytes;if(in[i].bytes)memcpy(e.bytes.get()+at,in[i].data,in[i].bytes);at+=in[i].bytes;}
    for(unsigned i=0;i<no;i++)e.out_sizes[i]=out[i].bytes;
    s.stats.bytes+=ib+ob;++s.stats.misses;return &e;
}
extern "C" int ps3_geo_resident_restore(ps3_geo_resident_lease* e,const ps3_geo_resident_output* out,unsigned n){
    if(!e || !e->ready || !shape(e,out,n))return 0;
    size_t at=e->input_bytes;
    for(unsigned i=0;i<n;i++){if(out[i].bytes)memcpy(out[i].data,e->bytes.get()+at,out[i].bytes);at+=out[i].bytes;}
    return 1;
}
extern "C" int ps3_geo_resident_publish(ps3_geo_resident_lease* e,const ps3_geo_resident_output* out,unsigned n){
    if(!e || e->ready || !shape(e,out,n))return 0;
    size_t at=e->input_bytes;
    for(unsigned i=0;i<n;i++){if(out[i].bytes)memcpy(e->bytes.get()+at,out[i].data,out[i].bytes);at+=out[i].bytes;}
    auto& s=table()[e->shard];std::lock_guard<std::mutex> guard(s.mutex);
    e->ready=true;++s.stats.published;s.stats.output_bytes+=e->output_bytes;return 1;
}
extern "C" void ps3_geo_resident_release(ps3_geo_resident_lease* e){
    if(!e)return;auto& s=table()[e->shard];std::lock_guard<std::mutex> guard(s.mutex);
    if(!e->pins)abort();--e->pins;if(!e->pins&&!e->ready)discard(s,*e);
}
extern "C" void ps3_geo_resident_get_stats(ps3_geo_resident_stats* out){
    *out={};for(auto& s:table()){std::lock_guard<std::mutex> guard(s.mutex);
#define ADD(x) out->x+=s.stats.x
        ADD(hits);ADD(misses);ADD(declined);ADD(published);ADD(evicted);ADD(input_bytes);ADD(output_bytes);ADD(bytes);
#undef ADD
    }
}
extern "C" void ps3_geo_resident_reset(){
    for(auto& s:table()){std::lock_guard<std::mutex> guard(s.mutex);
        for(auto& e:s.entries)discard(s,e);s.stats={};s.clock=0;}
}
