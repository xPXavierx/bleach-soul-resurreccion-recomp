#include <unordered_set>
#include <map>
#ifndef BLEACH_NATIVE_FRAME_DIAGNOSTICS_H
#define BLEACH_NATIVE_FRAME_DIAGNOSTICS_H

#include <vector>

extern "C" void ps3_indirect_call(ppu_context*);
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

/* Included only by main.cpp. Observe the original task graph before changing
 * simulation/render ownership. No register, memory or scheduling changes. */
#ifdef _WIN32
struct BleachStageSample {
    uint32_t site, target, object, mode, parent;
    uint64_t calls, ticks, max_ticks;
    double first_f1;
};
static BleachStageSample bleach_stage_samples[2048];
static thread_local uint32_t bleach_stage_parent;
static unsigned bleach_stage_count;
static uint64_t bleach_stage_overflow;
static SRWLOCK bleach_stage_lock=SRWLOCK_INIT;
static void bleach_dump_stages() {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    AcquireSRWLockExclusive(&bleach_stage_lock);
    fprintf(stderr,"[frame-stage-summary] entries=%u overflow_calls=%llu\n",
        bleach_stage_count,(unsigned long long)bleach_stage_overflow);
    for (unsigned i=0;i<bleach_stage_count;i++) {
        const auto& s=bleach_stage_samples[i];
        fprintf(stderr,"[frame-stage] site=%08x target=%08x object=%08x mode=%u calls=%llu total_ms=%.3f max_ms=%.3f first_f1=%.9g parent=%08x\n",
            s.site,s.target,s.object,s.mode,(unsigned long long)s.calls,
            (double)s.ticks*1000/f.QuadPart,(double)s.max_ticks*1000/f.QuadPart,s.first_f1,s.parent);
    }
    ReleaseSRWLockExclusive(&bleach_stage_lock);
}
#endif

/* BLEACH_TASK_HALF=<hex update fn>[,...] or "*" (experiment, with BLEACH_SIMULATION_HZ
 * above 30): scene tasks whose update (vtable+8) is listed run only on every
 * (Hz/30)th simulation tick, i.e. keep their original 30 Hz per-tick clocks.
 * BLEACH_TASK_FULL=<list> exempts tasks from "*". Applies only while actors update. */
// True after bleach_engine_task_decimated() lets a BLEACH_TASK_HALF task run its update.
static thread_local bool bleach_half_task_running=false;
/* Scene tasks seen asking for sub-steps (func_0002D9F4) while they ran: their motion follows the
 * sub-step clock, so they run every tick. BLEACH_TASK_HALF=auto keeps every other scene task
 * of the gameplay at 30 Hz: it counts in whole ticks (cutscene and event timers, scrolling
 * effects, target switching...) and ran r times too fast at r ticks per original update. A
 * task held back that then asks for sub-steps gets the original 4 there (bleach_in_half_task)
 * and every tick from its next run on. BLEACH_TASK_FULL lists tasks never held back (actors,
 * their components, input, hit director, camera, dialog, effects). */
static std::unordered_set<uint32_t> bleach_task_substep_users;
static bool bleach_engine_task_decimated(uint32_t target) {
    bleach_half_task_running=false;
    struct Config {bool all=false,automatic=false; std::vector<uint32_t> half,full;};
    static const Config config=[] {
        Config c;
        auto parse=[](const char* e,std::vector<uint32_t>& out) {
            for(const char* p=e;p&&*p;) {
                char* end=nullptr; const uint32_t v=(uint32_t)strtoul(p,&end,16);
                if(end==p)break; out.push_back(v); p=*end==','?end+1:end;
            }
        };
        const char* half=getenv("BLEACH_TASK_HALF");
        if(half&&!strcmp(half,"*"))c.all=true;
        else if(half&&!strncmp(half,"auto",4)){c.automatic=true;parse(half[4]==','?half+5:half+4,c.half);}
        else parse(half,c.half);
        parse(getenv("BLEACH_TASK_FULL"),c.full);
        return c;
    }();
    // Gameplay only (an actor update ran this or the previous tick): menus read
    // button edges per tick, and a skipped tick there loses the press.
    const uint64_t tick=bleach_native_updates.load(std::memory_order_relaxed);
    if(target==0x000A1770u)bleach_engine_actor_tick.store(tick,std::memory_order_relaxed);
    const unsigned ratio=bleach_simulation_hz()/30;
    if(ratio<2||!bleach_engine_active||(!config.all&&!config.automatic&&config.half.empty()))return false;
    if(!bleach_engine_gameplay())return false;
    bool half=config.all||(config.automatic&&!bleach_task_substep_users.count(target));
    for(uint32_t t:config.full)if(t==target)half=false;
    if(!config.all)for(uint32_t t:config.half)if(t==target)half=true;
    if(config.automatic&&half) {
        static std::unordered_set<uint32_t> logged;
        if(logged.insert(target).second)
            fprintf(stderr,"[task-auto] %08x held at 30 Hz (tick %llu)\n",target,(unsigned long long)tick);
    }
    if(tick%ratio==0) { bleach_half_task_running=half; return false; }
    return half;
}

