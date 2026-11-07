#include "rsx_geometry_handoff.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* State and generation share a CAS word: an old cancellation/claim cannot
 * release or acquire a later occupant of the same slot (including ABA). */
typedef struct slot { atomic_uint_fast64_t state; rsx_geometry_lease lease; } slot;
static slot* slots;
static unsigned char* storage;
static unsigned slot_count;
static size_t slot_bytes;
static atomic_uint_fast64_t serial=1;
static atomic_uint cursor;
static rsx_geometry_lease pending;
static atomic_uint_fast64_t claimed,matched,mismatched,abandoned;
static slot* locate(uint64_t token) {
    unsigned i=(unsigned)(token&1023);
    return slots && i && i<=slot_count ? &slots[i-1] : NULL;
}
int rsx_geometry_handoff_init(unsigned count,size_t bytes) {
    if(slots || !count || count>1023 || !bytes || bytes>SIZE_MAX/count)return 0;
    slot* s=calloc(count,sizeof(*s));unsigned char* p=malloc(count*bytes);
    if(!s || !p){free(s);free(p);return 0;}
    for(unsigned i=0;i<count;i++)atomic_init(&s[i].state,0);
    slots=s;storage=p;slot_count=count;slot_bytes=bytes;
    atomic_store(&cursor,0);return 1;
}
void rsx_geometry_handoff_destroy(void) {
    free(storage);free(slots);slots=NULL;storage=NULL;slot_count=0;
    memset(&pending,0,sizeof pending);
    /* Serial deliberately survives reinitialization. */
}
rsx_geometry_lease rsx_geometry_handoff_reserve(void) {
    rsx_geometry_lease empty={0};if(!slots)return empty;
    unsigned first=atomic_fetch_add_explicit(&cursor,1,memory_order_relaxed);
    for(unsigned k=0;k<slot_count;k++) {
        unsigned i=(first+k)%slot_count;
        uint64_t n=atomic_fetch_add_explicit(&serial,1,memory_order_relaxed);
        if(n>=(UINT64_C(1)<<52))return empty; /* fail closed, never wrap */
        uint64_t token=(n<<10)|(i+1),expected=0;
        if(!atomic_compare_exchange_strong_explicit(&slots[i].state,&expected,
            (token<<2)|1,memory_order_acquire,memory_order_relaxed))continue;
        rsx_geometry_lease lease={0};lease.token=token;
        lease.data=storage+(size_t)i*slot_bytes;lease.capacity=slot_bytes;
        return lease;
    }
    return empty; /* bounded backpressure: caller keeps the CPU path */
}
int rsx_geometry_handoff_publish(const rsx_geometry_lease* lease) {
    if(!lease)return 0;slot* s=locate(lease->token);
    if(!s || !lease->command_ea || !lease->bytes || lease->bytes>slot_bytes ||
       lease->capacity!=slot_bytes || lease->data!=storage+(s-slots)*slot_bytes ||
       atomic_load_explicit(&s->state,memory_order_relaxed)!=((lease->token<<2)|1))return 0;
    s->lease=*lease;
    atomic_store_explicit(&s->state,(lease->token<<2)|2,memory_order_release);
    return 1;
}
int rsx_geometry_handoff_claim(uint64_t token,uint32_t ea,rsx_geometry_lease* out) {
    slot* s=locate(token);uint64_t expected=(token<<2)|2;
    if(!s || !out || !atomic_compare_exchange_strong_explicit(&s->state,&expected,
        (token<<2)|3,memory_order_acquire,memory_order_relaxed))return 0;
    if(s->lease.command_ea!=ea) {
        atomic_store_explicit(&s->state,(token<<2)|2,memory_order_release);return 0;
    }
    *out=s->lease;return 1;
}
void rsx_geometry_handoff_release(uint64_t token) {
    slot* s=locate(token);if(!s)return;
    uint64_t expected=(token<<2)|3;
    atomic_compare_exchange_strong_explicit(&s->state,&expected,0,
        memory_order_release,memory_order_relaxed);
}
void rsx_geometry_handoff_cancel(uint64_t token) {
    slot* s=locate(token);if(!s)return;
    uint64_t expected=(token<<2)|1;
    atomic_compare_exchange_strong_explicit(&s->state,&expected,0,
        memory_order_release,memory_order_relaxed);
}
void rsx_geometry_handoff_marker(uint64_t token,uint32_t ea) {
    if(pending.token)atomic_fetch_add(&abandoned,1);
    rsx_geometry_handoff_release(pending.token);memset(&pending,0,sizeof pending);
    if(rsx_geometry_handoff_claim(token,ea,&pending))atomic_fetch_add(&claimed,1);
}
rsx_geometry_lease rsx_geometry_handoff_take(void) {
    rsx_geometry_lease result=pending;memset(&pending,0,sizeof pending);return result;
}
int rsx_geometry_handoff_command(uint32_t header,uint32_t ea,const void* args) {
    if(header!=RSX_GEOMETRY_MARKER_HEADER || !args)return 0;
    const unsigned char* p=args;uint32_t words[3];
    for(unsigned i=0;i<3;i++)words[i]=(uint32_t)p[i*4]<<24|(uint32_t)p[i*4+1]<<16|
        (uint32_t)p[i*4+2]<<8|p[i*4+3];
    if(words[0]!=RSX_GEOMETRY_MARKER_MAGIC)return 0;
    rsx_geometry_handoff_marker((uint64_t)words[1]<<32|words[2],ea);return 1;
}
void rsx_geometry_handoff_shadow_check(const rsx_geometry_lease* lease,uint32_t primitive,uint32_t indices,int contiguous) {
    if(!lease || !lease->token)return;
    if(primitive==5 && contiguous && indices==lease->selected_indices)
        atomic_fetch_add(&matched,1);
    else atomic_fetch_add(&mismatched,1);
    rsx_geometry_handoff_release(lease->token);
    if(((atomic_load(&matched)+atomic_load(&mismatched))&1023)==0)rsx_geometry_handoff_report();
}
void rsx_geometry_handoff_report(void) {
    if(!slots)return;
    unsigned writing=0,ready=0,reading=0;
    for(unsigned i=0;i<slot_count;i++)switch(atomic_load(&slots[i].state)&3) {
        case 1:writing++;break;case 2:ready++;break;case 3:reading++;break;
    }
    fprintf(stderr,"[geometry-handoff] claimed=%llu matched=%llu mismatched=%llu abandoned=%llu writing=%u ready=%u reading=%u\n",
        (unsigned long long)atomic_load(&claimed),(unsigned long long)atomic_load(&matched),
        (unsigned long long)atomic_load(&mismatched),(unsigned long long)atomic_load(&abandoned),writing,ready,reading);
}
