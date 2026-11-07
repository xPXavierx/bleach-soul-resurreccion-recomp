#ifndef BLEACH_NATIVE_ENGINE_SCHEDULE_H
#define BLEACH_NATIVE_ENGINE_SCHEDULE_H
#include "native_frame_clock.h"
#include <cstdlib>

/* Simulation (game update) rate. The title ships at 30 Hz; BLEACH_SIMULATION_HZ
 * (30..240, a divisor-friendly integer) is an experiment to learn whether the
 * engine scales its updates by elapsed time or advances a fixed step per tick. */
static inline unsigned bleach_simulation_hz() {
    static const unsigned hz=[] {
        const char* e=getenv("BLEACH_SIMULATION_HZ");
        const unsigned v=e?(unsigned)atoi(e):30u;
        return v>=30&&v<=240?v:30u;
    }();
    return hz;
}

/* Two independent rational timelines. Missed rendering slots are discarded,
 * but simulation debt is retained: no multiplication of per-tick movement or
 * timers, and no artificial vblank pulses. Catch-up presently executes one
 * tick per scene build; multiple simulation ticks per build is future work. */
struct BleachEngineSchedule {
    BleachClockDeadline render, simulation;
    uint64_t frames=0, ticks=0, skipped_render_slots=0;
    BleachEngineSchedule(uint64_t now,uint64_t frequency,uint32_t rate)
        :render(now,frequency,rate),simulation(now,frequency,bleach_simulation_hz()){}
    bool begin(uint64_t now) {
        ++frames;
        bool update=now>=simulation.deadline;
        if(update){simulation.advance();++ticks;}
        render.advance();
        while(render.deadline<=now){render.advance();++skipped_render_slots;}
        return update;
    }
};

/* Buffer-driven dispatch keeps simulation debt independent of presentation.
 * A busy renderer never advances the render deadline. Simulation can still
 * execute, using the game's existing rejected-submission cleanup path. */
struct BleachEngineDispatchSchedule {
    BleachClockDeadline render, simulation;
    bool unlimited;
    uint64_t frames=0, ticks=0, skipped_render_slots=0;
    BleachEngineDispatchSchedule(uint64_t now,uint64_t frequency,uint32_t rate)
        :render(now,frequency,rate?rate:1),simulation(now,frequency,bleach_simulation_hz()),unlimited(!rate){}
    bool render_due(uint64_t now) const {return unlimited||now>=render.deadline;}
    bool runnable(uint64_t now,bool ready) const {
        return now>=simulation.deadline||(ready&&render_due(now));
    }
    uint64_t wake_deadline(bool ready) const {
        if(!ready)return simulation.deadline;
        if(unlimited)return 0;
        return render.deadline<simulation.deadline?render.deadline:simulation.deadline;
    }
    bool begin(uint64_t now) {
        ++frames;
        if(now<simulation.deadline)return false;
        simulation.advance();++ticks;return true;
    }
    void submitted(uint64_t now) {
        if(unlimited)return;
        render.advance();
        while(render.deadline<=now){render.advance();++skipped_render_slots;}
    }
};
#endif
