#include "rsx_vertex_pull.h"
#include "rsx_vertex_formats.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

_Static_assert(sizeof(rsx_vertex_pull_attr)==48, "GPU attribute ABI");
_Static_assert(sizeof(rsx_vertex_pull_header)==784, "GPU packet ABI");
_Static_assert(sizeof(rsx_vertex_ref)==8, "GPU reference ABI");

static int rsx_vertex_pull_prepare_impl(rsx_vertex_pull_plan* out,
    const rsx_vertex_fetch_plan* fetch, const rsx_vertex_ref* refs,
    u32 count, u32 capacity, int linear)
{
    if(!out || !fetch || (!linear && !refs) || !count || fetch->layout.count>16)return 0;
    if(linear && (fetch->inline_data || count>0x100000u))return 0;
    rsx_vertex_pull_plan p={0};
    p.refs=linear?NULL:refs;p.header.version=linear?2:1;p.header.count=count;
    p.header.mask=fetch->layout.mask;p.header.refs_offset=linear?0:sizeof(p.header);
    u64 bytes=(sizeof(p.header)+(linear?0:(u64)count*sizeof(*refs))+15)&~15ull;
    if(bytes>capacity)return 0;
    for(u32 slot=0;slot<fetch->layout.count;slot++) {
        u32 a=fetch->layout.attrs[slot];if(a>=16)return 0;
        const rsx_vertex_fetch_attr* f=&fetch->attr[a];
        rsx_vertex_pull_attr* d=&p.header.attr[a];
        d->offset=UINT32_MAX;memcpy(d->defaults,f->default_value,16);
        if(!f->desc.type || !f->desc.size)continue;
        if(f->desc.type>7 || f->desc.size>4 || !f->stable_base ||
           !f->elem_size || !f->stride || f->stable_last<f->stable_first)return 0;
        /* Original vertex IDs, base zero, ordinary per-vertex attributes.
         * Other addressing contracts must use the existing reference path. */
        if(linear && (f->desc.frequency>1 || f->stable_first || f->stable_last<count-1))return 0;
        // Inline spans may contain fewer vertices than the submitted indices.
        // Do not change the existing partial/default/fatal fetch behavior.
        if(fetch->inline_data)for(u32 i=0;i<count;i++) {
            u32 e=rsx_vertex_element_index(refs[i].vertex_id,refs[i].base_index,
                f->desc.frequency,(fetch->divider_mask>>a)&1);
            if(e<f->stable_first || e>f->stable_last)return 0;
        }
        u64 span=(u64)(f->stable_last-f->stable_first)*f->stride+f->elem_size;
        if(span>UINT32_MAX || span>capacity ||
           (uintptr_t)f->stable_base>UINTPTR_MAX-span)return 0;
        d->stride=f->stride;d->first=f->stable_first;d->last=f->stable_last;
        d->type=f->desc.type;d->size=f->desc.size;d->frequency=f->desc.frequency;
        d->modulo=(fetch->divider_mask>>a)&1;
        p.spans[p.span_count++]=(rsx_vertex_pull_span){f->stable_base,(u32)span,0};
    }
    // Merge overlapping validated interleaved streams before copying them.
    for(u32 i=1;i<p.span_count;i++) {
        rsx_vertex_pull_span x=p.spans[i];u32 j=i;
        while(j && (uintptr_t)p.spans[j-1].source>(uintptr_t)x.source) {
            p.spans[j]=p.spans[j-1];--j;
        }
        p.spans[j]=x;
    }
    u32 merged=0;
    for(u32 i=0;i<p.span_count;i++) {
        rsx_vertex_pull_span x=p.spans[i];
        if(merged) {
            rsx_vertex_pull_span* last=&p.spans[merged-1];
            uintptr_t end=(uintptr_t)last->source+last->size;
            if((uintptr_t)x.source<=end) {
                u64 extent=(uintptr_t)x.source+x.size-(uintptr_t)last->source;
                if(extent>UINT32_MAX)return 0;
                if(extent>last->size)last->size=(u32)extent;
                continue;
            }
        }
        p.spans[merged++]=x;
    }
    p.span_count=merged;
    for(u32 i=0;i<merged;i++) {
        p.spans[i].offset=(u32)bytes;
        bytes=(bytes+p.spans[i].size+15)&~15ull;
        if(bytes>capacity || bytes>UINT32_MAX)return 0;
    }
    for(u32 slot=0;slot<fetch->layout.count;slot++) {
        u32 a=fetch->layout.attrs[slot];const rsx_vertex_fetch_attr* f=&fetch->attr[a];
        if(!p.header.attr[a].type)continue;
        int found=0;
        for(u32 i=0;i<merged;i++) {
            uintptr_t start=(uintptr_t)p.spans[i].source,source=(uintptr_t)f->stable_base;
            if(source>=start && source-start<p.spans[i].size) {
                p.header.attr[a].offset=p.spans[i].offset+(u32)(source-start);found=1;break;
            }
        }
        if(!found)return 0;
    }
    p.bytes=(u32)bytes;*out=p;return 1;
}
int rsx_vertex_pull_prepare(rsx_vertex_pull_plan* out,
    const rsx_vertex_fetch_plan* fetch,const rsx_vertex_ref* refs,u32 count,u32 capacity)
{ return rsx_vertex_pull_prepare_impl(out,fetch,refs,count,capacity,0); }
int rsx_vertex_pull_prepare_linear(rsx_vertex_pull_plan* out,
    const rsx_vertex_fetch_plan* fetch,u32 count,u32 capacity)
{ return rsx_vertex_pull_prepare_impl(out,fetch,NULL,count,capacity,1); }
int rsx_vertex_pull_write(const rsx_vertex_pull_plan* p,void* output,u32 capacity)
{
    if(!p || !output || p->bytes>capacity || p->bytes<sizeof(p->header))return 0;
    u8* dst=(u8*)output;
    memcpy(dst,&p->header,sizeof(p->header));
    u32 end=sizeof(p->header);
    if(p->header.refs_offset) {
        end=p->header.refs_offset+p->header.count*sizeof(*p->refs);
        memcpy(dst+p->header.refs_offset,p->refs,p->header.count*sizeof(*p->refs));
    }
    for(u32 i=0;i<p->span_count;i++) {
        const rsx_vertex_pull_span* s=&p->spans[i];
        memset(dst+end,0,s->offset-end);memcpy(dst+s->offset,s->source,s->size);
        end=s->offset+s->size;
    }
    memset(dst+end,0,p->bytes-end);return 1;
}

