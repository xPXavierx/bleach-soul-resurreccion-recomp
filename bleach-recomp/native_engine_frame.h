#ifndef BLEACH_NATIVE_ENGINE_FRAME_H
#define BLEACH_NATIVE_ENGINE_FRAME_H
#include "native_engine_schedule.h"
#include "rsx_aspect.h"
#ifdef _WIN32
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/* Experimental ownership split. The guest's independent 60-Hz clock and
 * actual GPU completion semaphores remain intact. Only native-tagged scene
 * submissions leave the guest's vblank deadline domain. This is not yet an
 * interpolated high-FPS mode: pose interpolation remains a separate stage. */
static std::atomic<uint64_t> bleach_native_scenes{0},bleach_native_updates{0},
    bleach_native_completions{0},bleach_native_position_changes{0},
    bleach_native_accepted{0},bleach_native_rejected{0};
static thread_local bool bleach_engine_active=false,bleach_engine_update=true;
static std::atomic<uint64_t> bleach_engine_execution_epoch{0};
extern "C" uint64_t bleach_engine_audit_token() {
    return bleach_engine_execution_epoch.load(std::memory_order_acquire);
}
static thread_local std::unique_ptr<BleachEngineDispatchSchedule> bleach_engine_dispatch;
static std::atomic<HANDLE> bleach_engine_buffer_event{nullptr};
static std::atomic<uint64_t> bleach_engine_buffer_releases{0};
static std::atomic<uint64_t> bleach_camera_updates{0},bleach_camera_reuses{0};
static thread_local uint32_t bleach_camera_prepared=0;
static bool bleach_engine_reuse_camera(uint32_t site,uint32_t target,uint32_t object) {
    static const bool enabled=[] {const char* v=getenv("BLEACH_NATIVE_CAMERA");return v&&!strcmp(v,"1");}();
    if(!enabled||!bleach_engine_active||site!=0x0027b888||target!=0x00061e30)return false;
    // This callback advances camera collision/transition state (including its
    // frame counter at +0x130), then publishes persistent view matrices. It is
    // a simulation phase despite living in the original prepare traversal.
    // Rendering between ticks reuses those matrices; no interpolation yet.
    if(bleach_engine_update||bleach_camera_prepared!=object) {
        bleach_camera_prepared=object;++bleach_camera_updates;return false;
    }
    ++bleach_camera_reuses;return true;
}
static bool bleach_engine_buffer_ready() {
    const uint32_t graphics=vm_read32(0x004ae670u-0x3c38u);
    if(!graphics)return false;
    const unsigned index=vm_read8(graphics+0x4400);
    if(index>1)return false;
    // func_00248E24 accepts a slot only when its busy byte is ZERO.
    return vm_read8(graphics+0x4480+index*0x138900)==0;
}
/* Timeline marks for the two-slot command buffer: 20 = acquisition attempt
 * (a = accepted, b = slot index), 21 = release (a = busy-byte address). */
enum { BLEACH_FT_SLOT_ACQUIRE = 20, BLEACH_FT_SLOT_RELEASE = 21 };
extern "C" void bleach_engine_buffer_released(uint32_t address) {
    if(PS3_FT_ON()){const uint64_t t=ps3_ft_now();ps3_ft_span(BLEACH_FT_SLOT_RELEASE,0,address,0,t,t);}
    HANDLE event=bleach_engine_buffer_event.load(std::memory_order_acquire);
    if(event){++bleach_engine_buffer_releases;SetEvent(event);}
}
/* Executed-tick index of the latest actor update (task vtable+8 func_000A1770). */
static std::atomic<uint64_t> bleach_engine_actor_tick{~0ull};
static bool bleach_engine_gameplay() {
    const uint64_t seen=bleach_engine_actor_tick.load(std::memory_order_relaxed);
    return seen!=~0ull&&bleach_native_updates.load(std::memory_order_relaxed)<=seen+1;
}
/* Simulation ticks per original 30 Hz update, latched at each frame start: Hz/30, or 1
 * where BLEACH_SIM_GAMEPLAY_ONLY keeps a screen at 30 Hz. */
static int bleach_tick_ratio_now=1;
static void bleach_camera_rate(int ratio);
static thread_local uint32_t bleach_engine_actors[128]={};
static thread_local uint64_t bleach_engine_positions;
static const bool bleach_engine_audit=getenv("BLEACH_FRAME_OWNERSHIP")!=nullptr;
static constexpr unsigned bleach_actor_words=0x1600/4;
struct BleachActorSnapshot {uint32_t object,words[bleach_actor_words];};
static thread_local BleachActorSnapshot bleach_actor_before[128];
static std::atomic<uint64_t> bleach_actor_changes[bleach_actor_words];
static std::atomic<unsigned> bleach_actor_change_details{0};
static uint64_t bleach_engine_position_hash() {
    uint64_t h=1469598103934665603ull;
    for(uint32_t actor:bleach_engine_actors)if(actor)
        for(unsigned off: {0x104u,0x108u,0x10cu,0x140u,0x144u,0x148u})
            h=(h^vm_read32(actor+off))*1099511628211ull;
    return h;
}
/* BLEACH_TASK_DUMP=<hex update fn>[,...]: objects of these scene tasks are added to the
 * BLEACH_ACTOR_DUMP records (after the actors), to find per-tick clocks outside actors. */