/* BLEACH_PREPARE_TICK=<hex prepare fn>[,...] (experiment): scene-prepare callbacks
 * (site 0x27B888, run every rendered frame) that advance per-call animation state
 * (HUD and similar) are skipped on render-only frames, as BLEACH_NATIVE_CAMERA does
 * for the camera, so they advance once per simulation tick like on the PS3. */
static bool bleach_engine_prepare_tick_only(uint32_t target) {
    static const std::vector<uint32_t> list=[] {
        std::vector<uint32_t> out;
        for(const char* p=getenv("BLEACH_PREPARE_TICK");p&&*p;) {
            char* end=nullptr; const uint32_t v=(uint32_t)strtoul(p,&end,16);
            if(end==p)break; out.push_back(v); p=*end==','?end+1:end;
        }
        return out;
    }();
    if(list.empty()||!bleach_engine_active||bleach_engine_update)return false;
    for(uint32_t t:list)if(t==target)return true;
    return false;
}

/* BLEACH_PREPARE_FREEZE=<hex prepare fn>[,...] (experiment): those scene-prepare
 * callbacks still run (and draw) on every rendered frame, but on render-only frames
 * the first BLEACH_PREPARE_FREEZE_BYTES (hex, default 0x2000) of their object are
 * restored afterwards, so per-call animation state advances only on ticks.
 * BLEACH_PREPARE_FREEZE_AUDIT=1 restores nothing and reports, at exit, which object
 * offsets each callback changes on render-only frames. "*" = every prepare callback but the camera. */
struct BleachFreezeConfig {std::vector<uint32_t> targets; uint32_t bytes=0x2000; bool audit=false, all=false, phase=false;};
static const BleachFreezeConfig& bleach_freeze_config() {
    static const BleachFreezeConfig config=[] {
        BleachFreezeConfig c;
        const char* list=getenv("BLEACH_PREPARE_FREEZE");
        if(list&&*list=='*'){c.all=true;list=nullptr;}
        for(const char* p=list;p&&*p;) {
            char* end=nullptr; const uint32_t v=(uint32_t)strtoul(p,&end,16);
            if(end==p)break; c.targets.push_back(v); p=*end==','?end+1:end;
        }
        if(const char* b=getenv("BLEACH_PREPARE_FREEZE_BYTES"))c.bytes=(uint32_t)strtoul(b,nullptr,16)&~3u;
        const char* a=getenv("BLEACH_PREPARE_FREEZE_AUDIT"); c.audit=a&&!strcmp(a,"1");
        const char* ph=getenv("BLEACH_PREPARE_FREEZE_PHASE"); c.phase=ph&&!strcmp(ph,"1");
        return c;
    }();
    return config;
}
struct BleachFreezeAudit {uint32_t target=0; uint64_t calls=0; std::vector<uint32_t> changes;};
static std::vector<BleachFreezeAudit> bleach_freeze_audits;
static void bleach_freeze_report() {
    for(const auto& a:bleach_freeze_audits) {
        unsigned changed=0; for(uint32_t c:a.changes)if(c)++changed;
        fprintf(stderr,"[prepare-freeze-audit] target=%08x render_only_calls=%llu changed_words=%u:",
            a.target,(unsigned long long)a.calls,changed);
        unsigned shown=0;
        for(size_t i=0;i<a.changes.size()&&shown<40;i++)if(a.changes[i]){fprintf(stderr," +%zx(%u)",i*4,a.changes[i]);++shown;}
        fprintf(stderr,"\n");
    }
}

/* BLEACH_CALL_HALF=<hex return address>[,...] (experiment, with BLEACH_SIMULATION_HZ
 * above 30): hand-marked guest calls (sites in recompiled/, "BLEACH_CALL_HALF hand
 * edit") are skipped on the ticks a decimated task would skip, during gameplay. */
extern "C" int bleach_engine_call_decimated(uint32_t site) {
    static const std::vector<uint32_t> sites=[] {
        std::vector<uint32_t> out;
        for(const char* p=getenv("BLEACH_CALL_HALF");p&&*p;) {
            char* end=nullptr; const uint32_t v=(uint32_t)strtoul(p,&end,16);
            if(end==p)break; out.push_back(v); p=*end==','?end+1:end;
        }
        return out;
    }();
    const unsigned ratio=bleach_simulation_hz()/30;
    if(sites.empty()||ratio<2||!bleach_engine_active||!bleach_engine_gameplay())return 0;
    if(bleach_native_updates.load(std::memory_order_relaxed)%ratio==0)return 0;
    for(uint32_t s:sites)if(s==site)return 1;
    return 0;
}

