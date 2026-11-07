#ifndef BLEACH_NATIVE_GEOMETRY_RESIDENT_RUNTIME_H
#define BLEACH_NATIVE_GEOMETRY_RESIDENT_RUNTIME_H
#include "native_geometry_pose_plan.h"
#include "native_geometry_resident.h"
#include "native_geometry_trace.h"
extern int bleach_native_resident_mode;
extern unsigned bleach_native_resident_stride;
extern void bleach_native_resident_record(unsigned,int,int);
#if defined(__FMA__)
typedef struct bleach_resident_plan {
    bleach_decode_plan decode;
    bleach_pose_plan pose;
    uint32_t parameters[16];
    uint32_t tail_bindings[32];
    unsigned char tail_inputs[768];
    ps3_geo_resident_input input[10];
    ps3_geo_resident_output output[6];
    unsigned ni,no;
} bleach_resident_plan;
static int bleach_resident_prepare(spu_context* c,bleach_resident_plan* p){
    memset(p,0,sizeof(*p));
    const uint32_t sp=c->gpr[1]._u32[0],descriptor=c->gpr[3]._u32[0];
    // 7F58 reads blend-shape commands from shared memory when r84 is nonzero.
    // Those jobs retain their original complete execution and publication.
    if(c->gpr[84]._u32[0] || sp>SPU_LS_SIZE-0x100 ||
       !bleach_decode_prepare(c->ls,SPU_LS_SIZE,descriptor,c->gpr[4]._u32[0],
           c->gpr[6]._u32[0],sp,&p->decode))return 0;
    const uint32_t palette=ps3_geo_read_be32(c->ls+sp+0xbc),
        influences=ps3_geo_read_be32(c->ls+sp+0xc4),
        matrices=(uint32_t)(c->gpr[87]._u32[0]+c->gpr[86]._u32[0])/48;
    if(!bleach_pose_prepare_descriptor(c->ls,SPU_LS_SIZE,descriptor,palette,matrices,
        influences,sp,&p->pose,p->decode.result_descriptor))return 0;
    const ps3_geometry_decode_job* d=&p->decode.job;
    const ps3_geometry_pose_job* q=&p->pose.job;
    // Tiny jobs retain the original stack-local scratch path.
    if(q->vertices<=12 || q->direction_count!=d->packed_count ||
       q->positions.data!=d->vectors[0].data)return 0;
    for(unsigned i=0;i<q->direction_count;i++)
        if(q->directions[i].data!=d->vectors[i+1].data)return 0;
    for(unsigned i=0;i<=d->packed_count;i++)
        if(ps3_geo_overlap(d->vectors[i].data,d->vectors[i].bytes,q->palette.data,q->palette.bytes) ||
           ps3_geo_overlap(d->vectors[i].data,d->vectors[i].bytes,q->influences.data,q->influences.bytes))return 0;
    uint32_t* m=p->parameters;
    m[0]=0x42524701;m[1]=((unsigned)c->ls[descriptor+8]<<8)|c->ls[descriptor+9];
    m[2]=d->vertices;m[3]=d->packed_count;m[4]=d->packed_stride;
    for(unsigned i=0;i<3;i++)m[5+i]=d->packed_offset[i];
    m[8]=q->palette_entries;m[9]=q->normalize_vertices;m[10]=q->chunk_vertices;
    m[11]=q->direction_count;m[12]=q->tail_start;
    p->input[p->ni++]=(ps3_geo_resident_input){m,sizeof(p->parameters)};
    p->input[p->ni++]=(ps3_geo_resident_input){d->xyz_f32_be.data,d->xyz_f32_be.bytes};
    p->input[p->ni++]=(ps3_geo_resident_input){d->packed_vectors_be.data,d->packed_vectors_be.bytes};
    p->input[p->ni++]=(ps3_geo_resident_input){q->palette.data,q->palette.bytes};
    p->input[p->ni++]=(ps3_geo_resident_input){q->influences.data,q->influences.bytes};
    p->output[p->no++]=(ps3_geo_resident_output){q->positions.data,q->positions.bytes};
    for(unsigned i=0;i<q->direction_count;i++){
        size_t decoded=(size_t)d->vertices*16;
        p->input[p->ni++]=(ps3_geo_resident_input){q->directions[i].data+decoded,q->directions[i].bytes-decoded};
        p->output[p->no++]=(ps3_geo_resident_output){q->directions[i].data,q->directions[i].bytes};
    }
    p->output[p->no++]=(ps3_geo_resident_output){q->palette.data,q->palette.bytes};
    p->output[p->no++]=(ps3_geo_resident_output){q->frames.data,q->frames.bytes};
    // SIMD padding can read matrices from adjacent storage. Preserve the
    // dependency graph, not absolute LS addresses: an alias refers to a typed
    // output and offset; external sources carry all 48 input bytes. At this
    // tail every output is initialized from the keyed inputs: decoded vectors,
    // converted palette and scratch from earlier complete chunks. Normalization
    // padding is explicitly an input above. Cross-boundary/descriptor aliases
    // stay on the original path until they have their own contract.
    for(unsigned v=q->tail_start;v<q->vertices;v++)for(unsigned k=0;k<4;k++){
        unsigned item=(v-q->tail_start)*4+k;
        const unsigned char* src=q->tail_sources[v-q->tail_start][k].data;
        if(ps3_geo_overlap(src,48,c->ls+descriptor,416))return 0;
        int alias=-1;
        for(unsigned i=0;i<p->no;i++)if(ps3_geo_overlap(src,48,p->output[i].data,p->output[i].bytes)){
            const unsigned char* base=(const unsigned char*)p->output[i].data;
            if(src<base || (size_t)(src-base)>p->output[i].bytes || 48>p->output[i].bytes-(size_t)(src-base))return 0;
            alias=(int)i;p->tail_bindings[item*2]=i+1;p->tail_bindings[item*2+1]=(uint32_t)(src-base);break;
        }
        if(alias<0)memcpy(p->tail_inputs+item*48,src,48);
    }
    p->input[p->ni++]=(ps3_geo_resident_input){p->tail_bindings,sizeof(p->tail_bindings)};
    p->input[p->ni++]=(ps3_geo_resident_input){p->tail_inputs,sizeof(p->tail_inputs)};
    return 1;
}
static int bleach_geometry_resident_try(spu_context* c,void(*run)(spu_context*),
    void(*between)(spu_context*),void(*reference)(spu_context*)){
    if(!bleach_native_resident_mode || bleach_geometry_phase_observer)return 0;
    const uint64_t trace_total=ps3_geo_trace_begin();
    uint64_t trace_stage=trace_total;
    bleach_resident_plan p;
    const int prepared=bleach_kernel_regions_enabled(c) && !g_ps3_phase_capture &&
       (_mm_getcsr()&0xffc0)==0xffc0 && bleach_resident_prepare(c,&p);
    ps3_geo_trace_end(100,prepared?p.pose.job.vertices:0,prepared?0:2,trace_stage);
    if(!prepared){
        ps3_geo_trace_end(107,0,2,trace_total);
        bleach_native_resident_record(0,0,0);return 0;
    }
    trace_stage=trace_total?ps3_ft_now():0;
    int hit=0;ps3_geo_resident_lease* lease=ps3_geo_resident_acquire(p.input,p.ni,p.output,p.no,&hit);
    ps3_geo_trace_end(101,p.pose.job.vertices,lease?(unsigned)hit:2,trace_stage);
    if(!lease){ps3_geo_trace_end(107,p.pose.job.vertices,2,trace_total);bleach_native_resident_record(0,0,0);return 0;}
    static SPU_THREAD_LOCAL unsigned calls;
    const int verify=bleach_native_resident_stride && ++calls%bleach_native_resident_stride==0;
    static SPU_THREAD_LOCAL spu_context* original;
    const unsigned fp_in=_mm_getcsr();
    if(verify){
        if(!original)original=(spu_context*)malloc(sizeof(*original));
        if(!original)abort();memcpy(original,c,sizeof(*c));original->ls=original->ls_store;
    }
    trace_stage=trace_total?ps3_ft_now():0;
    if(hit){
        // Rebind descriptor addresses to THIS job. Shared publication, culling
        // and view calculation still run for every consumer in original order.
        memcpy(c->ls+p.decode.descriptor,p.decode.result_descriptor,416);
        c->gpr[0]=spu_link(0x408);c->pc=0x408;g_spu_trampoline_fn=0;
        between(c);
        if(c->gpr[3]._u32[0]!=p.decode.descriptor ||
           c->gpr[4]._u32[0]!=(uint32_t)(p.pose.job.palette.data-c->ls) ||
           c->gpr[5]._u32[0]!=p.pose.job.palette_entries ||
           c->gpr[6]._u32[0]!=(uint32_t)(p.pose.job.influences.data-c->ls))abort();
        ps3_geo_trace_end(102,p.pose.job.vertices,1,trace_stage);
        trace_stage=trace_total?ps3_ft_now():0;
        if(!ps3_geo_resident_restore(lease,p.output,p.no))abort();
        ps3_geo_trace_end(103,p.pose.job.vertices,1,trace_stage);
        ps3_geo_write_be32(c->ls+p.pose.descriptor+0x58,p.pose.first_free);
        c->gpr[0]=spu_link(0x480);c->pc=0x480;g_spu_trampoline_fn=0;
    }else{
        run(c);
        ps3_geo_trace_end(104,p.pose.job.vertices,0,trace_stage);
        if(c->status || c->pc!=0x480 || g_spu_trampoline_fn){ps3_geo_resident_release(lease);return 1;}
        trace_stage=trace_total?ps3_ft_now():0;
        if(!ps3_geo_resident_publish(lease,p.output,p.no))abort();
        ps3_geo_trace_end(105,p.pose.job.vertices,0,trace_stage);
    }
    trace_stage=trace_total?ps3_ft_now():0;
    ps3_geo_resident_release(lease);
    ps3_geo_trace_end(106,p.pose.job.vertices,(unsigned)hit,trace_stage);
    ps3_geo_trace_end(107,p.pose.job.vertices,(unsigned)hit,trace_total);
    if(verify){
        const unsigned fp_out=_mm_getcsr();_mm_setcsr(fp_in);g_spu_trampoline_fn=0;
        reference(original);
        const uint32_t sp=c->gpr[1]._u32[0];
        const int same=c->pc==original->pc && c->status==original->status &&
            c->host_depth==original->host_depth && c->image_id==original->image_id &&
            !memcmp(&c->gpr[0],&original->gpr[0],2*sizeof(u128)) &&
            !memcmp(&c->gpr[80],&original->gpr[80],48*sizeof(u128)) &&
            !memcmp(c->ls,original->ls,sp-1200) &&
            !memcmp(c->ls+sp+32,original->ls+sp+32,SPU_LS_SIZE-sp-32) &&
            (_mm_getcsr()&0xffc0)==(fp_out&0xffc0) && !g_spu_trampoline_fn;
        _mm_setcsr(fp_out);g_spu_trampoline_fn=0;
        if(!same){
            unsigned first=SPU_LS_SIZE;for(unsigned at=0;at<SPU_LS_SIZE;at++){
                if(at>=sp-1200 && at<sp+32)continue;
                if(c->ls[at]!=original->ls[at]){first=at;break;}
            }
            fprintf(stderr,"[native-resident] MISMATCH call=%u hit=%d vertices=%u first=%x\n",calls,hit,p.pose.job.vertices,first);abort();
        }
    }
    bleach_native_resident_record(p.pose.job.vertices,hit,verify);return 1;
}
#else
static int bleach_geometry_resident_try(spu_context* c,void(*run)(spu_context*),void(*between)(spu_context*),void(*reference)(spu_context*)){
    (void)c;(void)run;(void)between;(void)reference;return 0;
}
#endif
#endif