static thread_local uint32_t bleach_dump_extra[256]={};
static void bleach_note_dump_extra(uint32_t target,uint32_t object,uint32_t mode) {
    static const std::vector<uint32_t> targets=[] {
        std::vector<uint32_t> out;
        for(const char* p=getenv("BLEACH_TASK_DUMP");p&&*p;) {
            char* end=nullptr; const uint32_t v=(uint32_t)strtoul(p,&end,16);
            if(end==p)break; out.push_back(v); p=*end==','?end+1:end;
        }
        return out;
    }();
    if(targets.empty())return;
    bool listed=false; for(uint32_t t:targets)if(t==target)listed=true;
    if(!listed)return;
    for(auto& o:bleach_dump_extra)if(o==object){if(mode==1)o=0;return;}
    if(mode!=1)for(auto& o:bleach_dump_extra)if(!o){o=object;return;}
}
static void bleach_engine_note_actor(uint32_t target,uint32_t object,uint32_t mode) {
    if(target==0x615d0&&(mode==0||mode==1)&&bleach_camera_prepared==object)bleach_camera_prepared=0;
    if(bleach_engine_active)bleach_note_dump_extra(target,object,mode);
    if(!bleach_engine_active||target!=0xa1770)return;
    for(auto& actor:bleach_engine_actors)if(actor==object){if(mode==1)actor=0;return;}
    if(mode!=1)for(auto& actor:bleach_engine_actors)if(!actor){actor=object;return;}
}
static void bleach_engine_summary() {
    fprintf(stderr,"[native-engine-summary] scene_builds=%llu simulation_ticks=%llu completed_native_deadlines=%llu render_only_position_changes=%llu accepted=%llu rejected=%llu buffer_releases=%llu interpolation=not_implemented\n",
        (unsigned long long)bleach_native_scenes.load(),(unsigned long long)bleach_native_updates.load(),
        (unsigned long long)bleach_native_completions.load(),(unsigned long long)bleach_native_position_changes.load(),
        (unsigned long long)bleach_native_accepted.load(),(unsigned long long)bleach_native_rejected.load(),
        (unsigned long long)bleach_engine_buffer_releases.load());
    if(bleach_engine_audit)for(unsigned word=0;word<bleach_actor_words;word++) {
        auto count=bleach_actor_changes[word].load();
        if(count)fprintf(stderr,"[native-engine-actor-write] offset=%04x render_only_frames=%llu\n",
            word*4,(unsigned long long)count);
    }
    fprintf(stderr,"[native-camera-summary] simulation_prepares=%llu render_reuses=%llu interpolation=not_implemented\n",
        (unsigned long long)bleach_camera_updates.load(),(unsigned long long)bleach_camera_reuses.load());
}
extern "C" int bleach_engine_frame_begin(ppu_context* ctx) {
    static const unsigned rate=[] {
        const char* e=getenv("BLEACH_EXPERIMENTAL_RENDER_HZ");
        unsigned r=e&&!strcmp(e,"unlimited")?UINT32_MAX:(e?atoi(e):0);
        // Whole frames per 30 Hz simulation tick only: both clocks share one
        // origin, so every (r/30)th render deadline lands exactly on a tick and
        // each game state is shown for the same number of frames.
        if(r!=UINT32_MAX&&(r<30||r>240||r%30))return 0u;
        atexit(bleach_engine_summary);return r;
    }();
    // Opt-in until long stalls observed in run116 have been resolved. The
    // unlimited experiment necessarily uses the buffer-driven dispatcher.
    static const bool backpressure=[] {const char* e=getenv("BLEACH_NATIVE_BACKPRESSURE");return e&&strcmp(e,"0");}();
    static const uint64_t after=[] {const char* e=getenv("BLEACH_EXPERIMENTAL_AFTER");return e?strtoull(e,nullptr,10):110ull;}();
    static const ULONGLONG origin=GetTickCount64();
    static thread_local std::unique_ptr<BleachEngineSchedule> schedule;
    const uint32_t game=vm_read32(0x004ae670u-0x7e84u);
    bleach_engine_active=rate&&GetTickCount64()-origin>=after*1000&&game&&vm_read8(game+0x11);
    if(!bleach_engine_active){schedule.reset();bleach_engine_dispatch.reset();bleach_camera_prepared=0;memset(bleach_engine_actors,0,sizeof(bleach_engine_actors));return 0;}
    LARGE_INTEGER now,frequency;QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
    const bool driven=backpressure||rate==UINT32_MAX;
    if(driven&&!bleach_engine_dispatch) {
        bleach_engine_dispatch.reset(new BleachEngineDispatchSchedule(now.QuadPart,frequency.QuadPart,rate==UINT32_MAX?0:rate));
        if(!bleach_engine_buffer_event.load()) {
            HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(!event)abort();
            bleach_engine_buffer_event.store(event,std::memory_order_release);
        }
        fprintf(stderr,"[native-engine] experimental buffer-driven render=%uHz (0=unlimited) simulation=%uHz; original vblank=60Hz\n",
            rate==UINT32_MAX?0u:rate,bleach_simulation_hz());
    }
    if(!driven&&!schedule) {
        schedule.reset(new BleachEngineSchedule(now.QuadPart,frequency.QuadPart,rate));
        fprintf(stderr,"[native-engine] experimental render=%uHz simulation=30Hz; original vblank=60Hz; interpolation pending\n",rate);
    }
    static thread_local HANDLE timer=[] {
        HANDLE h=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        return h?h:CreateWaitableTimerW(nullptr,FALSE,nullptr);
    }();
    if(!timer)abort();
    const uint64_t trace_begin=PS3_FT_ON()?ps3_ft_now():0;
    // BLEACH_MENU_RENDER_TICK=1: outside gameplay (loading, story and pause screens)
    // scene builds happen only on executed simulation ticks. Their 2D animations
    // (the loading icon) advance once per built frame, as on the PS3 at 30 FPS.
    static const bool menu_render_tick=[] {const char* e=getenv("BLEACH_MENU_RENDER_TICK");return e&&!strcmp(e,"1");}();
    static const bool gameplay_only=[] {const char* e=getenv("BLEACH_SIM_GAMEPLAY_ONLY");return e&&!strcmp(e,"1");}();
    static uint64_t pulses=0;
    const unsigned ratio=bleach_simulation_hz()/30;
    for(;;) {
        const bool ready=driven&&bleach_engine_buffer_ready();
        BleachClockDeadline& simulation=driven?bleach_engine_dispatch->simulation:schedule->simulation;
        uint64_t deadline;
        if(menu_render_tick&&!bleach_engine_gameplay()) {
            if((uint64_t)now.QuadPart>=simulation.deadline) {
                // A pulse that BLEACH_SIM_GAMEPLAY_ONLY would not execute builds nothing.
                if(gameplay_only&&ratio>=2&&pulses%ratio){++pulses;simulation.advance();continue;}
                break;
            }
            deadline=simulation.deadline;
        } else {
            if(driven?bleach_engine_dispatch->runnable(now.QuadPart,ready):(uint64_t)now.QuadPart>=schedule->render.deadline)break;
            deadline=driven?bleach_engine_dispatch->wake_deadline(ready):schedule->render.deadline;
        }
        uint64_t left=deadline-now.QuadPart;
        // BLEACH_LIMITER_SPIN_US (default 2000; 0 = timer only): a capped rate sleeps on the
        // high-resolution timer until this close to the deadline, then spins on QPC. The timer
        // alone wakes up to ~1 ms late and unevenly; the spin lands each frame start on the
        // grid to a few microseconds (the RTSS method).
        static const uint64_t spin=[&] {const char* e=getenv("BLEACH_LIMITER_SPIN_US");
            return (e?strtoull(e,nullptr,10):2000ull)*(uint64_t)frequency.QuadPart/1000000ull;}();
        if(spin&&rate!=UINT32_MAX&&left<=spin) {
            while((uint64_t)now.QuadPart<deadline){YieldProcessor();QueryPerformanceCounter(&now);}
            continue;
        }
        if(spin&&rate!=UINT32_MAX)left-=spin;
        LARGE_INTEGER due;due.QuadPart=-(LONGLONG)((left*10000000ull+frequency.QuadPart-1)/frequency.QuadPart);
        if(!SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))abort();
        HANDLE waits[]={timer,bleach_engine_buffer_event.load()};
        DWORD result=WaitForMultipleObjects(driven?2:1,waits,FALSE,INFINITE);
        if(result!=WAIT_OBJECT_0&&result!=WAIT_OBJECT_0+1)abort();
        QueryPerformanceCounter(&now);
    }
    if(trace_begin)ps3_ft_usleep((uint32_t)ctx->thread_id,0x0001ce58,trace_begin,ps3_ft_now());
    // A debugger/suspend gap pauses this experimental domain, never runs a
    // burst of minutes of stale gameplay. Ordinary timing debt is retained.
    const uint64_t simulation_deadline=driven?bleach_engine_dispatch->simulation.deadline:schedule->simulation.deadline;
    if((uint64_t)now.QuadPart>simulation_deadline+(uint64_t)frequency.QuadPart*4) {
        if(driven)bleach_engine_dispatch.reset(new BleachEngineDispatchSchedule(now.QuadPart,frequency.QuadPart,rate==UINT32_MAX?0:rate));
        else schedule.reset(new BleachEngineSchedule(now.QuadPart,frequency.QuadPart,rate));
    }
    bleach_engine_update=driven?bleach_engine_dispatch->begin(now.QuadPart):schedule->begin(now.QuadPart);
    // BLEACH_SIM_GAMEPLAY_ONLY=1 (with BLEACH_SIMULATION_HZ above 30): outside
    // gameplay (no actor update in the last executed tick: menus, loading, story
    // screens, pause) only every (Hz/30)th tick runs, so those screens keep 30 Hz.
    {
        if(bleach_engine_update&&gameplay_only&&ratio>=2&&(pulses++%ratio)&&!bleach_engine_gameplay())
            bleach_engine_update=false;
    }
    ++bleach_native_scenes;if(bleach_engine_update)++bleach_native_updates;
    if(bleach_engine_update) {
        const int before=bleach_tick_ratio_now;
        bleach_tick_ratio_now=gameplay_only&&!bleach_engine_gameplay()?1:(ratio<1?1:(int)ratio);
        if(before!=bleach_tick_ratio_now)   // screen transitions only; ms is GetTickCount64
            fprintf(stderr,"[sim-ratio] %d -> %d tick=%llu ms=%llu\n",before,bleach_tick_ratio_now,
                (unsigned long long)bleach_native_updates.load(),(unsigned long long)GetTickCount64());
        bleach_camera_rate(bleach_tick_ratio_now);
    }
    // 22 = scene build begins (a = simulation update, b = buffer was ready).
    if(PS3_FT_ON()){const uint64_t t=ps3_ft_now();ps3_ft_span(22,0,bleach_engine_update,driven&&bleach_engine_buffer_ready(),t,t);}
    static const bool input_audit=getenv("BLEACH_GEOMETRY_INPUT_AUDIT")!=nullptr;
    if(input_audit)
        bleach_engine_execution_epoch.store((bleach_native_scenes.load()<<32)|
            (bleach_native_updates.load()<<1)|uint64_t(bleach_engine_update),std::memory_order_release);
    bleach_engine_positions=bleach_engine_position_hash();
    if(bleach_engine_audit&&!bleach_engine_update)for(unsigned i=0;i<128;i++) {
        auto& snapshot=bleach_actor_before[i];snapshot.object=bleach_engine_actors[i];
        if(snapshot.object)for(unsigned word=0;word<bleach_actor_words;word++)
            snapshot.words[word]=vm_read32(snapshot.object+word*4);
    }
    const uint64_t frames=driven?bleach_engine_dispatch->frames:schedule->frames;
    const uint64_t ticks=driven?bleach_engine_dispatch->ticks:schedule->ticks;
    const uint64_t skipped=driven?bleach_engine_dispatch->skipped_render_slots:schedule->skipped_render_slots;
    if(frames%240==0)
        fprintf(stderr,"[native-engine] scenes=%llu updates=%llu skipped_render_slots=%llu game_counter=%u\n",
            (unsigned long long)frames,(unsigned long long)ticks,
            (unsigned long long)skipped,vm_read32(game+0x14));
    (void)ctx;return bleach_engine_update?2:1;
}
/* BLEACH_ACTOR_TRACE=<start s>:<path>: for 15 s, one line per simulation tick
 * with each tracked actor's candidate position words (+0x104/108/10C, +0x140/
 * 144/148) as floats -- to identify the player and measure game speed. */
