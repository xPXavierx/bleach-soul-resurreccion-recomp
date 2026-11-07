/* Immutable program analysis, independent of the graphics API.
 * Single consumer ownership. Guest addresses are hints, never identities:
 * every hit validates code bytes. Inline FP constants are dynamic data.
 * Pointers returned here remain valid only until the next lookup of that kind.
 */
#ifndef RSX_NATIVE_PROGRAM_H
#define RSX_NATIVE_PROGRAM_H
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"
#include <stdlib.h>
#include <string.h>

#define RSX_NP_RECORDS 128u
#define RSX_NP_BINDINGS 1024u
typedef const u8* (*rsx_np_resolve_fn)(void*, u32 minimum_bytes);
typedef struct rsx_np_record {
    u8* code;
    u32 size, generation, constant_count, input_mask, txl_mask, sampled_units;
    u32* constants;
    int branch;
    u64 hash;
    struct { u64 seed, hash; int valid; } combinations[8];
    u32 next_combination;
} rsx_np_record;
typedef struct rsx_np_binding { u64 address; u32 slot, generation; } rsx_np_binding;
typedef struct rsx_np_cache {
    rsx_np_record records[RSX_NP_RECORDS];
    rsx_np_binding bindings[RSX_NP_BINDINGS];
    u32 next_slot, generation;
    u64 hits, builds, shared, invalidations, rejected;
} rsx_np_cache;

