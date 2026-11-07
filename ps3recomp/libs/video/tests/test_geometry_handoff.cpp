#include "rsx_geometry_handoff.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <thread>
#include <cstdio>
int main() {
    assert(rsx_geometry_handoff_init(8,256));
    rsx_geometry_lease all[8];
    for(auto& x:all){x=rsx_geometry_handoff_reserve();assert(x.token);}
    assert(!rsx_geometry_handoff_reserve().token);
    uint64_t old=all[0].token;rsx_geometry_handoff_cancel(old);
    auto replacement=rsx_geometry_handoff_reserve();assert(replacement.token!=old);
    rsx_geometry_handoff_cancel(old); // must not cancel the replacement
    assert(!rsx_geometry_handoff_reserve().token);
    rsx_geometry_handoff_cancel(replacement.token);
    for(unsigned i=1;i<8;i++)rsx_geometry_handoff_cancel(all[i].token);
    std::atomic<uint64_t> inbox[4]{};
    std::thread workers[4];
    constexpr unsigned iterations=5000;
    for(unsigned w=0;w<4;w++)workers[w]=std::thread([&,w] {
        for(unsigned n=0;n<iterations;n++) {
            rsx_geometry_lease x;
            do{x=rsx_geometry_handoff_reserve();if(!x.token)std::this_thread::yield();}while(!x.token);
            x.bytes=256;x.command_ea=0x10000+w*16;x.vertices=w;x.input_indices=n;
            memset(x.data,(unsigned char)n,256);assert(rsx_geometry_handoff_publish(&x));
            // Ownership transferred: writer cancellation cannot affect ready data.
            rsx_geometry_handoff_cancel(x.token);
            while(inbox[w].load())std::this_thread::yield();
            inbox[w].store(x.token);
        }
    });
    unsigned received[4]={};
    for(unsigned total=0;total<4*iterations;)for(unsigned w=0;w<4;w++) {
        uint64_t token=inbox[w].exchange(0);if(!token)continue;
        rsx_geometry_lease x{};
        assert(!rsx_geometry_handoff_claim(token,0xdeadbeef,&x));
        assert(rsx_geometry_handoff_claim(token,0x10000+w*16,&x));
        assert(x.vertices==w && x.input_indices==received[w]);
        for(unsigned i=0;i<256;i++)assert(x.data[i]==(unsigned char)received[w]);
        rsx_geometry_handoff_cancel(token); // reading lease remains owned
        assert(!rsx_geometry_handoff_claim(token,0x10000+w*16,&x));
        rsx_geometry_handoff_release(token);
        assert(!rsx_geometry_handoff_claim(token,0x10000+w*16,&x));
        rsx_geometry_handoff_release(old);old=token;
        ++received[w];++total;
    }
    for(auto& w:workers)w.join();
    rsx_geometry_handoff_destroy();
    assert(rsx_geometry_handoff_init(1,256));
    auto x=rsx_geometry_handoff_reserve();assert(x.token!=old);
    rsx_geometry_handoff_cancel(old);rsx_geometry_handoff_cancel(x.token);
    rsx_geometry_handoff_destroy();
    puts("PASS 20000 handoffs, four producers, payload ownership, exhaustion, wrong EA, stale/replayed tokens, cancellation, reinitialization.");
}
