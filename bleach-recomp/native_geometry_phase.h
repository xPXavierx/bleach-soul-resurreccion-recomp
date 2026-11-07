#ifndef BLEACH_NATIVE_GEOMETRY_PHASE_H
#define BLEACH_NATIVE_GEOMETRY_PHASE_H
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "native_phase_result.h"
#include "native_skinning_result.h"
#include "native_geometry_resident.h"
static std::atomic<unsigned long long> bleach_phase_verified_count{0};
static bool bleach_phase_profile_enabled=false;
static uint64_t bleach_phase_profile_start=0;
struct BleachPhaseProfile {
    uint64_t count=0,native_ns=0,reference_ns=0;
    ps3_phase_result_probe sum{};
};
static BleachPhaseProfile bleach_phase_profile[2];
static std::mutex bleach_phase_profile_mutex;
static std::atomic<unsigned long long> bleach_skin_batches{0},bleach_skin_vertices{0},bleach_skin_declined{0},bleach_skin_verified{0};
static std::atomic<unsigned long long> bleach_skin_bounded{0};
static std::atomic<unsigned long long> bleach_pose_batches{0},bleach_pose_vertices{0},bleach_pose_directions{0},bleach_pose_declined{0},bleach_pose_verified{0};
static std::atomic<unsigned long long> bleach_decode_batches{0},bleach_decode_vertices{0},bleach_decode_packed{0},bleach_decode_declined{0},bleach_decode_verified{0};
static std::atomic<unsigned long long> bleach_triangle_batches{0},bleach_triangle_vertices{0},bleach_triangle_indices{0},bleach_triangle_declined{0},bleach_triangle_verified{0};
extern "C" {
int bleach_native_resident_mode=0;
unsigned bleach_native_resident_stride=0;
static std::atomic<unsigned long long> bleach_resident_jobs{0},bleach_resident_hits{0},bleach_resident_vertices{0},bleach_resident_declined{0},bleach_resident_verified{0};
void bleach_native_resident_record(unsigned vertices,int hit,int verified){
    if(!vertices){bleach_resident_declined.fetch_add(1,std::memory_order_relaxed);return;}
    bleach_resident_jobs.fetch_add(1,std::memory_order_relaxed);
    bleach_resident_vertices.fetch_add(vertices,std::memory_order_relaxed);
    if(hit)bleach_resident_hits.fetch_add(1,std::memory_order_relaxed);
    if(verified)bleach_resident_verified.fetch_add(1,std::memory_order_relaxed);
}
int bleach_native_triangle_mode=0;
unsigned bleach_native_triangle_stride=0;
void bleach_native_triangle_record(unsigned vertices,unsigned indices,int verified) {
    if(!vertices){bleach_triangle_declined.fetch_add(1,std::memory_order_relaxed);return;}
    bleach_triangle_batches.fetch_add(1,std::memory_order_relaxed);
    bleach_triangle_vertices.fetch_add(vertices,std::memory_order_relaxed);
    bleach_triangle_indices.fetch_add(indices,std::memory_order_relaxed);
    if(verified)bleach_triangle_verified.fetch_add(1,std::memory_order_relaxed);
}
int bleach_native_pose_mode=0;
unsigned bleach_native_pose_stride=0;
void bleach_native_pose_record(unsigned vertices,unsigned directions,int verified) {
    if(!vertices){bleach_pose_declined.fetch_add(1,std::memory_order_relaxed);return;}
    bleach_pose_batches.fetch_add(1,std::memory_order_relaxed);
    bleach_pose_vertices.fetch_add(vertices,std::memory_order_relaxed);
    if(directions)bleach_pose_directions.fetch_add(1,std::memory_order_relaxed);
    if(verified)bleach_pose_verified.fetch_add(1,std::memory_order_relaxed);
}
int bleach_native_decode_mode=0;
unsigned bleach_native_decode_stride=0;
void bleach_native_decode_record(unsigned vertices,unsigned packed,int verified) {
    if(!vertices){bleach_decode_declined.fetch_add(1,std::memory_order_relaxed);return;}
    bleach_decode_batches.fetch_add(1,std::memory_order_relaxed);
    bleach_decode_vertices.fetch_add(vertices,std::memory_order_relaxed);
    if(packed)bleach_decode_packed.fetch_add(1,std::memory_order_relaxed);
    if(verified)bleach_decode_verified.fetch_add(1,std::memory_order_relaxed);
}
int bleach_native_skinning_mode=0;
unsigned bleach_native_skinning_stride=0;
void bleach_native_skinning_record(unsigned bulk,int bounded) {
    if(bounded)bleach_skin_bounded.fetch_add(1,std::memory_order_relaxed);
    if(bulk){bleach_skin_batches.fetch_add(1,std::memory_order_relaxed);bleach_skin_vertices.fetch_add(bulk,std::memory_order_relaxed);}
    else bleach_skin_declined.fetch_add(1,std::memory_order_relaxed);
}
void bleach_native_skinning_verified(void){bleach_skin_verified.fetch_add(1,std::memory_order_relaxed);}
int bleach_native_phase_mode=0;
unsigned bleach_native_phase_stride=0;
void bleach_native_phase_verified(void) {bleach_phase_verified_count.fetch_add(1,std::memory_order_relaxed);}
int bleach_native_phase_profile_active(void) {
    if(!bleach_phase_profile_enabled)return 0;
    uint64_t elapsed=ps3_phase_result_clock_ns()-bleach_phase_profile_start;
    return elapsed>=UINT64_C(120000000000) && elapsed<UINT64_C(138000000000);
}
void bleach_native_phase_profile_sample(const ps3_phase_result_probe* p,uint64_t native_ns,uint64_t reference_ns) {
    if(!p->valid)return;
    std::lock_guard<std::mutex> lock(bleach_phase_profile_mutex);
    auto& a=bleach_phase_profile[p->hit?1:0];++a.count;a.native_ns+=native_ns;a.reference_ns+=reference_ns;
#define ADD(field) a.sum.field+=p->field
    ADD(lookup_ns);ADD(setup_ns);ADD(compute_ns);ADD(collect_ns);ADD(publish_ns);ADD(apply_ns);
    ADD(total_ns);ADD(input_bytes);ADD(output_bytes);
#undef ADD
}
}
static void bleach_phase_report()
{
    if(bleach_native_resident_mode){ps3_geo_resident_stats s;ps3_geo_resident_get_stats(&s);
        fprintf(stderr,"[native-resident] jobs=%llu hits=%llu vertices=%llu declined=%llu verified=%llu store_hits=%llu misses=%llu store_declined=%llu published=%llu evicted=%llu input_bytes=%llu output_bytes=%llu resident_bytes=%llu\n",
            bleach_resident_jobs.load(),bleach_resident_hits.load(),bleach_resident_vertices.load(),bleach_resident_declined.load(),bleach_resident_verified.load(),
            (unsigned long long)s.hits,(unsigned long long)s.misses,(unsigned long long)s.declined,(unsigned long long)s.published,
            (unsigned long long)s.evicted,(unsigned long long)s.input_bytes,(unsigned long long)s.output_bytes,(unsigned long long)s.bytes);}
    fprintf(stderr,"[native-triangle] enabled=%d batches=%llu vertices=%llu input_indices=%llu declined=%llu verified=%llu\n",
        bleach_native_triangle_mode,bleach_triangle_batches.load(),bleach_triangle_vertices.load(),bleach_triangle_indices.load(),bleach_triangle_declined.load(),bleach_triangle_verified.load());
    fprintf(stderr,"[native-pose] enabled=%d batches=%llu vertices=%llu directions_batches=%llu declined=%llu verified=%llu\n",
        bleach_native_pose_mode,bleach_pose_batches.load(),bleach_pose_vertices.load(),bleach_pose_directions.load(),bleach_pose_declined.load(),bleach_pose_verified.load());
    fprintf(stderr,"[native-decode] enabled=%d batches=%llu vertices=%llu packed_batches=%llu declined=%llu verified=%llu\n",
        bleach_native_decode_mode,bleach_decode_batches.load(),bleach_decode_vertices.load(),bleach_decode_packed.load(),bleach_decode_declined.load(),bleach_decode_verified.load());
    fprintf(stderr,"[native-skin4] enabled=%d batches=%llu vertices=%llu declined=%llu verified=%llu bounded=%llu\n",
        bleach_native_skinning_mode,bleach_skin_batches.load(),bleach_skin_vertices.load(),bleach_skin_declined.load(),bleach_skin_verified.load(),bleach_skin_bounded.load());
    if(bleach_native_skinning_mode==3){ps3_skin4_result_counters c;ps3_skin4_result_stats(&c);
        fprintf(stderr,"[native-skin4-results] hits=%llu misses=%llu stored=%llu declined=%llu evicted=%llu allocations=%llu bytes=%llu inflight=%llu hit_vertices=%llu miss_vertices=%llu\n",
            (unsigned long long)c.hits,(unsigned long long)c.misses,(unsigned long long)c.stored,
            (unsigned long long)c.declined,(unsigned long long)c.evicted,(unsigned long long)c.allocations,
            (unsigned long long)c.bytes,(unsigned long long)c.inflight,(unsigned long long)c.hit_vertices,(unsigned long long)c.miss_vertices);}
    fprintf(stderr,"[native-phase] enabled=%d verify_stride=%u verified=%llu\n",
        bleach_native_phase_mode,bleach_native_phase_stride,bleach_phase_verified_count.load());
    if(bleach_native_phase_mode==2){ps3_phase_result_counters c;ps3_phase_result_stats(&c);
        fprintf(stderr,"[native-phase-results] hits=%llu misses=%llu stored=%llu declined=%llu evicted=%llu bytes=%llu\n",
            (unsigned long long)c.hits,(unsigned long long)c.misses,(unsigned long long)c.stored,
            (unsigned long long)c.declined,(unsigned long long)c.evicted,(unsigned long long)c.bytes);}
    if(bleach_phase_profile_enabled){std::lock_guard<std::mutex> lock(bleach_phase_profile_mutex);
        for(unsigned h=0;h<2;h++){const auto& a=bleach_phase_profile[h];const auto& p=a.sum;
            fprintf(stderr,"[native-phase-profile] hit=%u count=%llu native_ns=%llu reference_ns=%llu lookup_ns=%llu setup_ns=%llu compute_ns=%llu collect_ns=%llu publish_ns=%llu apply_ns=%llu total_ns=%llu input_bytes=%llu output_bytes=%llu after=120 before=138\n",
                h,(unsigned long long)a.count,(unsigned long long)a.native_ns,(unsigned long long)a.reference_ns,
                (unsigned long long)p.lookup_ns,(unsigned long long)p.setup_ns,(unsigned long long)p.compute_ns,
                (unsigned long long)p.collect_ns,(unsigned long long)p.publish_ns,(unsigned long long)p.apply_ns,
                (unsigned long long)p.total_ns,(unsigned long long)p.input_bytes,(unsigned long long)p.output_bytes);}}
}
static void bleach_phase_initialize()
{
    const char* resident=getenv("BLEACH_NATIVE_RESIDENT_GEOMETRY");
    bleach_native_resident_mode=resident && !strcmp(resident,"1");
    const char* resident_verify=getenv("BLEACH_NATIVE_RESIDENT_VERIFY");
    if(resident_verify){char* end=nullptr;unsigned long n=strtoul(resident_verify,&end,10);
        if(!*resident_verify || *end || n>1000000)abort();bleach_native_resident_stride=(unsigned)n;}
    const char* triangle=getenv("BLEACH_NATIVE_TRIANGLES");
    bleach_native_triangle_mode=triangle && !strcmp(triangle,"1");
    const char* triangle_verify=getenv("BLEACH_NATIVE_TRIANGLES_VERIFY");
    if(triangle_verify){char* end=nullptr;unsigned long n=strtoul(triangle_verify,&end,10);
        if(!*triangle_verify || *end || n>1000000)abort();bleach_native_triangle_stride=(unsigned)n;}
    const char* pose=getenv("BLEACH_NATIVE_POSE");
    bleach_native_pose_mode=pose && !strcmp(pose,"1");
    const char* pose_verify=getenv("BLEACH_NATIVE_POSE_VERIFY");
    if(pose_verify){char* end=nullptr;unsigned long n=strtoul(pose_verify,&end,10);
        if(!*pose_verify || *end || n>1000000)abort();bleach_native_pose_stride=(unsigned)n;}
    const char* decode=getenv("BLEACH_NATIVE_DECODE");
    bleach_native_decode_mode=decode && !strcmp(decode,"1");
    const char* decode_verify=getenv("BLEACH_NATIVE_DECODE_VERIFY");
    if(decode_verify){char* end=nullptr;unsigned long n=strtoul(decode_verify,&end,10);
        if(!*decode_verify || *end || n>1000000)abort();bleach_native_decode_stride=(unsigned)n;}
    const char* skin=getenv("BLEACH_NATIVE_SKINNING");
    bleach_native_skinning_mode=skin && !strcmp(skin,"3")?3:(skin && !strcmp(skin,"2")?2:(skin && !strcmp(skin,"1")));
    const char* skin_verify=getenv("BLEACH_NATIVE_SKINNING_VERIFY");
    if(skin_verify){char* end=nullptr;unsigned long n=strtoul(skin_verify,&end,10);
        if(!*skin_verify || *end || n>1000000)abort();bleach_native_skinning_stride=(unsigned)n;}
    const char* mode=getenv("BLEACH_NATIVE_PHASES");
    bleach_native_phase_mode=mode && !strcmp(mode,"2")?2:(mode && !strcmp(mode,"1"));
    const char* stride=getenv("BLEACH_NATIVE_PHASE_VERIFY");
    if(stride) {
        char* end=nullptr;unsigned long value=strtoul(stride,&end,10);
        if(!*stride || *end || value>1000000)abort();
        bleach_native_phase_stride=static_cast<unsigned>(value);
    }
    const char* profile=getenv("BLEACH_NATIVE_PHASE_PROFILE");
    bleach_phase_profile_enabled=profile && !strcmp(profile,"1");
    if(bleach_phase_profile_enabled && (bleach_native_phase_mode!=2 || !bleach_native_phase_stride))abort();
    bleach_phase_profile_start=ps3_phase_result_clock_ns();
    atexit(bleach_phase_report);
}
#endif
