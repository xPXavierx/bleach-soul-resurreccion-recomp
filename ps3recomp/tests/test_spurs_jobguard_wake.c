/* Windows: clang -std=gnu17 -O2 -ffunction-sections -fdata-sections -I include
 * -I runtime/spu tests/test_spurs_jobguard_wake.c tests/spurs_jobguard_unused_stubs.c
 * -Wl,--gc-sections -o guard-test.exe
 * Exercises the real guard/notify/shutdown implementation without game code. */
#include "../libs/spurs/cellSpurs.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size=65536;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi;
void ppu_resv_break_store(uint64_t ea){(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
int spu_coh_is_reserved(uint32_t a){(void)a;return 0;}
void spu_coh_notify_write(uint32_t a){(void)a;}
void spu_lockline_lock(void){}
void spu_lockline_unlock(void){}
static HANDLE entered;
static u32 guard=0x1000, iterations=1;
static volatile LONG completed;
static DWORD WINAPI waiter(void* unused){
    (void)unused;SetEvent(entered);
    for(u32 i=0;i<iterations;i++){
        if(!jg_wait(guard,0))return 0;
        InterlockedIncrement(&completed);
    }
    return 1;
}
static HANDLE start(void){
    ResetEvent(entered);completed=0;s_jobchains[0].stop_requested=0;
    HANDLE t=CreateThread(NULL,0,waiter,NULL,0,NULL);assert(t);
    assert(WaitForSingleObject(entered,2000)==WAIT_OBJECT_0);return t;
}
static void join(HANDLE t,DWORD expected){
    assert(WaitForSingleObject(t,3000)==WAIT_OBJECT_0);
    DWORD code=9;assert(GetExitCodeThread(t,&code));assert(code==expected);CloseHandle(t);
}
int main(void){
    vm_base=calloc(1,ppu_vm_size);assert(vm_base);
    entered=CreateEventW(NULL,TRUE,FALSE,NULL);assert(entered);
    s_jobchains[0].jc_ea=0x2000;
    assert(!cellSpursJobGuardInitialize(0x2000,guard,2,1,1));
    assert(!cellSpursJobGuardNotify(guard));assert(!cellSpursJobGuardNotify(guard));
    assert(jg_wait(guard,0)==1 && s_jobguards[0].pending==0);
    iterations=1024;HANDLE t=start();
    for(u32 i=0;i<2048;i++)assert(!cellSpursJobGuardNotify(guard));
    join(t,1);assert(completed==1024 && s_jobguards[0].pending==0);
    iterations=1;assert(!cellSpursJobGuardNotify(guard));assert(!cellSpursJobGuardReset(guard));
    t=start();assert(WaitForSingleObject(t,20)==WAIT_TIMEOUT);
    assert(!cellSpursJobGuardNotify(guard));assert(WaitForSingleObject(t,20)==WAIT_TIMEOUT);
    assert(!cellSpursJobGuardNotify(guard));join(t,1);
    assert(!cellSpursJobGuardInitialize(0x2000,0x1100,1,1,1));
    t=start();assert(!cellSpursJobGuardNotify(0x1100));assert(WaitForSingleObject(t,20)==WAIT_TIMEOUT);
    assert(!cellSpursShutdownJobChain(0x2000));join(t,0);assert(completed==0);
    /* Stop racing entry/check/park must always release the waiter. */
    for(unsigned i=0;i<128;i++){t=start();assert(!cellSpursShutdownJobChain(0x2000));join(t,0);}
    CloseHandle(entered);free(vm_base);
    puts("job guard: queued credits, threshold, reset, unrelated wake and 128 shutdown races passed");
    return 0;
}
