#ifndef BLEACH_NATIVE_JOB_AUDIT_H
#define BLEACH_NATIVE_JOB_AUDIT_H
/* Bounded, read-only job I/O audit. Captures whole jobs admitted in a short
 * window, with private initial stores for offline data-flow inspection.
 * This is diagnostic instrumentation, never a clean performance measurement. */
#include "spu_context.h"
#include <atomic>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" void (*g_spu_mfc_command_observer)(spu_context*,uint32_t,uint32_t,int);
extern "C" void (*g_spu_channel_observer)(spu_context*,uint32_t,int);
namespace bleach_job_audit {
struct Access {
    uint64_t start,end;
    uint32_t channel,command,pc,lr,ea_hi,ea,lsa,size,tag,atomic_status,stalled;
    uint32_t parent,flags;
    unsigned char bytes[32];
};
struct Job {
    std::atomic<bool> done{false};
    spu_context* owner=nullptr;
    uint32_t descriptor=0,descriptor_lsa=0,context=0,lane=0,pc_end=0,status=0;
    uint64_t start=0,end=0;
    unsigned dropped=0,pending=UINT32_MAX;
    unsigned channels[3][32]{};
    std::vector<Access> accesses;
    std::vector<unsigned char> initial;
};
static Job jobs[64];
static std::atomic<unsigned> claimed{0};
static thread_local Job* active=nullptr;
static std::string path;
static uint64_t origin,start_ms,end_ms;
static uint32_t selected_image;
static unsigned sampling_stride=1;
static uint32_t be32(const unsigned char* p)
{return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
static uint64_t stamp() { LARGE_INTEGER q;QueryPerformanceCounter(&q);return q.QuadPart; }
static void observe_channel(spu_context* ctx,uint32_t channel,int kind)
{
    Job* j=active;
    if(j && j->owner==ctx && channel<32 && kind>=0 && kind<3)++j->channels[kind][channel];
}
static void bytes_from_store(Access& a,const spu_context* ctx)
{
    const unsigned count=a.size<32?a.size:32;
    for(unsigned i=0;i<count;i++)a.bytes[i]=ctx->ls[(a.lsa+i)&SPU_LS_MASK];
}
static void observe(spu_context* ctx,uint32_t channel,uint32_t command,int after)
{
    Job* j=active;
    if(!j || j->owner!=ctx)return;
    if(after) {
        if(j->pending==UINT32_MAX)return;
        Access& parent=j->accesses[j->pending];
        parent.end=stamp();parent.atomic_status=ctx->atomic_stat;parent.stalled=ctx->list_stall_mask;
        const bool get=(command&0xf0)==0x40 || command==MFC_GETLLAR_CMD;
        if(get)bytes_from_store(parent,ctx);
        for(unsigned i=j->pending+1;i<j->accesses.size();i++) {
            Access& a=j->accesses[i];a.end=parent.end;a.stalled=parent.stalled;
            if(get)bytes_from_store(a,ctx);
        }
        j->pending=UINT32_MAX;return;
    }
    if(j->pending!=UINT32_MAX || j->accesses.size()>=2048) {++j->dropped;return;}
    Access a{};a.start=stamp();a.channel=channel;a.command=command;
    a.pc=ctx->pc;a.lr=ctx->gpr[0]._u32[0];a.ea_hi=ctx->mfc_eah;a.ea=ctx->mfc_eal;
    a.lsa=ctx->mfc_lsa&SPU_LS_MASK;a.size=ctx->mfc_size;a.tag=ctx->mfc_tag;
    a.parent=UINT32_MAX;
    const bool atomic=command==MFC_GETLLAR_CMD || command==MFC_PUTLLC_CMD ||
        command==MFC_PUTLLUC_CMD || command==MFC_PUTQLLUC_CMD;
    if(atomic){a.size=128;a.ea&=~127u;}
    bytes_from_store(a,ctx);
    j->pending=(unsigned)j->accesses.size();j->accesses.push_back(a);
    /* A list is recorded as a command plus proposed elements. Consumers must
     * reject stalled/truncated lists before interpreting elements as executed. */
    if(channel==MFC_Cmd && ((command&0xf0)==0x20 || (command&0xf0)==0x40) && (command&4)) {
        uint32_t dest=a.lsa&~15u;
        for(unsigned i=0;i<a.size/8;i++) {
            if(j->accesses.size()>=2048){++j->dropped;break;}
            const uint32_t at=(a.ea+i*8)&SPU_LS_MASK;
            unsigned char entry[8];for(unsigned k=0;k<8;k++)entry[k]=ctx->ls[(at+k)&SPU_LS_MASK];
            const uint32_t flags=be32(entry),ea=be32(entry+4),size=flags&0x7fff;
            Access e=a;e.parent=j->pending;e.flags=flags;e.ea=ea;e.size=size;
            e.lsa=dest|(ea&15);memset(e.bytes,0,sizeof(e.bytes));bytes_from_store(e,ctx);
            j->accesses.push_back(e);dest+=(size+15)&~15u;
            if(flags&0x80000000u)break;
        }
    }
}
static void dump()
{
    if(path.empty())return;
    const std::string index=path+".json";
    FILE* f=fopen(index.c_str(),"wb");if(!f){fprintf(stderr,"[job-audit] cannot write %s\n",index.c_str());return;}
    LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
    fprintf(f,"{\"version\":2,\"qpc_frequency\":%llu,\"image\":%u,\"sampling_stride\":%u,\"jobs\":[",
        (unsigned long long)frequency.QuadPart,selected_image,sampling_stride);
    unsigned complete=0;
    for(unsigned i=0;i<64;i++) {
        const Job& j=jobs[i];if(!j.done.load(std::memory_order_acquire))continue;
        const std::string snapshot=path+"-job-"+std::to_string(i)+".ls";
        FILE* b=fopen(snapshot.c_str(),"wb");bool saved=false;
        if(b){saved=fwrite(j.initial.data(),1,j.initial.size(),b)==j.initial.size();fclose(b);}
        fprintf(f,"%s{\"id\":%u,\"descriptor\":%u,\"descriptor_lsa\":%u,\"context\":%u,\"lane\":%u,\"start\":%llu,\"end\":%llu,\"pc_end\":%u,\"status\":%u,\"dropped\":%u,\"snapshot_saved\":%s,\"accesses\":[",
            complete++?",":"",i,j.descriptor,j.descriptor_lsa,j.context,j.lane,(unsigned long long)j.start,
            (unsigned long long)j.end,j.pc_end,j.status,j.dropped,saved?"true":"false");
        unsigned n=0;
        for(const Access& a:j.accesses) {
            fprintf(f,"%s[%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,\"",
                n++?",":"",(unsigned long long)a.start,(unsigned long long)a.end,
                a.channel,a.command,a.pc,a.lr,a.ea_hi,a.ea,a.lsa,a.size,a.tag,
                a.atomic_status,a.stalled,a.parent,a.flags);
            for(unsigned c=0;c<32;c++)fprintf(f,"%02x",a.bytes[c]);
            fputs("\"]",f);
        }
        fputs("],\"channels\":[",f);
        for(unsigned kind=0;kind<3;kind++) {
            fprintf(f,"%s[",kind?",":"");
            for(unsigned channel=0;channel<32;channel++)fprintf(f,"%s%u",channel?",":"",j.channels[kind][channel]);
            fputs("]",f);
        }
        fputs("]}",f);
    }
    fprintf(f,"],\"admissions\":%u,\"completed\":%u}\n",claimed.load(),complete);fclose(f);
    fprintf(stderr,"[job-audit] completed=%u admissions=%u index=%s\n",complete,claimed.load(),index.c_str());
}
static void initialize()
{
    const char* env=getenv("BLEACH_JOB_AUDIT");if(!env)return;
    unsigned start=0,duration=0,image=0;int used=0;
    if(sscanf(env,"%u:%u:%u:%n",&start,&duration,&image,&used)!=3 || !used || !duration || !env[used])abort();
    path=env+used;selected_image=image;start_ms=start;end_ms=(uint64_t)start+duration;
    const char* stride=getenv("BLEACH_JOB_AUDIT_STRIDE");
    if(stride){sampling_stride=(unsigned)strtoul(stride,nullptr,10);if(!sampling_stride)abort();}
    origin=GetTickCount64();g_spu_mfc_command_observer=observe;
    g_spu_channel_observer=observe_channel;atexit(dump);
    fprintf(stderr,"[job-audit] image=%u window_ms=%u..%llu max_jobs=64\n",image,start,(unsigned long long)end_ms);
}
struct Scope {
    Job* previous;Job* current=nullptr;
    Scope(spu_context* ctx,int image):previous(active) {
        active=nullptr;
        if(path.empty() || (uint32_t)image!=selected_image)return;
        const uint64_t now=GetTickCount64()-origin;
        if(now<start_ms || now>=end_ms)return;
        unsigned sequence=claimed.fetch_add(1);
        if(sequence%sampling_stride)return;
        unsigned index=sequence/sampling_stride;if(index>=64)return;
        current=&jobs[index];current->owner=ctx;current->context=ctx->gpr[3]._u32[0];
        current->descriptor_lsa=ctx->gpr[4]._u32[0];
        current->descriptor=be32(ctx->ls+((current->context+0x2c)&SPU_LS_MASK));
        current->lane=be32(ctx->ls+0x1c8);
        current->accesses.reserve(2048);current->initial.assign(ctx->ls,ctx->ls+SPU_LS_SIZE);
        current->start=stamp();active=current;
    }
    ~Scope() {
        if(current){current->end=stamp();current->pc_end=current->owner->pc;
            current->status=current->owner->status;current->done.store(true,std::memory_order_release);}
        active=previous;
    }
};
}
#endif