const char* rsx_vertex_pull_hlsl(void)
{
    return
    "\nByteAddressBuffer rsx_raw_vertices : register(t20);\n"
    "uint rsx_raw_byte(uint p){return (rsx_raw_vertices.Load(p&~3u)>>((p&3u)*8))&255u;}\n"
    "uint rsx_raw_be16(uint p){return (rsx_raw_byte(p)<<8)|rsx_raw_byte(p+1);}\n"
    "uint rsx_raw_be32(uint p){return (rsx_raw_be16(p)<<16)|rsx_raw_be16(p+2);}\n"
    "float rsx_raw_half(uint h){uint s=(h&32768u)<<16,e=(h>>10)&31u,m=h&1023u;"
      "return asfloat(s|(e==0?0:e==31?0x7f800000u|(m<<13):((e+112)<<23)|(m<<13)));}\n"
    "float4 rsx_pull_fetch(uint vertex,uint attr){\n"
    " uint4 head=rsx_raw_vertices.Load4(0);if(vertex>=head.y)return float4(0,0,0,1);\n"
    " uint base=16+attr*48;uint4 span=rsx_raw_vertices.Load4(base);"
      "uint4 fmt=rsx_raw_vertices.Load4(base+16);"
      "float4 def=asfloat(rsx_raw_vertices.Load4(base+32));\n"
    " if(span.x==0xffffffffu)return def;uint2 ref=head.w==0?uint2(vertex,0):rsx_raw_vertices.Load2(head.w+vertex*8);\n"
    " uint el=fmt.z<=1?((ref.x+ref.y)&0xfffffu):(fmt.w!=0?ref.x%fmt.z:ref.x/fmt.z);\n"
    " if(el<span.z||el>span.w)return def;uint p=span.x+(el-span.z)*span.y;float4 v=float4(0,0,0,1);\n"
    " if(fmt.x==6){uint w=rsx_raw_be32(p);int x=(int)(w<<21)>>21;int y=(int)(w<<10)>>21;"
      "int z=(int)w>>22;return float4((float)x/1023.0,(float)y/1023.0,(float)z/511.0,1);}\n"
    " [unroll]for(uint c=0;c<4;c++){if(c>=fmt.y)break;"
      "if(fmt.x==2)v[c]=asfloat(rsx_raw_be32(p+c*4));"
      "else if(fmt.x==3)v[c]=rsx_raw_half(rsx_raw_be16(p+c*2));"
      "else if(fmt.x==4)v[c]=(float)rsx_raw_byte(p+c)/255.0;"
      "else if(fmt.x==7)v[c]=(float)rsx_raw_byte(p+c);"
      "else {int s=(int)(rsx_raw_be16(p+c*2)<<16)>>16;v[c]=fmt.x==1?(float)s/32767.0:(float)s;}}\n"
    " return v;}\n";
}
int rsx_vertex_pull_wrap(char* shader,u32 capacity,u32 mask)
{
    char* entry=strstr(shader,"VSOutput main(VSInput input)");if(!entry)return 0;
    const char* decode=rsx_vertex_pull_hlsl();size_t old=strlen(shader),used=0;
    char tail[4096];
    int n=snprintf(tail,sizeof(tail),"\nVSOutput main(uint vertex_id:SV_VertexID){VSInput input=(VSInput)0;\n");
    if(n<0)return 0;used=(size_t)n;
    if(mask){for(u32 a=0;a<16;a++)if(mask&(1u<<a)) {
        n=snprintf(tail+used,sizeof(tail)-used,"input.a%u=rsx_pull_fetch(vertex_id,%u);\n",a,a);
        if(n<0 || (size_t)n>=sizeof(tail)-used)return 0;used+=(size_t)n;
    }}
    else {n=snprintf(tail+used,sizeof(tail)-used,"input.vertex_id=vertex_id;\n");used+=(size_t)n;}
    n=snprintf(tail+used,sizeof(tail)-used,"return rsxp(input);}\n");
    if(n<0 || (size_t)n>=sizeof(tail)-used)return 0;used+=(size_t)n;
    size_t dec=strlen(decode);if(old+dec+used+1>capacity)return 0;
    memcpy(entry+9,"rsxp",4);memcpy(shader+old,decode,dec);
    memcpy(shader+old+dec,tail,used+1);return 1;
}