static void bleach_actor_trace() {
    static FILE* out=nullptr; static double start=-1;
    if(start<0) {
        start=1e30; const char* e=getenv("BLEACH_ACTOR_TRACE"); char path[512];
        if(e && sscanf(e,"%lf:%511[^\n]",&start,path)==2) out=fopen(path,"w");
        else start=1e30;
    }
    if(!out) return;
    // Seconds since process creation: the engine becomes active long after boot.
    FILETIME created,exited,kernel,user,now_ft; GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
    GetSystemTimeAsFileTime(&now_ft);
    const double t=((double)(((uint64_t)now_ft.dwHighDateTime<<32)|now_ft.dwLowDateTime)-
                    (double)(((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime))/1e7;
    if(t<start) return;
    if(t>start+15) { fclose(out); out=nullptr; return; }
    fprintf(out,"%.4f",t);
    for(uint32_t actor:bleach_engine_actors) if(actor) {
        fprintf(out," %08x",actor);
        for(unsigned off: {0x104u,0x10cu,0x128u,0x12cu,0x130u,0x24cu,0x250u}) {
            uint32_t bits=vm_read32(actor+off); float v; memcpy(&v,&bits,4); fprintf(out,":%g",v);
        }
    }
    fprintf(out,"\n"); fflush(out);
}
/* BLEACH_ACTOR_DUMP=<start s>:<hex actor>:<path>: for 15 s, per simulation tick,
 * the raw (big-endian) 0x1800 bytes of every tracked actor, plus 0x200 bytes behind
 * each heap-looking pointer word of <hex actor> -- to find animation clocks offline.
 * <path>.vt lists each actor's vtable (entry -> function) once.
 * BLEACH_ACTOR_DUMP_BYTES (default 0x1800) and _SECONDS (default 15) shrink a dump;
 * BLEACH_ACTOR_DUMP_EXTRA_ONLY=1 records only the BLEACH_TASK_DUMP objects. */
static void bleach_actor_dump() {
    static const unsigned bytes=[] {const char* e=getenv("BLEACH_ACTOR_DUMP_BYTES");return e?(unsigned)strtoul(e,nullptr,0):0x1800u;}();
    static const double seconds=[] {const char* e=getenv("BLEACH_ACTOR_DUMP_SECONDS");return e?atof(e):15.0;}();
    static const bool extra_only=[] {const char* e=getenv("BLEACH_ACTOR_DUMP_EXTRA_ONLY");return e&&!strcmp(e,"1");}();
    static FILE* out=nullptr; static FILE* vt=nullptr; static double start=-1; static uint32_t focus=0;
    static uint32_t seen[512]={};
    if(start<0) {
        start=1e30; const char* e=getenv("BLEACH_ACTOR_DUMP"); char path[512];
        if(e && sscanf(e,"%lf:%x:%511[^\n]",&start,&focus,path)==3) {
            out=fopen(path,"wb"); strcat(path,".vt"); vt=fopen(path,"w");
        } else start=1e30;
    }
    if(!out) return;
    FILETIME created,exited,kernel,user,now_ft; GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
    GetSystemTimeAsFileTime(&now_ft);
    const double t=((double)(((uint64_t)now_ft.dwHighDateTime<<32)|now_ft.dwLowDateTime)-
                    (double)(((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime))/1e7;
    if(t<start) return;
    if(t>start+seconds) { fclose(out); fclose(vt); out=nullptr; return; }
    std::vector<uint32_t> objects;
    if(!extra_only) for(uint32_t actor:bleach_engine_actors) if(actor) objects.push_back(actor);
    for(uint32_t o:bleach_dump_extra) if(o) objects.push_back(o);
    uint32_t count=(uint32_t)objects.size();
    fwrite(&t,8,1,out); fwrite(&count,4,1,out);
    for(uint32_t actor:objects) {
        bool known=false; for(uint32_t s:seen) if(s==actor) {known=true;break;}
        if(!known) {
            for(auto& s:seen) if(!s) {s=actor;break;}
            const uint32_t table=vm_read32(actor);
            fprintf(vt,"actor %08x vtable %08x\n",actor,table);
            for(unsigned i=0;i<0x200;i+=4) {
                const uint32_t opd=vm_read32(table+i);
                fprintf(vt,"  +%03x %08x %08x\n",i,opd,opd?vm_read32(opd):0);
            }
            fflush(vt);
        }
        fwrite(&actor,4,1,out); fwrite(vm_base+actor,1,bytes,out);
        if(actor==focus) {
            uint32_t pointers[0x600]; uint16_t offsets[0x600]; uint32_t n=0;
            for(unsigned off=0;off<0x1800;off+=4) {
                const uint32_t p=vm_read32(actor+off);
                if(p>=0x10000000u&&p<0x40000000u&&!(p&3)) {pointers[n]=p;offsets[n]=(uint16_t)off;++n;}
            }
            fwrite(&n,4,1,out);
            for(uint32_t i=0;i<n;i++) {fwrite(&offsets[i],2,1,out);fwrite(&pointers[i],4,1,out);fwrite(vm_base+pointers[i],1,0x200,out);}
        }
    }
    fflush(out);
}
/* BLEACH_STACK_PROBE=<n>: the first n calls of each probed guest function made
 * while the native engine is active print their guest call chain (hand-placed
 * bleach_stack_probe() calls in recompiled/). */
extern "C" void ppu_dump_guest_stack(ppu_context*, const char*);
extern "C" void bleach_stack_probe(ppu_context* ctx, const char* tag, unsigned* seen) {
    static const unsigned limit=[] {const char* e=getenv("BLEACH_STACK_PROBE");return e?(unsigned)atoi(e):0u;}();
    if(!limit||!bleach_engine_active||*seen>=limit)return;
    ++*seen; ppu_dump_guest_stack(ctx,tag);
}
/* BLEACH_COUNTER_SCAN=<start s>:<path>: once, from <start s>, snapshots committed
 * guest memory below 0x40000000, follows every word that changed over the next
 * 40 scene builds, and writes the words that change on most render-only builds
 * (no simulation tick) -- the candidates for per-rendered-frame animation clocks.
 * Text: "addr changes_on_render_only/render_only changes_on_tick/ticks v0 v1 ...". */
static void bleach_counter_scan() {
    static int phase=-1; static double start=0; static char path[512];
    static std::vector<std::pair<uint32_t,std::vector<uint32_t>>> regions;
    static std::vector<uint32_t> addrs; static std::vector<std::vector<uint32_t>> values;
    static std::vector<bool> ticks;
    if(phase==-1) {
        const char* e=getenv("BLEACH_COUNTER_SCAN");
        phase=e&&sscanf(e,"%lf:%511[^\n]",&start,path)==2?0:99;
    }
    if(phase==99)return;
    FILETIME created,exited,kernel,user,now_ft; GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
    GetSystemTimeAsFileTime(&now_ft);
    const double t=((double)(((uint64_t)now_ft.dwHighDateTime<<32)|now_ft.dwLowDateTime)-
                    (double)(((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime))/1e7;
    if(phase==0) {
        if(t<start)return;
        // BLEACH_COUNTER_SCAN_RANGE=<hex lo>-<hex hi> (default 0x10000-0x40000000; RSX local
        // memory, where fragment programs and their patched constants live, is 0xC0000000+).
        uint64_t scan_lo=0x10000, scan_hi=0x40000000;
        if(const char* r=getenv("BLEACH_COUNTER_SCAN_RANGE")) {
            unsigned long long a0=0,a1=0; if(sscanf(r,"%llx-%llx",&a0,&a1)==2&&a1>a0){scan_lo=a0;scan_hi=a1;}
        }
        for(uint64_t a=scan_lo;a<scan_hi;) {
            MEMORY_BASIC_INFORMATION mi;
            if(!VirtualQuery(vm_base+a,&mi,sizeof mi))break;
            const uint64_t end=(uint64_t)((uint8_t*)mi.BaseAddress-vm_base)+mi.RegionSize;
            if(mi.State==MEM_COMMIT) {
                const uint32_t lo=(uint32_t)a, n=(uint32_t)((end>scan_hi?scan_hi:end)-a)/4;
                regions.emplace_back(lo,std::vector<uint32_t>());
                regions.back().second.resize(n); memcpy(regions.back().second.data(),vm_base+lo,n*4ull);
            }
            a=end;
        }
        phase=1; return;
    }
    if(phase==1) {
        for(auto& r:regions) for(uint32_t w=0;w<r.second.size();w++) {
            uint32_t v; memcpy(&v,vm_base+r.first+w*4ull,4);
            if(v!=r.second[w]&&addrs.size()<2000000){addrs.push_back(r.first+w*4);}
        }
        regions.clear(); regions.shrink_to_fit();
        values.resize(addrs.size());
        phase=2;
    }
    for(size_t i=0;i<addrs.size();i++){uint32_t v; memcpy(&v,vm_base+addrs[i],4); values[i].push_back(v);}
    ticks.push_back(bleach_engine_update);
    static const bool tick_mode=[] {const char* e=getenv("BLEACH_COUNTER_SCAN_MODE");return e&&!strcmp(e,"tick");}();
    if(ticks.size()<(tick_mode?80u:40u))return;
    FILE* out=fopen(path,"w");
    if(out) {
        unsigned render_only=0,tick_n=0; for(size_t k=1;k<ticks.size();k++)(ticks[k]?tick_n:render_only)++;
        fprintf(out,"# candidates=%zu frames=%zu render_only=%u ticks=%u flags=",addrs.size(),ticks.size(),render_only,tick_n);
        for(bool b:ticks)fputc(b?'T':'r',out); fputc('\n',out);
        for(size_t i=0;i<addrs.size();i++) {
            unsigned cr=0,ct=0;
            for(size_t k=1;k<ticks.size();k++)if(values[i][k]!=values[i][k-1])(ticks[k]?ct:cr)++;
            // BLEACH_COUNTER_SCAN_MODE=tick: words that change on (almost) every tick and never between.
            if(tick_mode?(cr!=0||ct*10<tick_n*9):(cr*10<render_only*7))continue;
            fprintf(out,"%08x %u/%u %u/%u",addrs[i],cr,render_only,ct,tick_n);
            for(uint32_t v:values[i])fprintf(out," %08x",v);
            fputc('\n',out);
        }
        fclose(out);
    }
    addrs.clear(); values.clear(); phase=99;
}
/* BLEACH_FRAMECOUNT_TICKS=1: the engine frame counter (engine object +0x2530,
 * func_00018D4C, once per presented frame) only advances on frames that ran a
 * simulation tick. On the PS3 frames and ticks were 1:1 at 30 Hz; HUD/2D
 * animations clocked by this counter otherwise run at the render rate. */
/* func_0002D9F4 also returns an actor's TIME-EFFECT sub-step count (slow motion,
 * hit stop, Ignition, ...) for 4 = normal speed. With BLEACH_SUBSTEPS below 4 (a
 * faster simulation) that value is scaled the same way; fractions are spread
 * over ticks (Bresenham on the tick count), so 1 at 60 Hz gives 0,1,0,1...
 * BLEACH_TIME_EFFECT_LOG=1 prints the first distinct values seen. */
/* Set while a scene task kept at 30 Hz (BLEACH_TASK_HALF) runs: it advances once per
 * Hz/30 ticks, so its sub-step clock is the original one (4 per update, time effects
 * unscaled). The camera follows the player with this clock; with the faster rate's
 * count it trailed behind at half its follow speed. */
static thread_local bool bleach_in_half_task=false;
extern "C" int bleach_tick_ratio() { return bleach_tick_ratio_now; }
/* Simulation ticks executed so far (BLEACH_STORE_PROFILE normalizes per tick). */
extern "C" uint64_t bleach_tick_count() { return bleach_native_updates.load(std::memory_order_relaxed); }
/* The camera (task 615D0, func_0005F77C) turns a fixed amount per update: the auto-turn
 * speed limit (32 of 4096 angle units, TOC-0x772C, read nowhere else) and the turn gains
 * of each camera mode (table 0x3DBCAC, 0x2C per mode: +0 pitch, +4 yaw) times the stick.
 * Its follow runs on the sub-step clock and needs nothing. At r ticks per original
 * update each of these words holds 1/r of its original value. */
static void bleach_camera_rate(int ratio) {
    struct Word {uint32_t ea; float original;};
    static const Word words[]={{0x004A6F44u,32.0f},
        {0x003DBCACu,1.0f},{0x003DBCB0u,1.0f},{0x003DBCD8u,1.0f},{0x003DBCDCu,1.0f},
        {0x003DBD04u,1.0f},{0x003DBD08u,1.0f},{0x003DBD30u,1.0f},{0x003DBD34u,1.0f},
        {0x003DBD5Cu,1.0f},{0x003DBD60u,1.0f},{0x003DBD88u,1.0f},{0x003DBD8Cu,1.0f}};
    static int applied=1;
    if(ratio==applied)return;
    for(const auto& w:words) {
        const float expected=w.original/applied, value=w.original/ratio;
        uint32_t expected_bits,value_bits; memcpy(&expected_bits,&expected,4); memcpy(&value_bits,&value,4);
        if(vm_read32(w.ea)!=expected_bits) {
            fprintf(stderr,"[camera-rate] %08X is not %g, left alone\n",w.ea,expected);continue;
        }
        vm_write32(w.ea,value_bits);
    }
    applied=ratio;
}
/* Ultrawide (rsx_aspect.h, BLEACH_ASPECT). func_0023D4B0 is the game's perspective builder
 * (out, fov, aspect, near, far): m00 = cot(fov/2) and m11 = m00 / aspect, with aspect =
 * height / width -- the game fixes the HORIZONTAL field of view. Every screen camera passes
 * 9/16 (gameplay, cutscenes, menus); other uses (shadow and texture projections) pass other
 * shapes. For those screen ones m00 is divided by k: the vertical view stays the game's and
 * the horizontal one widens to the monitor (Hor+). The game culls with these matrices too
 * (EDGE geometry jobs carry them), so the sides are populated. 1 when off or not 9/16. */
extern "C" double bleach_aspect_widen(double aspect) {
    static const double inverse_k=1.0/rsx_aspect_factor();
    // BLEACH_ASPECT_LOG=1 (debug): each distinct aspect argument, once.
    static const bool log=[]{const char* e=getenv("BLEACH_ASPECT_LOG");return e&&*e=='1';}();
    if(log) {
        static float seen[32]; static int n;
        bool known=false; for(int i=0;i<n;i++) known|=seen[i]==(float)aspect;
        if(!known&&n<32) { seen[n++]=(float)aspect; fprintf(stderr,"[aspect-projection] aspect=%.7g\n",aspect); }
    }
    return (float)aspect==0.5625f ? inverse_k : 1.0;
}
/* A turn step from those gains, made an integer by the game (fctiwz). At 1/r of the
 * gain the fraction would be dropped r times per original update (95 -> 47 + 47 at
 * 60 Hz); it is carried instead, so r ticks add up to the original step. */
extern "C" int32_t bleach_camera_step(double value,int site) {
    static double carry[3];
    double& c=carry[site];
    if(value==0||(c!=0&&(c<0)!=(value<0)))c=0;
    const double total=value+c;
    const int32_t step=(int32_t)std::trunc(total);
    c=total-step;
    return step;
}
/* State kept per key (an object, or an object and a call site) by the tick scaling below: 8 ways per set, the
 * least recently used way replaced. These were direct-mapped, and a key sharing its entry with another one
 * lost its state on every call: Rukia's model in episode 5 shared the elapsed-ticks entry of its motion command
 * (site 2FF394) with an unrelated object, so the command read 1 tick where 4 had passed and her intro pose clock
 * ran at 1/4 at 120 Hz, holding the turn to gameplay 4.6 s. Which keys collide depends only on the addresses,
 * so it hit one character and not the others. BLEACH_SLOT_LOG=1 (debug) reports a key still in use replaced. */
namespace {
template<class T,unsigned SetBits,uint32_t Mult=0x9E3779B1u>
struct BleachKeyedTable {
    struct Way {uint32_t key; uint64_t used; T value;};   // used: tick + 1, 0 = empty
    Way ways[8u<<SetBits];
    T& at(uint32_t key,bool& fresh) {
        const uint64_t now=bleach_native_updates.load(std::memory_order_relaxed)+1;
        Way* set=ways+(((key*Mult)>>(32-SetBits))<<3);
        Way* oldest=set;
        for(int i=0;i<8;++i) {
            if(set[i].used&&set[i].key==key) {set[i].used=now; fresh=false; return set[i].value;}
            if(set[i].used<oldest->used)oldest=set+i;
        }
        static const bool log=[] {const char* e=getenv("BLEACH_SLOT_LOG");return e&&!strcmp(e,"1");}();
        if(log&&oldest->used&&now-oldest->used<=4) {
            static std::atomic<uint64_t> n{0}; const uint64_t k=n.fetch_add(1);
            if(!(k&4095))fprintf(stderr,"[slot] %llu keys in use replaced (table of %u, tick %llu)\n",(unsigned long long)k+1,8u<<SetBits,(unsigned long long)now);
        }
        *oldest=Way{key,now,T{}};
        fresh=true;
        return oldest->value;
    }
    T& at(uint32_t key) {bool fresh; return at(key,fresh);}
};
}
/* Sub-steps per tick for an object without a time effect (func_0002D9F4, keyed by its
 * argument): the game's 4 per original 30 Hz update, split over the ticks that now make one up.
 * An object updated every tick gets 4/r (r = bleach_tick_ratio). One updated only every few
 * ticks -- whoever drives it at 30 Hz: a BLEACH_TASK_HALF parent, a held-back call, a screen
 * BLEACH_SIM_GAMEPLAY_ONLY keeps at 30 Hz -- gets 4 * elapsed / r, so it covers the time
 * that passed. Ichigo's episode 1 intro pose is driven by an opening task kept at 30 Hz;
 * with 4/r it advanced a quarter of its time per update and held the turn to gameplay 4.5 s.
 * A gap longer than r ticks (a paused object, a new one at a reused address) restarts at
 * 4/r instead of jumping. Calls within one tick get the same count. */
namespace {
struct BleachSubstepSlot {uint32_t count; uint64_t tick;};
thread_local BleachKeyedTable<BleachSubstepSlot,9> bleach_substep_slots;
}
/* Sub-step requests made by the innermost scene task running (BLEACH_TASK_CENSUS). */
static thread_local unsigned bleach_task_substep_calls=0;
extern "C" int bleach_substeps_for(uint32_t key) {
    ++bleach_task_substep_calls;
    if(bleach_in_half_task)return 4;
    const int ratio=bleach_tick_ratio_now<1?1:(bleach_tick_ratio_now>4?4:bleach_tick_ratio_now);
    const int base=4/ratio;
    if(ratio==1)return 4;
    const uint64_t tick=bleach_native_updates.load(std::memory_order_relaxed);
    bool fresh;
    BleachSubstepSlot& slot=bleach_substep_slots.at(key,fresh);
    if(!fresh&&slot.tick==tick)return (int)slot.count;
    const uint64_t elapsed=fresh?0:tick-slot.tick;
    const int count=elapsed>=2&&elapsed<=(uint64_t)ratio?(int)(4*elapsed/ratio):base;
    slot={(uint32_t)count,tick};
    return count;
}
/* Ticks since the last call with this key, for a consumer of a per-tick step stored elsewhere:
 * 1 when it runs every tick; 2..r when whatever drives it only gets there every few ticks
 * (r = bleach_tick_ratio), so it covers the time that passed; 1 again after a longer gap, and
 * for further calls within the same tick. The model event script of Ichigo's episode 1 intro
 * pose reads the step the actor stores every tick (a quarter at 120 Hz) but is run by an
 * opening task kept at 30 Hz, and advanced a quarter of its time: the turn to gameplay came
 * 4.5 s late. A BLEACH_TASK_HALF task or held-back call counts as r. */
namespace { thread_local BleachKeyedTable<uint64_t,9> bleach_elapsed_slots; }
extern "C" int bleach_elapsed_ticks(uint32_t key) {
    const int ratio=bleach_tick_ratio_now<1?1:(bleach_tick_ratio_now>4?4:bleach_tick_ratio_now);
    if(ratio==1)return 1;
    if(bleach_in_half_task)return ratio;
    const uint64_t tick=bleach_native_updates.load(std::memory_order_relaxed);
    bool fresh;
    uint64_t& last=bleach_elapsed_slots.at(key,fresh);
    const uint64_t elapsed=fresh?0:tick-last;
    last=tick;
    return elapsed>=2&&elapsed<=(uint64_t)ratio?(int)elapsed:1;
}
/* Original updates (30 Hz frames) that passed for a per-update countdown run every time its object
 * is: 1 at 30 Hz; at r ticks per update, 1 on the last tick of each aligned group of r ticks, so a
 * caller run every tick counts one frame per r ticks, and one run only every few ticks (a 30 Hz
 * task, a held-back call) gets the frames of the ticks it missed (bleach_elapsed_ticks). The model
 * event script's WAIT n (func_002FEFBC stores n frames at +0x14, func_002FEF40 holds the script
 * while it is not 0) is counted down by func_0022A15C on every run; Ichigo's Ignition start runs
 * it from the actor update every tick, so its camera cuts came r times early and the close-up
 * "blinked" at 120 Hz while his motion (the step at +0x1C, already per tick) kept its speed. */
namespace { thread_local BleachKeyedTable<int64_t,9> bleach_frame_slots; }
extern "C" int bleach_update_frames(uint32_t key) {
    static const bool on=[] {const char* e=getenv("BLEACH_CLOCK_FIX");return !(e&&!strcmp(e,"0"));}();
    const int ratio=bleach_tick_ratio_now<1?1:(bleach_tick_ratio_now>4?4:bleach_tick_ratio_now);
    if(ratio==1||!on)return 1;
    if(bleach_in_half_task)return 1;
    const int64_t tick=(int64_t)bleach_native_updates.load(std::memory_order_relaxed);
    bool fresh;
    int64_t& last=bleach_frame_slots.at(key,fresh);
    if(!fresh&&last==tick)return 0;   // again in the same tick
    const int64_t gap=fresh?1:tick-last;
    const int64_t elapsed=gap>=1&&gap<=ratio?gap:1;
    last=tick;
    return (int)((tick+1)/ratio-(tick+1-elapsed)/ratio);
}
/* Step of the real-time event timeline (func_00308AFC adds the global step at 0x71B8C4, 0x1000 =
 * one frame, "Slow" keys change it, to the clock at 0xADCB64 once per update). The task (308D1C)
 * runs every tick so its MotionCamera and model tracks move every tick -- held at 30 Hz, the
 * Ignition special move's camera stepped 30 times a second at 60/120 Hz. Each tick adds the
 * step times the ticks it stands for over r (bleach_elapsed_ticks: a caller held at 30 Hz gets
 * it whole), the remainder carried per object, so the timeline keeps the original length. */
namespace { thread_local BleachKeyedTable<int64_t,9> bleach_step_slots; }
/* An amount the original adds once per update (a frame of a timeline or an animation), spread over the
 * ticks: amount x ticks covered / r, the remainder carried per key, so r ticks add exactly the amount. */
extern "C" int32_t bleach_per_update(int32_t amount,uint32_t key) {
    static const bool on=[] {const char* e=getenv("BLEACH_CLOCK_FIX");return !(e&&!strcmp(e,"0"));}();
    const int ratio=bleach_tick_ratio_now<1?1:(bleach_tick_ratio_now>4?4:bleach_tick_ratio_now);
    if(ratio==1||!on)return amount;
    const int64_t elapsed=bleach_elapsed_ticks(key^0x5A5A5A5Au);
    int64_t& rem=bleach_step_slots.at(key);
    const int64_t total=(int64_t)amount*elapsed+rem;
    const int64_t part=total>=0?total/ratio:-((-total)/ratio);
    rem=total-part*ratio;
    return (int32_t)part;
}
extern "C" int32_t bleach_event_step(int32_t step,uint32_t key) { return bleach_per_update(step,key^0x308C54u); }
/* Model event script step (+0x1C, 0x1000 = one frame per run) -- what the stored value means depends on who
 * wrote it. The actor stores it every tick from the game's sub-steps (func_00229AF8 from code running every
 * tick: 1/r of the original at r ticks per update) -- a per-tick step. The init (func_0022A93C/22AA50), the
 * seek's restore (func_0022A964) and setters running at 30 Hz store an original per-update step -- cutscene
 * models: 43 of them, run by a task held at 30 Hz, got the per-tick multiplier and played 4x fast at 120 Hz
 * (Uryuu entering the episode 2 final area). The seek itself (func_0022A964) sets an exact step for one run.
 * bleach_model_step(object, step) is what one script run adds: per-tick step x ticks covered; original
 * step x ticks covered / r with the remainder carried; exact step as is. */
extern "C" void bleach_step_log(uint32_t object,int32_t step,int elapsed);
namespace {
struct BleachStepKind {uint8_t kind; int32_t carry;};
thread_local BleachKeyedTable<BleachStepKind,9> bleach_step_kinds;
BleachStepKind& bleach_step_slot(uint32_t object) {
    bool fresh;
    BleachStepKind& s=bleach_step_kinds.at(object,fresh);
    if(fresh)s={1,0};   // unknown: as stored by the init, an original step
    return s;
}
}
extern "C" void bleach_model_step_written(uint32_t object,int kind) {   // kind: 0 per tick, 1 original, 2 exact
    BleachStepKind& s=bleach_step_slot(object);
    if(kind==0&&(bleach_tick_ratio_now<2||bleach_in_half_task))kind=1;   // a 30 Hz writer stores the original
    s.kind=(uint8_t)kind; s.carry=0;
}
extern "C" int32_t bleach_model_step(uint32_t object,int32_t step) {
    const int ratio=bleach_tick_ratio_now<1?1:(bleach_tick_ratio_now>4?4:bleach_tick_ratio_now);
    static const bool kinds=[] {const char* e=getenv("BLEACH_MODEL_STEP");return !(e&&!strcmp(e,"0"));}();
    if(!kinds)return step*bleach_elapsed_ticks(object);   // BLEACH_MODEL_STEP=0: every step taken as per tick
    BleachStepKind& s=bleach_step_slot(object);
    if(s.kind==2)return step;
    const int el=bleach_elapsed_ticks(object);
    bleach_step_log(object,step,el*(s.kind==0?1:-1));
    if(s.kind==0||ratio==1)return step*el;
    const int64_t total=(int64_t)step*el+s.carry;
    const int64_t part=total>=0?total/ratio:-((-total)/ratio);
    s.carry=(int32_t)(total-part*ratio);
    return (int32_t)part;
}
/* BLEACH_ANIM_LOG=1 (debug): animation time each model advances per real second, summed over the two ways the
 * game moves it -- its event script (func_0022A15C, path 's') and the direct advance func_00268B8C (path 'a',
 * the event tracks' seek and the motion code) -- with the caller. Every second of simulation, the objects that
 * advanced more than 1.3x the original 30 frames a second are printed: a model playing too fast. */
extern "C" void bleach_anim_log(char path,uint32_t object,int32_t delta,uint32_t caller) {
    // BLEACH_ANIM_TRACE=<hex object> (debug): each run of that object's animation with a new (path, caller, delta, ticks since)
    static const uint32_t traced=[] {const char* e=getenv("BLEACH_ANIM_TRACE");return e?(uint32_t)strtoul(e,nullptr,16):0u;}();
    if(traced&&object==traced) {
        static std::unordered_map<uint64_t,uint64_t> last_tick; static std::unordered_map<std::string,int> seen;
        const uint64_t tick=bleach_native_updates.load(std::memory_order_relaxed);
        const uint64_t key=((uint64_t)caller<<8)|(uint8_t)path;
        const uint64_t gap=last_tick.count(key)?tick-last_tick[key]:0;
        last_tick[key]=tick;
        char id[96]; snprintf(id,sizeof id,"%c %08x %x %llu %d",path,caller,(unsigned)delta,(unsigned long long)gap,(int)bleach_in_half_task);
        if(seen[id]++==0&&seen.size()<2000)
            fprintf(stderr,"[animtrace] tick %llu %s (path caller delta gap in_half_task) event_clock %x\n",(unsigned long long)tick,id,vm_read32(0x00ADCB64));
    }
    static const bool on=[] {const char* e=getenv("BLEACH_ANIM_LOG");return e&&!strcmp(e,"1");}();
    if(!on)return;
    struct Sum {int64_t total=0; uint32_t calls=0, caller=0; char path=0;};
    static std::unordered_map<uint64_t,Sum> sums; static uint64_t window=0;
    const uint64_t tick=bleach_native_updates.load(std::memory_order_relaxed);
    const int ratio=bleach_tick_ratio_now<1?1:bleach_tick_ratio_now;
    if(tick/(30u*ratio)!=window) {
        for(auto& kv:sums) if(kv.second.total>(int64_t)(0x1000*30*13/10))
            fprintf(stderr,"[anim] tick %llu obj %08x path %c caller %08x frames/s %.1f calls %u event_clock %x\n",
                (unsigned long long)tick,(uint32_t)(kv.first>>8),kv.second.path,kv.second.caller,
                kv.second.total/4096.0,kv.second.calls,vm_read32(0x00ADCB64));
        sums.clear(); window=tick/(30u*ratio);
    }
    Sum& s=sums[((uint64_t)object<<8)|(uint8_t)path]; s.total+=delta>0?delta:0; ++s.calls; s.caller=caller; s.path=path;
}
/* BLEACH_STEP_LOG=1 (debug): each model event script run (func_0022A15C) that advances its time with a
 * (step, ticks covered) pair not seen before for that object prints it -- a step of 0x1000 (one original
 * frame) over one tick at 120 Hz is a model playing 4x fast. With the event clock (0xADCB64) for when. */
extern "C" void bleach_step_log(uint32_t object,int32_t step,int elapsed) {
    static const bool on=[] {const char* e=getenv("BLEACH_STEP_LOG");return e&&!strcmp(e,"1");}();
    if(!on||bleach_tick_ratio_now<2)return;
    static std::unordered_map<uint64_t,uint64_t> seen; static std::atomic<int> printed{0};
    const uint64_t k=((uint64_t)object<<32)^((uint64_t)(uint32_t)step<<4)^(uint64_t)elapsed;
    if(seen.count(k)||printed.load()>4000)return;
    seen[k]=1; ++printed;
    fprintf(stderr,"[step] tick %llu obj %08x step %x x%d (%s) event_clock %x\n",
        (unsigned long long)bleach_native_updates.load(std::memory_order_relaxed),object,(unsigned)step,elapsed,
        bleach_in_half_task?"30 Hz task":"every tick",vm_read32(0x00ADCB64));
}
/* Counts the motion command handlers (func_002FF218 and siblings, 0x2FF000-0x304300) make an
 * integer from the model playback rate (func_00229AAC: the stored step times a rate). The step is
 * stored per tick, 1/r of the original at r ticks per update, and the game's int() then dropped
 * what the original kept: rate 1 x 1 frame = 1 per update at 30 Hz, 0.25 x 1 = 0 per tick at
 * 120 Hz, so the frame counter behind Ichigo's episode 1 intro interrupt never moved and the
 * turn to gameplay waited for the end of the clip. The original count I = int(value * r) is
 * spread over the r ticks of an update by the tick count (each aligned group of r ticks adds up
 * to exactly I); a caller that runs only every few ticks gets the ticks it missed
 * (bleach_elapsed_ticks). At 30 Hz this is the game's own int(). */
static int32_t bleach_trunc_saturate(double v) {
    if(v!=v)return INT32_MIN;
    if(v>=2147483647.0)return INT32_MAX;
    if(v<=-2147483648.0)return INT32_MIN;
    return (int32_t)v;
}
extern "C" int32_t bleach_rate_count(double value,uint32_t object,uint32_t site) {
    const int ratio=bleach_tick_ratio_now<1?1:(bleach_tick_ratio_now>4?4:bleach_tick_ratio_now);
    if(ratio==1)return bleach_trunc_saturate(value);
    // the value is the stored step x a rate: per tick when the actor stores the step every tick, the original
    // per-update amount for a model whose step is the original (cutscene models, bleach_model_step)
    static const bool kinds=[] {const char* e=getenv("BLEACH_MODEL_STEP");return !(e&&!strcmp(e,"0"));}();
    const bool per_tick=!kinds||bleach_step_slot(object).kind==0;
    const int64_t count=bleach_trunc_saturate(per_tick?value*ratio:value);
    const int64_t elapsed=bleach_elapsed_ticks(object^(site*0x9E3779B1u));
    const int64_t tick=(int64_t)bleach_native_updates.load(std::memory_order_relaxed);
    auto floor_div=[](int64_t a,int64_t b){return a>=0?a/b:-((-a+b-1)/b);};
    return (int32_t)(floor_div((tick+1)*count,ratio)-floor_div((tick+1-elapsed)*count,ratio));
}
/* Per-update state in tasks that ignore the game's clock -- the generic fix. The game measures time
 * in sub-steps: func_0002D9F4 hands each object N sub-steps per update (4 at 30 Hz), and the
 * systems that ask for it (actors, camera, effects 3CFB90/3CF72C...) scale by N, so at r ticks per
 * original update (N = 4/r) they keep the original speed. Some scene tasks never ask: they assume
 * one update = 1/30 s (static analysis: claude-tools/static_acc.py + static_rate.py; the auto policy
 * logs the same). Most are held at 30 Hz, but these must run every tick or the scene breaks (input
 * 1B76CC, director 645A8, the multi-object components 1BBEA4/1B5B1C, particles 20FBA8: held, the
 * desert sky washes out and the hit effects vanish) -- and every state they advance per update
 * (x += v, v *= drag, life -= 1, alpha -= 0.3) ran r times too fast.
 * While one of them runs (BLEACH_CLOCK_TASKS, set around the task call), each state update it makes
 * through a wrapped store (claude-tools/edits/clock_sites.py: every store whose value is computed
 * from the old value of the same field, found statically in the code those tasks reach) keeps 1/r
 * of its change; integers keep the remainder per address, so each original update's total is
 * exact; a jump far larger than the last change (wrap, reload, reset) passes whole; a field the
 * same store writes again in the same tick (a count rebuilt within the update) passes unchanged.
 * The same function called from an actor (which already scales by N) is untouched.
 * Stores measured in play as per-update clocks even inside tasks that use N (jogo_compare.py: same
 * step per tick at 30 and 120 -- skill and special timers, the camera's lock-on counter, effect
 * timers) carry site bit 31 and keep 1/r wherever they run every tick; a caller held at 30 Hz
 * (gap = r ticks) passes whole.
 * At 30 Hz the values pass through. BLEACH_CLOCK_FIX=0 turns it off (A/B). */
static thread_local bool bleach_clock_task_context=false;
static bool bleach_clock_task(uint32_t target) {
    static const std::vector<uint32_t> tasks=[] {
        std::vector<uint32_t> v; const char* e=getenv("BLEACH_CLOCK_TASKS");
        std::string list=e?e:"1bbea4,1b5b1c,20fba8,645a8,1b76cc";
        for(size_t a=0;a<list.size();) {size_t b=list.find(',',a); if(b==std::string::npos)b=list.size();
            if(b>a)v.push_back((uint32_t)strtoul(list.substr(a,b-a).c_str(),nullptr,16)); a=b+1;}
        return v;
    }();
    for(uint32_t t:tasks)if(t==target)return true;
    return false;
}
namespace {
struct BleachClockRem {int32_t rem; double last; uint64_t tick;};
thread_local BleachKeyedTable<BleachClockRem,10,0x85EBCA6Bu> bleach_clock_rems;
BleachClockRem& bleach_clock_slot(uint32_t ea,uint32_t site) {
    return bleach_clock_rems.at(ea^(site*0x9E3779B1u));
}
/* true for a jump (wrap, reload): much larger than the last change kept at this address */
bool bleach_clock_jump(BleachClockRem& r,double d) {
    const double a=fabs(d);
    if(r.last>0.0&&a>16.0*r.last)return true;
    r.last=a; return false;
}
bool bleach_clock_enabled() {
    static const bool on=[] {const char* e=getenv("BLEACH_CLOCK_FIX");return !(e&&!strcmp(e,"0"));}();
    return on;
}
/* ticks this change stands for (0 = pass it whole) */
int bleach_clock_part(BleachClockRem& r,int& ratio,uint32_t site) {
    ratio=bleach_tick_ratio_now<1?1:(bleach_tick_ratio_now>4?4:bleach_tick_ratio_now);
    static const bool log=[] {const char* e=getenv("BLEACH_CLOCK_LOG");return e&&!strcmp(e,"1");}();
    {   // BLEACH_CLOCK_LOG=1: the first wrapped stores reached, with the context they ran in
        static std::atomic<int> seen{0};
        if(log&&seen.fetch_add(1)<8)fprintf(stderr,"[clock] wrapped store reached: ratio=%d task_context=%d\n",ratio,(int)bleach_clock_task_context);
    }
    // site bit 31: a store measured as a per-update clock in play (claude-tools/jogo_compare.py) -- scaled
    // wherever it runs every tick; the others only inside the clock-ignoring tasks
    if(ratio==1||!(bleach_clock_task_context||(site&0x80000000u))||!bleach_clock_enabled())return 0;
    const uint64_t tick=bleach_native_updates.load(std::memory_order_relaxed);
    if(r.tick==tick)return 0;
    const uint64_t gap=r.tick?tick-r.tick:1;
    r.tick=tick;
    const int part=gap>=1&&gap<(uint64_t)ratio?(int)gap:0;
    if(log&&part) {   // BLEACH_CLOCK_LOG=1: confirms the scaling runs (1st, then every 4096th)
        static std::atomic<uint64_t> n{0}; const uint64_t k=n.fetch_add(1);
        if(!(k&4095))fprintf(stderr,"[clock] scaled %llu state updates (tick %llu)\n",(unsigned long long)k+1,(unsigned long long)tick);
    }
    return part;
}
uint32_t bleach_clock_int(uint32_t ea,uint32_t v,uint32_t site,int bits) {
    BleachClockRem& r=bleach_clock_slot(ea,site);
    int ratio; const int elapsed=bleach_clock_part(r,ratio,site);
    if(!elapsed)return v;
    const uint32_t mask=bits==32?0xFFFFFFFFu:((1u<<bits)-1u);
    const uint32_t old=bits==32?vm_read32(ea):bits==16?vm_read16(ea):vm_read8(ea);
    const int shift=32-bits;
    const int32_t d=(int32_t)((v-old)<<shift)>>shift;
    if(!d)return v;
    if(bleach_clock_jump(r,d)){r.rem=0;return v;}
    const int64_t total=(int64_t)r.rem+(int64_t)d*elapsed;
    const int64_t out=total/ratio;          // toward zero; the rest waits for the next ticks
    r.rem=(int32_t)(total-out*ratio);
    return (old+(uint32_t)out)&mask;
}
}
extern "C" uint32_t bleach_clock_f32(uint32_t ea,uint32_t v,uint32_t site) {
    BleachClockRem& r=bleach_clock_slot(ea,site);
    int ratio; const int elapsed=bleach_clock_part(r,ratio,site);
    if(!elapsed)return v;
    const uint32_t ob=vm_read32(ea);
    float fo,fn; memcpy(&fo,&ob,4); memcpy(&fn,&v,4);
    if(!(fo==fo)||!(fn==fn)||std::isinf(fo)||std::isinf(fn)||fo==fn)return v;
    if(bleach_clock_jump(r,(double)fn-(double)fo))return v;
    const float out=(float)((double)fo+((double)fn-(double)fo)*elapsed/ratio);
    uint32_t b; memcpy(&b,&out,4); return b;
}
extern "C" uint32_t bleach_clock_i32(uint32_t ea,uint32_t v,uint32_t site) {return bleach_clock_int(ea,v,site,32);}
extern "C" uint32_t bleach_clock_i16(uint32_t ea,uint32_t v,uint32_t site) {return bleach_clock_int(ea,v&0xFFFFu,site,16);}
extern "C" uint32_t bleach_clock_i8(uint32_t ea,uint32_t v,uint32_t site) {return bleach_clock_int(ea,v&0xFFu,site,8);}
/* Camera transitions (func_00060958, func_00060CE0, driven by the camera task 615D0 every tick):
 * a mode stores n = remaining frames (camera +0xE8, from the requested duration at +0x118) and each
 * update moves angle, zoom and position by (target - current)/n, then n -= 1 -- it lands exactly on
 * the target after n updates. At r ticks per original update that took n ticks: the Ignition
 * cut-in camera and the lock-on switch ran in a quarter of the time while the character kept its
 * speed. The duration is converted when the transition starts: n frames -> n*r ticks, so each tick
 * moves 1/(n*r) of the way and the camera arrives when it did at 30 FPS, moving every tick.
 * func_0005EF40 (value, frames: camera +0x180/+0x184, eased by 615D0 the same way) is requested by
 * event scripts from any task; what counts is the camera task, which runs every tick in gameplay
 * (BLEACH_TASK_FULL), so the length scales whoever asks. At 30 FPS n passes as is. */
extern "C" uint32_t bleach_frames_to_ticks(uint32_t n) {
    const int ratio=bleach_tick_ratio_now<1?1:(bleach_tick_ratio_now>4?4:bleach_tick_ratio_now);
    static const bool log=[] {const char* e=getenv("BLEACH_CLOCK_LOG");return e&&!strcmp(e,"1");}();
    if(ratio==1||!bleach_clock_enabled()||(int32_t)n<=0||n>0x100000u)return n;
    if(log)fprintf(stderr,"[clock] camera transition %u frames -> %u ticks (tick %llu)\n",n,n*ratio,
                   (unsigned long long)bleach_native_updates.load(std::memory_order_relaxed));
    return n*(uint32_t)ratio;
}
/* Scenery scroll and oscillation (func_001B9CFC, run by task 1B76CC every tick): texture offsets
 * and phases advance a fixed speed per call -- the desert dust of episode 1 drifts by it. At r
 * ticks per original update it moved r times too fast. Each call now advances by
 * (ticks since this object's last call) / r of each speed: 1/r every tick, so the motion
 * stays smooth at 60/120 at the original speed; 1 at 30 FPS. Set once per call. */
static thread_local double bleach_scroll_part=1.0;
extern "C" void bleach_scroll_begin(uint32_t object) {
    const int ratio=bleach_tick_ratio_now<1?1:(bleach_tick_ratio_now>4?4:bleach_tick_ratio_now);
    bleach_scroll_part=ratio==1?1.0:(double)bleach_elapsed_ticks(object^0x5C7011u)/ratio;
}
extern "C" double bleach_scroll_speed(double speed) { return (double)(float)(speed*bleach_scroll_part); }
extern "C" int bleach_scale_time_effect(int value,uint32_t key) {
    if(bleach_in_half_task)return value;
    const int substeps=bleach_substeps_for(key);
    static const bool log=[] {const char* e=getenv("BLEACH_TIME_EFFECT_LOG");return e&&!strcmp(e,"1");}();
    const uint64_t tick=bleach_native_updates.load(std::memory_order_relaxed);
    if(log) {
        static std::atomic<unsigned> seen{0}; static int values[32];
        bool known=false; const unsigned n=seen.load();
        for(unsigned i=0;i<n&&i<32;i++)if(values[i]==value)known=true;
        if(!known&&n<32){values[n]=value;seen.store(n+1);fprintf(stderr,"[time-effect] value=%d tick=%llu\n",value,(unsigned long long)tick);}
    }
    if(substeps==4||value<=0)return value;
    const uint64_t a=(uint64_t)value*(uint64_t)substeps;
    return (int)(((tick+1)*a)/4-(tick*a)/4);
}
/* The game's frame counter (game object +0x14, 0x00723534): on the PS3 func_0001CE58 counts one
 * per game frame (it waits on the vblank semaphore and drains the extra posts), 30 a second.
 * The native engine counted one per simulation tick, so at r ticks per original update it ran
 * r times as fast, and what animates by it -- the scenery mist of episode 1 -- with it.
 * BLEACH_GAME_COUNTER_PHASE=1 counts once per original update (every r-th tick). */
extern "C" int bleach_game_counter_step() {
    static const bool phase=[] {const char* e=getenv("BLEACH_GAME_COUNTER_PHASE");return e&&!strcmp(e,"1");}();
    const int ratio=bleach_tick_ratio_now<1?1:bleach_tick_ratio_now;
    if(!phase||ratio==1)return 1;
    return bleach_native_updates.load(std::memory_order_relaxed)%ratio==0?1:0;
}
extern "C" int bleach_engine_frame_count_step() {
    static const int mode=[] {const char* e=getenv("BLEACH_FRAMECOUNT_TICKS");
        return !e?0:!strcmp(e,"1")?1:!strcmp(e,"phase")?2:0;}();
    if(mode==2) {   // experiment: once per original 30 Hz update in gameplay
        if(!bleach_engine_active||!bleach_engine_update)return bleach_engine_active?0:1;
        const int ratio=bleach_tick_ratio_now<1?1:bleach_tick_ratio_now;
        return bleach_native_updates.load(std::memory_order_relaxed)%ratio==0?1:0;
    }
    return !mode||!bleach_engine_active||bleach_engine_update?1:0;
}
/* BLEACH_MEMSEARCH=<start s>:<hex word>[,<hex word>...]: once, at <start s>, prints every
 * aligned guest address below 0x40000000 holding one of the words (big-endian). */
/* BLEACH_PEEK=<start s>:<hex address>:<hex length> (debug): once, at <start s>, prints that
 * stretch of guest memory as big-endian words, 8 per line. */
static void bleach_peek() {
    static int done=-1; static double start; static uint32_t address,length;
    if(done==-1) {
        done=1; const char* e=getenv("BLEACH_PEEK"); if(!e)return;
        if(sscanf(e,"%lf:%x:%x",&start,&address,&length)!=3)return;
        done=0;
    }
    if(done)return;
    FILETIME created,exited,kernel,user,now_ft; GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
    GetSystemTimeAsFileTime(&now_ft);
    const double t=((double)(((uint64_t)now_ft.dwHighDateTime<<32)|now_ft.dwLowDateTime)-
                    (double)(((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime))/1e7;
    if(t<start)return;
    done=1;
    for(uint32_t a=address&~3u;a<address+length;a+=32) {
        fprintf(stderr,"[peek] %08x:",a);
        for(uint32_t i=0;i<32;i+=4)fprintf(stderr," %08x",vm_read32(a+i));
        fprintf(stderr,"\n");
    }
}
static void bleach_memsearch() {
    static int done=-1; static double start; static std::vector<uint32_t> words;
    if(done==-1) {
        done=1; const char* e=getenv("BLEACH_MEMSEARCH"); if(!e)return;
        char* end=nullptr; start=strtod(e,&end); if(*end!=':')return;
        for(const char* p=end+1;*p;) { char* q=nullptr; words.push_back((uint32_t)strtoul(p,&q,16)); if(q==p)break; p=*q==','?q+1:q; }
        done=0;
    }
    if(done)return;
    FILETIME created,exited,kernel,user,now_ft; GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
    GetSystemTimeAsFileTime(&now_ft);
    const double t=((double)(((uint64_t)now_ft.dwHighDateTime<<32)|now_ft.dwLowDateTime)-
                    (double)(((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime))/1e7;
    if(t<start)return;
    done=1; unsigned hits=0;
    for(uint64_t a=0x10000;a<0x40000000;) {
        MEMORY_BASIC_INFORMATION mi;
        if(!VirtualQuery(vm_base+a,&mi,sizeof mi))break;
        const uint64_t end=(uint64_t)((uint8_t*)mi.BaseAddress-vm_base)+mi.RegionSize;
        if(mi.State==MEM_COMMIT)
            for(uint64_t x=a;x+4<=end&&x<0x40000000;x+=4) {
                const uint32_t v=vm_read32((uint32_t)x);
                for(uint32_t w:words)if(v==w&&hits<400){fprintf(stderr,"[memsearch] %08x = %08x\n",(unsigned)x,v);++hits;}
            }
        a=end;
    }
    fprintf(stderr,"[memsearch] done, %u hits\n",hits);
}
/* BLEACH_TRACE32=<hex address>[,...] (debug): at the end of every tick where one of the words
 * changed, prints the tick, the seconds since start and every word. */
static void bleach_trace32() {
    static int state=-1; static std::vector<uint32_t> addrs,last;
    if(state==-1) {
        state=0; const char* e=getenv("BLEACH_TRACE32"); if(!e)return;
        for(const char* p=e;*p;) { char* q=nullptr; addrs.push_back((uint32_t)strtoul(p,&q,16)); if(q==p)break; p=*q==','?q+1:q; }
        last.assign(addrs.size(),0xDEADBEEFu); state=1;
    }
    if(!state)return;
    bool changed=false;
    for(size_t i=0;i<addrs.size();i++) { const uint32_t v=vm_read32(addrs[i]); if(v!=last[i]){changed=true;last[i]=v;} }
    if(!changed)return;
    fprintf(stderr,"[trace32] tick %llu t %.3f",(unsigned long long)bleach_tick_count(),(double)GetTickCount64()/1000.0);
    for(size_t i=0;i<addrs.size();i++)fprintf(stderr," %08x=%08x",addrs[i],last[i]);
    fprintf(stderr,"\n");
}
/* BLEACH_SNAPSHOT=<start s>:<interval s>:<hex begin>-<hex end>[,...]:<path> (debug): from <start s>, every
 * <interval s>, the 4 KB pages of those guest ranges that changed since the previous snapshot (the first one
 * writes them all). Record: double t, uint64 tick, uint32 pages, then per page uint32 address + 4096 bytes. */
static void bleach_snapshot() {
    static int state=-1; static double start,interval,next; static FILE* out=nullptr;
    static std::vector<std::pair<uint32_t,uint32_t>> ranges; static std::vector<uint8_t> last;
    if(state==-1) {
        state=0; const char* e=getenv("BLEACH_SNAPSHOT"); if(!e)return;
        int used=0;
        if(sscanf(e,"%lf:%lf:%n",&start,&interval,&used)<2||!used)return;
        const char* p=e+used;
        while(*p&&*p!=':') {
            char* q=nullptr; const uint32_t a=(uint32_t)strtoul(p,&q,16); if(q==p||*q!='-')return;
            const uint32_t b=(uint32_t)strtoul(q+1,&q,16); ranges.push_back({a&~0xFFFu,(b+0xFFFu)&~0xFFFu});
            p=*q==','?q+1:q;
        }
        if(*p!=':')return;
        size_t bytes=0; for(auto& r:ranges)bytes+=r.second-r.first;
        out=fopen(p+1,"wb"); if(!out||!bytes)return;
        last.assign(bytes,0); next=start; state=1;
    }
    if(state!=1)return;
    FILETIME created,exited,kernel,user,now_ft; GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
    GetSystemTimeAsFileTime(&now_ft);
    const double t=((double)(((uint64_t)now_ft.dwHighDateTime<<32)|now_ft.dwLowDateTime)-
                    (double)(((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime))/1e7;
    if(t<next)return;
    const bool first=next==start; next=t+interval;
    std::vector<uint32_t> pages; size_t base=0;
    for(auto& r:ranges) {
        for(uint32_t a=r.first;a<r.second;a+=4096,base+=4096) {
            MEMORY_BASIC_INFORMATION mi;   // pages the guest never mapped are skipped
            if(!VirtualQuery(vm_base+a,&mi,sizeof mi)||mi.State!=MEM_COMMIT)continue;
            if(first||memcmp(&last[base],vm_base+a,4096)) { memcpy(&last[base],vm_base+a,4096); pages.push_back(a); }
        }
    }
    const uint64_t tick=bleach_tick_count(); const uint32_t n=(uint32_t)pages.size();
    fwrite(&t,8,1,out); fwrite(&tick,8,1,out); fwrite(&n,4,1,out);
    for(uint32_t a:pages){fwrite(&a,4,1,out);fwrite(vm_base+a,1,4096,out);}
    fflush(out);
}
/* BLEACH_GAUGE_FULL=1 (test cheat): keeps the player's Ignition gauge full -- the halfword at +0x16C of the
 * player actor (vtable 0x461DA8), filled up to its maximum at +0x16E (900) after every tick. */
static void bleach_gauge_full() {
    static const bool on=[]{const char* e=getenv("BLEACH_GAUGE_FULL");return e&&!strcmp(e,"1");}();
    if(!on||!bleach_engine_active)return;
    for(uint32_t actor:bleach_engine_actors) {
        if(!actor||vm_read32(actor)!=0x00461DA8u)continue;
        const uint16_t max=vm_read16(actor+0x16E);
        if(max&&max<=10000&&vm_read16(actor+0x16C)<max)vm_write16(actor+0x16C,max);
    }
}
extern "C" void bleach_engine_frame_end() {
    bleach_counter_scan();
    bleach_memsearch();
    bleach_peek();
    bleach_trace32();
    bleach_snapshot();
    bleach_gauge_full();
    if(bleach_engine_active&&bleach_engine_update){bleach_actor_trace();bleach_actor_dump();}
    if(bleach_engine_active&&!bleach_engine_update&&bleach_engine_positions!=bleach_engine_position_hash())
        ++bleach_native_position_changes;
    if(bleach_engine_active&&!bleach_engine_update&&bleach_engine_audit)
        for(const auto& snapshot:bleach_actor_before)if(snapshot.object)
            for(unsigned word=0;word<bleach_actor_words;word++)
                if(snapshot.words[word]!=vm_read32(snapshot.object+word*4)) {
                    ++bleach_actor_changes[word];
                    if(bleach_actor_change_details.fetch_add(1)<32)
                        fprintf(stderr,"[native-engine-actor-detail] scene=%llu object=%08x offset=%04x before=%08x after=%08x\n",
                            (unsigned long long)bleach_native_scenes.load(),snapshot.object,word*4,
                            snapshot.words[word],vm_read32(snapshot.object+word*4));
                }
}
extern "C" uint32_t bleach_engine_submission(uint32_t legacy) {
    return bleach_engine_active?0x80000000u:legacy;
}
extern "C" void bleach_engine_acceptance(uint32_t accepted) {
    if(PS3_FT_ON()) {
        const uint64_t t=ps3_ft_now();
        const uint32_t graphics=vm_read32(0x004ae670u-0x3c38u);
        // func_00248E24 toggles the index after a successful acquisition.
        ps3_ft_span(BLEACH_FT_SLOT_ACQUIRE,0,accepted,graphics?vm_read8(graphics+0x4400):0xff,t,t);
    }
    if(bleach_engine_active) {
        if(accepted) {
            ++bleach_native_accepted;
            if(bleach_engine_dispatch) {LARGE_INTEGER now;QueryPerformanceCounter(&now);bleach_engine_dispatch->submitted(now.QuadPart);}
        } else ++bleach_native_rejected;
    }
}
extern "C" uint32_t bleach_engine_completion(uint32_t deadline,uint32_t vblank) {
    if(deadline==0x80000000u){++bleach_native_completions;return vblank;}
    return deadline;
}
#else
extern "C" int bleach_tick_ratio(){return 1;}
extern "C" int32_t bleach_camera_step(double value,int){return (int32_t)value;}
extern "C" uint64_t bleach_engine_audit_token(){return 0;}
static bool bleach_engine_reuse_camera(uint32_t,uint32_t,uint32_t){return false;}
static void bleach_engine_note_actor(uint32_t,uint32_t,uint32_t){}
extern "C" int bleach_engine_frame_begin(ppu_context*){return 0;}
extern "C" void bleach_engine_frame_end(){}
extern "C" uint32_t bleach_engine_submission(uint32_t legacy){return legacy;}
extern "C" void bleach_engine_acceptance(uint32_t){}
extern "C" void bleach_engine_buffer_released(uint32_t){}
extern "C" uint32_t bleach_engine_completion(uint32_t deadline,uint32_t){return deadline;}
#endif
#endif