/* A call kept at 30 Hz by BLEACH_CALL_HALF runs, when it runs, as a BLEACH_TASK_HALF task
 * does: once per Hz/30 ticks, so sub-step counts and the steps stored from them count as the
 * original (bleach_in_half_task), and a step it reads that was stored from them counts r times.
 * enter() returns the previous state to give back to leave(), or -1 when nothing changed. */
extern "C" int bleach_half_call_enter(uint32_t site) {
    static const std::vector<uint32_t> sites=[] {
        std::vector<uint32_t> out;
        for(const char* p=getenv("BLEACH_CALL_HALF");p&&*p;) {
            char* end=nullptr; const uint32_t v=(uint32_t)strtoul(p,&end,16);
            if(end==p)break; out.push_back(v); p=*end==','?end+1:end;
        }
        return out;
    }();
    if(bleach_simulation_hz()/30<2||!bleach_engine_active||!bleach_engine_gameplay())return -1;
    bool listed=false; for(uint32_t s:sites)if(s==site)listed=true;
    if(!listed)return -1;
    const int was=bleach_in_half_task?1:0;
    bleach_in_half_task=true;
    return was;
}
extern "C" void bleach_half_call_leave(int was) {
    if(was>=0)bleach_in_half_task=was!=0;
}

extern "C" void bleach_frame_dispatch(ppu_context* ctx,uint32_t site) {
    // The scene traversal ignores this callback's return value. Only its
    // persistent camera state belongs to simulation ticks, not extra draws.
    if(bleach_engine_reuse_camera(site,(uint32_t)ctx->ctr,(uint32_t)ctx->gpr[3]))return;
    if(site==0x0027B888 && bleach_engine_prepare_tick_only((uint32_t)ctx->ctr))return;
    if(site>=0x0027B8AC && site<0x0027C000)
        bleach_engine_note_actor((uint32_t)ctx->ctr,(uint32_t)ctx->gpr[3],(uint32_t)ctx->gpr[4]);
    if(site==0x0027BBFC && bleach_engine_task_decimated((uint32_t)ctx->ctr)) {ctx->gpr[3]=1;return;}
    const bool half_run=site==0x0027BBFC&&bleach_half_task_running;
#ifdef _WIN32
    static const long after=[] {
        const char* value=getenv("BLEACH_FRAME_STAGES_AFTER");
        if (value) atexit(bleach_dump_stages);
        return value ? atol(value) : -1L;
    }();
    static const ULONGLONG origin=GetTickCount64();
    const bool capture=after>=0 && GetTickCount64()-origin>=(ULONGLONG)after*1000;
    const uint32_t target=(uint32_t)ctx->ctr, object=(uint32_t)ctx->gpr[3];
    // Event selectors belong to lifecycle/update callbacks. Render callbacks
    // may leave a changing scratch value in r4; it is not an event selector.
    const uint32_t mode=site>=0x0027B8AC && site<0x0027C000 ? (uint32_t)ctx->gpr[4] : 0;
    const double f1=ctx->fpr[1];
    const uint32_t parent=bleach_stage_parent;
    LARGE_INTEGER start, end;
    if (capture) { QueryPerformanceCounter(&start); bleach_stage_parent=target; }
#endif
    // BLEACH_PREPARE_FREEZE: snapshot before a render-only prepare callback.
    static thread_local std::vector<uint8_t> freeze_saved;
    uint32_t freeze_object=0, freeze_target=0;
    {
        const auto& freeze=bleach_freeze_config();
        // BLEACH_PREPARE_FREEZE_PHASE=1: in gameplay at r ticks per original update, the ticks
        // between original updates count as render-only too (per-call state advances 1 in r).
        const unsigned freeze_ratio=bleach_simulation_hz()/30;
        const bool between_updates=freeze.phase&&freeze_ratio>=2&&bleach_engine_gameplay()&&
            bleach_native_updates.load(std::memory_order_relaxed)%freeze_ratio!=0;
        if(site==0x0027B888&&(freeze.all||!freeze.targets.empty())&&bleach_engine_active&&(!bleach_engine_update||between_updates))
            for(uint32_t t:freeze.all?std::vector<uint32_t>{(uint32_t)ctx->ctr}:freeze.targets)if(t==(uint32_t)ctx->ctr&&t!=0x00061E30u) {
                freeze_object=(uint32_t)ctx->gpr[3]; freeze_target=t;
                freeze_saved.assign(vm_base+freeze_object,vm_base+freeze_object+freeze.bytes);
                break;
            }
    }
    const bool was_half=bleach_in_half_task;
    if(half_run)bleach_in_half_task=true;
    // a task that ignores the game's sub-steps but runs every tick: its state updates keep 1/r
    const bool was_clock=bleach_clock_task_context;
    if(site==0x0027BBFC)bleach_clock_task_context=!half_run&&bleach_clock_task((uint32_t)ctx->ctr);
    const unsigned census_outer=bleach_task_substep_calls; bleach_task_substep_calls=0;
    // BLEACH_WATCH=<addr>: report the callbacks that change that word (debug).
    static const uint32_t watch=[] {const char* v=getenv("BLEACH_WATCH");return v?(uint32_t)strtoul(v,nullptr,16):0u;}();
    const uint32_t watch_before=watch?vm_read32(watch):0, watch_target=(uint32_t)ctx->ctr, watch_object=(uint32_t)ctx->gpr[3];
    ps3_indirect_call(ctx);
    while (g_trampoline_fn) {
        void (*next)(void*)=g_trampoline_fn;
        g_trampoline_fn=nullptr;
        next(ctx);
    }
    bleach_in_half_task=was_half;
    bleach_clock_task_context=was_clock;
    // BLEACH_TASK_CENSUS=1 (debug): per update target, runs, runs that asked for sub-steps
    // (func_0002D9F4) and whether BLEACH_TASK_HALF holds it; printed at exit.
    {
        static const bool census=[] {const char* e=getenv("BLEACH_TASK_CENSUS");return e&&!strcmp(e,"1");}();
        if(census&&site==0x0027BBFC&&bleach_engine_active) {
            struct Count {uint64_t runs,substeps,half;};
            static std::map<uint32_t,Count>* counts=[] {
                auto* m=new std::map<uint32_t,Count>;
                atexit([] {
                    for(auto& kv:*counts)fprintf(stderr,"[task-census] %08x runs=%llu substep_runs=%llu half_runs=%llu\n",
                        kv.first,(unsigned long long)kv.second.runs,(unsigned long long)kv.second.substeps,(unsigned long long)kv.second.half);
                });
                return m;
            }();
            Count& c=(*counts)[watch_target];
            ++c.runs; if(bleach_task_substep_calls)++c.substeps; if(half_run)++c.half;
        }
    }
    if(site==0x0027BBFC&&bleach_task_substep_calls&&bleach_task_substep_users.insert(watch_target).second)
        fprintf(stderr,"[task-auto] %08x asks for sub-steps: every tick\n",watch_target);
    bleach_task_substep_calls=census_outer+bleach_task_substep_calls;
    if(watch&&vm_read32(watch)!=watch_before)
        fprintf(stderr,"[watch] tick=%llu site=%08x target=%08x object=%08x %08x->%08x\n",
            (unsigned long long)bleach_native_updates.load(std::memory_order_relaxed),site,watch_target,watch_object,watch_before,vm_read32(watch));
    if(freeze_object) {
        const auto& freeze=bleach_freeze_config();
        if(freeze.audit) {
            if(bleach_freeze_audits.empty())atexit(bleach_freeze_report);
            BleachFreezeAudit* a=nullptr;
            for(auto& x:bleach_freeze_audits)if(x.target==freeze_target){a=&x;break;}
            if(!a){bleach_freeze_audits.push_back({});a=&bleach_freeze_audits.back();a->target=freeze_target;a->changes.assign(freeze.bytes/4,0);}
            ++a->calls;
            for(uint32_t w=0;w<freeze.bytes/4;w++)
                if(memcmp(&freeze_saved[w*4],vm_base+freeze_object+w*4,4))++a->changes[w];
        } else memcpy(vm_base+freeze_object,freeze_saved.data(),freeze.bytes);
    }
#ifdef _WIN32
    if (capture) {
        QueryPerformanceCounter(&end);
        bleach_stage_parent=parent;
        AcquireSRWLockExclusive(&bleach_stage_lock);
        unsigned i;
        for (i=0;i<bleach_stage_count;i++) {
            const auto& s=bleach_stage_samples[i];
            if (s.site==site && s.target==target && s.object==object && s.mode==mode && s.parent==parent) break;
        }
        if (i<2048) {
            if (i==bleach_stage_count) {
                ++bleach_stage_count;
                bleach_stage_samples[i]={site,target,object,mode,parent,0,0,0,f1};
            }
            auto& s=bleach_stage_samples[i];
            const uint64_t elapsed=end.QuadPart-start.QuadPart;
            ++s.calls; s.ticks+=elapsed;
            if (elapsed>s.max_ticks) s.max_ticks=elapsed;
        } else ++bleach_stage_overflow;
        ReleaseSRWLockExclusive(&bleach_stage_lock);
    }
#endif
}
#endif