static u64 rsx_np_hash(const void* data, u32 size, u64 seed)
{
    const u8* p = (const u8*)data;
    for (u32 i=0; i<size; ++i) { seed ^= p[i]; seed *= 1099511628211ull; }
    return seed;
}
static int rsx_np_equal(const rsx_np_record* r, const u8* p)
{
    if (!r->code || !p) return 0;
    /* Branch decoding has different constant traversal in the legacy
     * texture analyser. Validate its entire byte stream conservatively. */
    if (r->branch || !r->constant_count) return !memcmp(r->code,p,r->size);
    u32 begin=0;
    for (u32 i=0; i<r->constant_count; ++i) {
        u32 end=r->constants[i];
        if (memcmp(r->code+begin,p+begin,end-begin)) return 0;
        begin=end+16;
    }
    return !memcmp(r->code+begin,p+begin,r->size-begin);
}
static rsx_np_record* rsx_np_bound(rsx_np_cache* c, u64 address,
    rsx_np_resolve_fn resolve, void* context)
{
    rsx_np_binding* b=&c->bindings[(address^(address>>13))&(RSX_NP_BINDINGS-1)];
    if (b->generation && b->address==address) {
        rsx_np_record* r=&c->records[b->slot];
        if (r->generation==b->generation) {
            if (rsx_np_equal(r,resolve(context,r->size))) { ++c->hits; return r; }
            ++c->invalidations;
        }
    }
    return NULL;
}
static void rsx_np_bind(rsx_np_cache* c, u64 address, rsx_np_record* r)
{
    rsx_np_binding* b=&c->bindings[(address^(address>>13))&(RSX_NP_BINDINGS-1)];
    *b=(rsx_np_binding){address,(u32)(r-c->records),r->generation};
}
static rsx_np_record* rsx_np_install(rsx_np_cache* c, u64 address,
    const u8* code, u32 size, u64 hash, const u32* constants, u32 count, int branch)
{
    for (u32 i=0; i<RSX_NP_RECORDS; ++i) {
        rsx_np_record* r=&c->records[i];
        if (r->size==size && r->hash==hash && rsx_np_equal(r,code)) {
            ++c->shared; rsx_np_bind(c,address,r); return r;
        }
    }
    u8* copy=(u8*)malloc(size);
    u32* offsets=count ? (u32*)malloc(count*sizeof(u32)) : NULL;
    if (!copy || (count && !offsets)) { free(copy); free(offsets); return NULL; }
    memcpy(copy,code,size);
    if (count) memcpy(offsets,constants,count*sizeof(u32));
    rsx_np_record* r=&c->records[c->next_slot++ % RSX_NP_RECORDS];
    free(r->code); free(r->constants); memset(r,0,sizeof(*r));
    /* Generation wrap invalidates all address hints. */
    if (!++c->generation) { memset(c->bindings,0,sizeof(c->bindings)); ++c->generation; }
    r->code=copy; r->constants=offsets; r->size=size; r->hash=hash;
    r->constant_count=count; r->branch=branch; r->generation=c->generation;
    ++c->builds; rsx_np_bind(c,address,r); return r;
}
static rsx_np_record* rsx_np_fp(rsx_np_cache* c, u64 address,
    rsx_np_resolve_fn resolve, void* context)
{
    rsx_np_record* r=rsx_np_bound(c,address,resolve,context);
    if (r) return r;
    u32 off=0, count=0, offsets[RSX_FP_MAX_INLINE_CONSTANTS]; int branch=0;
    while (off+16<=0x10000u) {
        const u8* p=resolve(context,off+16);
        if (!p) break;
        const u32 w0=rsx_fp_read_word(p+off), w1=rsx_fp_read_word(p+off+4),
            w2=rsx_fp_read_word(p+off+8), w3=rsx_fp_read_word(p+off+12);
        branch |= (w2>>31)!=0;
        off+=16;
        if ((w1&3)==2 || (w2&3)==2 || (w3&3)==2) {
            if (count==RSX_FP_MAX_INLINE_CONSTANTS || off+16>0x10000u) break;
            offsets[count++]=off; off+=16;
            p=resolve(context,off); if (!p) break;
        }
        if (w0&1) {
            u64 hash=rsx_fp_structural_hash(p,off,1469598103934665603ull);
            r=rsx_np_install(c,address,p,off,hash,offsets,count,branch);
            if (r) r->sampled_units=rsx_fp_texture_units(p,off);
            return r;
        }
    }
    ++c->rejected; return NULL;
}
typedef struct rsx_np_span { const u8* data; u32 size; } rsx_np_span;
static const u8* rsx_np_span_resolve(void* context, u32 bytes)
{
    const rsx_np_span* span=(const rsx_np_span*)context;
    return bytes<=span->size ? span->data : NULL;
}
static rsx_np_record* rsx_np_vp(rsx_np_cache* c, u64 address, const u8* p, u32 max_bytes)
{
    rsx_np_span span={p,max_bytes};
    rsx_np_record* r=rsx_np_bound(c,address,rsx_np_span_resolve,&span);
    if (r) return r;
    u32 instrs=rsx_vp_program_size_instrs(p,max_bytes);
    if (!instrs) { ++c->rejected; return NULL; }
    u64 hash=rsx_np_hash(p,instrs*16,1469598103934665603ull);
    r=rsx_np_install(c,address,p,instrs*16,hash,NULL,0,0);
    if (!r) return NULL;
    rsx_vp_input_analysis analysis={0xFFFFu,0};
    if (rsx_vp_analyze_inputs(p,instrs*16,&analysis)!=(int)instrs || !analysis.exact)
        analysis.input_mask=0xFFFFu;
    r->input_mask=analysis.input_mask; r->txl_mask=0;
    for (u32 i=0; i<instrs; ++i) {
        u32 d1,d2; memcpy(&d1,p+i*16+4,4); memcpy(&d2,p+i*16+8,4);
        if (((d1>>22)&31)==0x19) r->txl_mask|=1u<<((d2>>8)&3);
    }
    return r;
}
/* Exact legacy seeded hash; a program can be paired with several VPs. */
static u64 rsx_np_fp_hash(rsx_np_record* r, u64 seed)
{
    for (u32 i=0; i<8; ++i)
        if (r->combinations[i].valid && r->combinations[i].seed==seed)
            return r->combinations[i].hash;
    const u32 slot=r->next_combination++%8;
    u64 hash=rsx_fp_structural_hash(r->code,r->size,seed);
    r->combinations[slot].seed=seed; r->combinations[slot].hash=hash;
    r->combinations[slot].valid=1; return hash;
}
/* Only [0,count) is initialized/valid, just like a variable-length payload. */
static void rsx_np_constants(const rsx_np_record* r, const u8* p, rsx_fp_constant_block* out)
{
    out->count=r->constant_count;
    for (u32 i=0; i<r->constant_count; ++i)
        for (u32 j=0; j<4; ++j)
            out->values[i][j]=rsx_fp_sanitize_constant_word(rsx_fp_read_word(p+r->constants[i]+j*4));
}
static void rsx_np_destroy(rsx_np_cache* c)
{
    for (u32 i=0; i<RSX_NP_RECORDS; ++i) { free(c->records[i].code); free(c->records[i].constants); }
    memset(c,0,sizeof(*c));
}
#endif
