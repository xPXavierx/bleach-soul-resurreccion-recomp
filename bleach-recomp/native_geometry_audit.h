#ifndef BLEACH_NATIVE_GEOMETRY_AUDIT_H
#define BLEACH_NATIVE_GEOMETRY_AUDIT_H
/* Read-only dependency capture over already staged private inputs. Fingerprint
 * equality is diagnostic evidence, never authorization to skip a real job.
 * Frame tokens describe execution time, not proven producer ownership. */
#include "spu_context.h"
#include <atomic>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
extern "C" uint64_t bleach_engine_audit_token();
extern "C" void (*g_spu_mfc_command_observer)(spu_context*,uint32_t,uint32_t,int);
extern "C" {
void (*bleach_geometry_phase_observer)(spu_context*,unsigned,uint32_t,uint32_t,int)=nullptr;
}
namespace bleach_geometry_audit {
struct Input { uint32_t ea,size,lsa,flags; uint64_t hash; unsigned char prefix[16]; };
struct Write { uint32_t command,ea,size,lsa; uint64_t hash; };
struct Phase { uint32_t target,return_pc,count,flags; uint64_t ticks,private_ticks,started; };
struct Record {
    uint64_t begin,first_shared,end,token_begin,token_end;
    uint32_t descriptor_ea,lane,pc,status,ninput,flags,writes,reads,other,reserved;
    unsigned char descriptor[256]; Input inputs[20]; Write output[8];
    Phase phases[17];
};
static_assert(sizeof(Input)==40 && sizeof(Write)==24 && sizeof(Phase)==40 && sizeof(Record)==2008);
struct Slot { std::atomic<bool> done{false}; Record value{}; };
static Slot* slots;
static constexpr unsigned capacity=32768;
static std::atomic<unsigned> claimed{0};
static thread_local Record* active;
static std::string path;
static uint64_t origin,start_ms,end_ms;
static void (*previous_observer)(spu_context*,uint32_t,uint32_t,int);
struct ExactEntry { uint64_t key=0;std::vector<unsigned char> bytes; };
static ExactEntry* exact_entries;
static SRWLOCK exact_locks[64]{};
static uint32_t be32(const unsigned char* p)
{return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
static uint64_t stamp(){LARGE_INTEGER q;QueryPerformanceCounter(&q);return q.QuadPart;}
static void phase(spu_context*,unsigned index,uint32_t target,uint32_t ret,int after)
{
    Record* r=active;if(!r)return;
    if(index>=17){r->flags|=8;return;}
    auto& p=r->phases[index];uint64_t now=stamp();
    if(!after) {
        if(p.started){p.flags|=1;r->flags|=8;return;}
        p.target=target;p.return_pc=ret;p.started=now;
    } else {
        if(!p.started || p.target!=target || p.return_pc!=ret){p.flags|=2;r->flags|=8;return;}
        ++p.count;p.ticks+=now-p.started;
        uint64_t end=r->first_shared && r->first_shared<now?r->first_shared:now;
        if(end>p.started)p.private_ticks+=end-p.started;
        p.started=0;
    }
}
static uint64_t hash_bytes(const unsigned char* p,uint32_t size)
{
    // Non-cryptographic full-byte fingerprint; much cheaper than bytewise FNV.
    uint64_t h=0x9e3779b185ebca87ull^size;
    while(size>=8){uint64_t v;memcpy(&v,p,8);h^=v*0xc2b2ae3d27d4eb4full;
        h=(h<<31)|(h>>33);h*=0x9e3779b185ebca87ull;p+=8;size-=8;}
    uint64_t tail=0;if(size)memcpy(&tail,p,size);h^=tail;
    h^=h>>33;h*=0xff51afd7ed558ccdull;h^=h>>33;h*=0xc4ceb9fe1a85ec53ull;return h^(h>>33);
}
static uint32_t verify_inputs(const Record& r,const unsigned char* ls)
{
    if(!exact_entries)return 0;
    uint64_t key=hash_bytes(r.descriptor,48)^hash_bytes(r.descriptor+216,40);
    size_t bytes=88+80;
    for(const auto& i:r.inputs){key=(key^i.hash^i.size)*0x9e3779b185ebca87ull;bytes+=i.size;}
    auto& e=exact_entries[key&1023];auto* lock=&exact_locks[key&63];
    AcquireSRWLockExclusive(lock);
    bool candidate=e.key==key && e.bytes.size()==bytes;
    bool equal=candidate;size_t at=0;
    auto compare=[&](const void* p,size_t n){if(equal && memcmp(e.bytes.data()+at,p,n))equal=false;at+=n;};
    compare(r.descriptor,48);compare(r.descriptor+216,40);
    for(const auto& i:r.inputs)compare(&i.size,4);
    for(const auto& i:r.inputs)if(i.size)compare(ls+i.lsa,i.size);
    if(!equal) {
        e.key=key;e.bytes.resize(bytes);at=0;
        auto copy=[&](const void* p,size_t n){memcpy(e.bytes.data()+at,p,n);at+=n;};
        copy(r.descriptor,48);copy(r.descriptor+216,40);
        for(const auto& i:r.inputs)copy(&i.size,4);
        for(const auto& i:r.inputs)if(i.size)copy(ls+i.lsa,i.size);
    }
    ReleaseSRWLockExclusive(lock);return equal?1:(candidate?2:0);
}
static void observe(spu_context* ctx,uint32_t channel,uint32_t command,int after)
{
    if(previous_observer)previous_observer(ctx,channel,command,after);
    Record* r=active;if(!r || after)return;
    if(!r->first_shared)r->first_shared=stamp();
    if(channel!=MFC_Cmd){++r->other;return;}
    if((command&0xf0)==0x20 && !(command&4)) {
        unsigned i=r->writes++;if(i>=8){r->flags|=2;return;}
        auto& w=r->output[i];w.command=command;w.ea=ctx->mfc_eal;
        w.size=ctx->mfc_size;w.lsa=ctx->mfc_lsa&SPU_LS_MASK;
        if(ctx->mfc_eah || w.lsa+w.size>SPU_LS_SIZE){r->flags|=4;return;}
        w.hash=hash_bytes(ctx->ls+w.lsa,w.size);
    } else if((command&0xf0)==0x40) ++r->reads;
    else ++r->other; // atomic publication and unknown/list operations remain visible
}
static void dump()
{
    if(!slots)return;
    FILE* f=fopen(path.c_str(),"wb");if(!f){fprintf(stderr,"[geometry-input-audit] cannot write\n");return;}
    const char magic[8]={'G','E','O','A','U','D','2',0};
    unsigned count=0;for(unsigned i=0;i<capacity;i++)if(slots[i].done.load(std::memory_order_acquire))count++;
    const uint32_t bytes=sizeof(Record);LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
    fwrite(magic,1,8,f);fwrite(&bytes,4,1,f);fwrite(&count,4,1,f);fwrite(&frequency.QuadPart,8,1,f);
    for(unsigned i=0;i<capacity;i++)if(slots[i].done.load(std::memory_order_acquire))fwrite(&slots[i].value,sizeof(Record),1,f);
    fclose(f);fprintf(stderr,"[geometry-input-audit] completed=%u admissions=%u record_bytes=%u path=%s\n",count,claimed.load(),bytes,path.c_str());
}
static void initialize()
{
    const char* e=getenv("BLEACH_GEOMETRY_INPUT_AUDIT");if(!e)return;
    unsigned start,duration;int used=0;
    if(sscanf(e,"%u:%u:%n",&start,&duration,&used)!=2 || !used || !duration || !e[used])abort();
    path=e+used;start_ms=start;end_ms=uint64_t(start)+duration;origin=GetTickCount64();
    slots=new Slot[capacity];previous_observer=g_spu_mfc_command_observer;
    const char* exact=getenv("BLEACH_GEOMETRY_INPUT_EXACT");
    if(exact && exact[0]=='1')exact_entries=new ExactEntry[1024];
    g_spu_mfc_command_observer=observe;atexit(dump);
    bleach_geometry_phase_observer=phase;
}
struct Scope {
    Record* previous;Slot* slot=nullptr;spu_context* owner;
    Scope(spu_context* ctx,int image):previous(active),owner(ctx) {
        active=nullptr;if(!slots || image!=1014)return;
        uint64_t now=GetTickCount64()-origin;if(now<start_ms || now>=end_ms)return;
        unsigned index=claimed.fetch_add(1);if(index>=capacity)return;
        slot=&slots[index];auto& r=slot->value;
        uint32_t context=ctx->gpr[3]._u32[0],desc=ctx->gpr[4]._u32[0];
        if(ctx->ls!=ctx->ls_store || context+48>SPU_LS_SIZE || desc+256>SPU_LS_SIZE) {r.flags|=1;return;}
        r.descriptor_ea=be32(ctx->ls+context+44);r.lane=be32(ctx->ls+0x1c8);
        memcpy(r.descriptor,ctx->ls+desc,256);
        r.ninput=(uint32_t(r.descriptor[10])<<8|r.descriptor[11])/8;
        if(r.ninput>20 || be32(r.descriptor+0xc) || be32(r.descriptor+0x24)){r.flags|=1;return;}
        uint32_t at=be32(ctx->ls+context);
        for(unsigned i=0;i<r.ninput;i++) {
            auto& input=r.inputs[i];input.flags=be32(r.descriptor+0x30+i*8);
            input.size=input.flags&0x7fff;input.ea=be32(r.descriptor+0x34+i*8);input.lsa=at;
            if(!input.size)continue;
            if(!input.ea || at>SPU_LS_SIZE || input.size>SPU_LS_SIZE-at){r.flags|=1;return;}
            input.hash=hash_bytes(ctx->ls+at,input.size);
            memcpy(input.prefix,ctx->ls+at,input.size<16?input.size:16);
            at+=(input.size+15)&~15u;
        }
        r.reserved=verify_inputs(r,ctx->ls);
        r.token_begin=bleach_engine_audit_token();r.begin=stamp();active=&r;
    }
    ~Scope() {
        if(slot){auto& r=slot->value;r.end=stamp();r.token_end=bleach_engine_audit_token();
            for(auto& p:r.phases)if(p.started){p.flags|=4;r.flags|=8;}
            r.pc=owner->pc;r.status=owner->status;slot->done.store(true,std::memory_order_release);}
        active=previous;
    }
};
}
#endif
