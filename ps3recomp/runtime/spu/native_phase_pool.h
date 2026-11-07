#ifndef PS3RECOMP_NATIVE_PHASE_POOL_H
#define PS3RECOMP_NATIVE_PHASE_POOL_H
#include "../platform/win32_compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <mutex>

/* Host workers calculate privately; an ordered per-lane lease covers every
 * shared side effect. The executor MUST call shared_begin() before its first
 * shared access. Task admission is contiguous, so cancellation cannot leave a
 * missing ordinal ahead of an already admitted task. One batch at a time per
 * pool; independent pools may run concurrently. No title/guest addresses here. */
namespace ps3_native {
class PhasePool {
public:
    struct Batch {
        void* user;
        unsigned count, lanes;
        void (*run)(void*,unsigned index,unsigned lane);
        bool (*cancelled)(void*);
    };
    struct Stats {
        uint64_t batches=0, admitted=0, completed=0, lease_waits=0;
        unsigned peak_inflight=0;
    };
private:
    struct Token { PhasePool* pool; unsigned lane, ordinal; bool leased; };
    static inline thread_local Token* current=nullptr;
    SRWLOCK lock=SRWLOCK_INIT;
    CONDITION_VARIABLE wake=CONDITION_VARIABLE_INIT;
    CONDITION_VARIABLE drained=CONDITION_VARIABLE_INIT;
    std::mutex execution;
    HANDLE threads[32]{};
    unsigned workers=0, next=0, pending=0, inflight=0, turns[6]{};
    uint64_t generation=0;
    bool stopping=false;
    const Batch* batch=nullptr;
    Stats counters;
    static DWORD WINAPI entry(void* user) {
#ifdef _WIN32
        ULONG guarantee=256*1024;SetThreadStackGuarantee(&guarantee);
#endif
        static_cast<PhasePool*>(user)->worker();return 0;
    }
    void worker() {
        uint64_t seen=0;
        AcquireSRWLockExclusive(&lock);
        for(;;) {
            while(!stopping && seen==generation)
                SleepConditionVariableSRW(&wake,&lock,INFINITE,0);
            if(stopping)break;
            seen=generation;
            const Batch* work=batch;
            for(;;) {
                if(next==work->count || (work->cancelled && work->cancelled(work->user)))break;
                unsigned index=next++;
                ++counters.admitted;++inflight;
                if(inflight>counters.peak_inflight)counters.peak_inflight=inflight;
                ReleaseSRWLockExclusive(&lock);
                Token token{this,index%work->lanes,index/work->lanes,false};
                Token* previous=current;current=&token;
                work->run(work->user,index,token.lane);
                // Even an empty task advances its ordinal in order.
                shared_begin();
                AcquireSRWLockExclusive(&lock);
                if(turns[token.lane]!=token.ordinal)abort();
                ++turns[token.lane];--inflight;++counters.completed;
                current=previous;
                WakeAllConditionVariable(&wake);
            }
            if(!--pending)WakeAllConditionVariable(&drained);
        }
        ReleaseSRWLockExclusive(&lock);
    }
public:
    explicit PhasePool(unsigned count):workers(count) {
        if(!count || count>32)abort();
        for(unsigned i=0;i<count;i++) {
            threads[i]=CreateThread(nullptr,4u*1024*1024,entry,this,0,nullptr);
            if(!threads[i]){fprintf(stderr,"[native-pool] worker creation failed\n");abort();}
        }
    }
    PhasePool(const PhasePool&)=delete;
    PhasePool& operator=(const PhasePool&)=delete;
    ~PhasePool() {
        std::lock_guard<std::mutex> single(execution);
        AcquireSRWLockExclusive(&lock);stopping=true;
        WakeAllConditionVariable(&wake);ReleaseSRWLockExclusive(&lock);
        for(unsigned i=0;i<workers;i++){WaitForSingleObject(threads[i],INFINITE);CloseHandle(threads[i]);}
    }
    void run(const Batch& work) {
        if(!work.run || !work.lanes || work.lanes>6)abort();
        std::lock_guard<std::mutex> single(execution);
        AcquireSRWLockExclusive(&lock);
        if(batch || inflight || stopping)abort();
        for(auto& turn:turns)turn=0;
        next=0;pending=workers;batch=&work;++generation;++counters.batches;
        WakeAllConditionVariable(&wake);
        while(pending)SleepConditionVariableSRW(&drained,&lock,INFINITE,0);
        if(inflight)abort();
        batch=nullptr;ReleaseSRWLockExclusive(&lock);
    }
    static void shared_begin() {
        Token* token=current;
        if(!token || token->leased)return;
        PhasePool* p=token->pool;
        AcquireSRWLockExclusive(&p->lock);
        if(p->turns[token->lane]!=token->ordinal)++p->counters.lease_waits;
        while(p->turns[token->lane]!=token->ordinal)
            SleepConditionVariableSRW(&p->wake,&p->lock,INFINITE,0);
        token->leased=true;
        ReleaseSRWLockExclusive(&p->lock);
    }
    static bool in_task() { return current!=nullptr; }
    Stats stats() {
        AcquireSRWLockExclusive(&lock);Stats result=counters;
        ReleaseSRWLockExclusive(&lock);return result;
    }
};
}
#endif
