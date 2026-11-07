#include "../native_phase_pool.h"
#include <atomic>
#include <cassert>
#include <vector>
#include <thread>
#include <cstdio>
using ps3_native::PhasePool;
struct Fixture {
    struct Slot {std::atomic<unsigned> ready{0};uint64_t payload=0;};
    Slot ring[4][2];
    std::atomic<unsigned> private_active{0},private_peak{0},initial{0},done{0};
    std::atomic<unsigned> owners[4]{};
    unsigned turn[4]{};
    unsigned calls[512]{};
    bool gate=false,cancel=false,empty=false;
    unsigned count=512;
    static uint64_t value(unsigned index) {return (uint64_t(index+13)*0x123456789ull)^0x987654321abcdef0ull;}
    static bool cancelled(void* ptr) {
        auto& f=*static_cast<Fixture*>(ptr);return f.cancel && f.done.load()>=37;
    }
    static void run(void* ptr,unsigned index,unsigned lane) {
        auto& f=*static_cast<Fixture*>(ptr);
        unsigned active=f.private_active.fetch_add(1)+1,peak=f.private_peak.load();
        while(active>peak && !f.private_peak.compare_exchange_weak(peak,active)){}
        if(f.gate && index<8) {
            f.initial.fetch_add(1);
            while(f.initial.load()!=8)std::this_thread::yield();
        }
        uint64_t computed=value(index);
        if(!index)Sleep(2); // Later same-lane tasks reach the lease first.
        f.private_active.fetch_sub(1);
        if(!f.empty) {
            PhasePool::shared_begin();PhasePool::shared_begin(); // Idempotent.
            assert(f.owners[lane].fetch_add(1)==0);
            assert(f.turn[lane]++==index/4);
            if(!f.cancel) {
                auto& slot=f.ring[lane][(index/4)%2];
                while(slot.ready.load(std::memory_order_acquire))std::this_thread::yield();
                slot.payload=computed;
                slot.ready.store(index+1,std::memory_order_release);
            }
            assert(f.owners[lane].fetch_sub(1)==1);
        }
        ++f.calls[index];f.done.fetch_add(1);
    }
    void consume() {
        for(unsigned index=0;index<count;index++) {
            auto& slot=ring[index%4][(index/4)%2];
            while(slot.ready.load(std::memory_order_acquire)!=index+1)std::this_thread::yield();
            assert(slot.payload==value(index));
            slot.ready.store(0,std::memory_order_release);
        }
    }
};
int main() {
    {
        PhasePool pool(8);
        Fixture ring;ring.gate=true;
        std::thread consumer([&]{ring.consume();});
        pool.run({&ring,ring.count,4,Fixture::run,Fixture::cancelled});consumer.join();
        for(auto call:ring.calls)assert(call==1);
        assert(ring.private_peak.load()==8);
        auto stats=pool.stats();assert(stats.completed==512 && stats.peak_inflight==8 && stats.lease_waits>0);
        puts("PASS private concurrency >4; ordered exclusive publication; ring wrap; exact payloads; no missing/duplicate tasks");
        Fixture cancel;cancel.cancel=true;
        pool.run({&cancel,cancel.count,4,Fixture::run,Fixture::cancelled});
        unsigned n=cancel.done.load();assert(n>=37 && n<512);
        for(unsigned i=0;i<512;i++)assert(cancel.calls[i]==unsigned(i<n));
        puts("PASS cancellation drains a contiguous admitted prefix");
        Fixture empty;empty.empty=true;
        pool.run({&empty,empty.count,4,Fixture::run,Fixture::cancelled});
        assert(empty.done.load()==512);
        puts("PASS no-shared-access tasks advance ordered leases; pool reusable after cancellation");
        Fixture precancel;precancel.cancel=true;precancel.done=37;
        pool.run({&precancel,512,4,Fixture::run,Fixture::cancelled});
        for(auto call:precancel.calls)assert(call==0);
        stats=pool.stats();assert(stats.admitted==stats.completed);
        pool.run({&empty,0,4,Fixture::run,Fixture::cancelled});
        puts("PASS cancellation before admission; empty batch; admitted equals completed");
    }
    puts("PASS worker shutdown joined");
}
