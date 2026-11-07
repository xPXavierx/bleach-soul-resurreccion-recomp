#include "../rsx_fifo_wait.h"
#include "ps3emu/ps3_write_watch.h"
#include <windows.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
unsigned char* vm_base;
static HANDLE go,done;
static void put(unsigned ea,unsigned value) { value=__builtin_bswap32(value);memcpy(vm_base+ea,&value,4); }
static DWORD WINAPI writer(void* ignored)
{
    (void)ignored;
    for(unsigned i=0;i<1000;i++) {
        assert(WaitForSingleObject(go,5000)==WAIT_OBJECT_0);
        put(32,i+1);ps3_write_watch_commit(32,4,PS3_WRITE_DMA);SetEvent(done);
    }
    return 0;
}
int main(int argc,char** argv)
{
    assert(argc==2);char env[1200];snprintf(env,sizeof env,"GCM_FIFO_WAIT_AUDIT=0:10000:%s",argv[1]);
    assert(!_putenv(env));vm_base=calloc(256,1);rsx_fifo_wait_init(NULL);
    go=CreateEventA(NULL,FALSE,FALSE,NULL);done=CreateEventA(NULL,FALSE,FALSE,NULL);
    HANDLE worker=CreateThread(NULL,0,writer,NULL,0,NULL);assert(worker);
    for(unsigned i=0;i<1000;i++) {
        rsx_fifo_wait_arm(RSX_WAIT_PARK,32,i,0,i);
        uint64_t started=rsx_fifo_wait_begin();SetEvent(go);
        assert(WaitForSingleObject(done,5000)==WAIT_OBJECT_0);
        rsx_fifo_wait_end(started,0);rsx_fifo_wait_resume(i);
        assert(!__atomic_load_n(&g_ps3_write_watch.addr,__ATOMIC_ACQUIRE));
    }
    assert(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0);CloseHandle(worker);
    /* Equal-ready label, overlapping range, unrelated store and timeout. */
    put(64,0);rsx_fifo_wait_arm(RSX_WAIT_LABEL,64,7,1,1000);
    ps3_write_watch_commit(0xfffffff0u,32,PS3_WRITE_PPU); /* must not wrap */
    ps3_write_watch_commit(68,4,PS3_WRITE_PPU); /* adjacent is not overlapping */
    uint64_t started=rsx_fifo_wait_begin();put(64,7);
    ps3_write_watch_commit(63,2,PS3_WRITE_ATOMIC);
    HANDLE never=CreateEventA(NULL,FALSE,FALSE,NULL);
    assert(WaitForSingleObject(never,3)==WAIT_TIMEOUT);
    rsx_fifo_wait_end(started,1);rsx_fifo_wait_resume(1000);
    /* Changed before arm and unobserved writer are distinct, not zero delay. */
    put(64,8);rsx_fifo_wait_arm(RSX_WAIT_PUT,64,7,0,1001);rsx_fifo_wait_resume(1001);
    put(64,0);rsx_fifo_wait_arm(RSX_WAIT_PUT,64,0,0,1002);
    put(64,9);rsx_fifo_wait_resume(1002);
    /* A transient ready value must not be attributed to later consumption. */
    put(64,0);rsx_fifo_wait_arm(RSX_WAIT_LABEL,64,10,1,1003);
    put(64,10);ps3_write_watch_commit(64,4,PS3_WRITE_HLE);
    put(64,0);ps3_write_watch_commit(64,4,PS3_WRITE_HLE);
    rsx_fifo_wait_resume(1003);assert(g_ps3_write_watch.addr==64);
    put(64,10);ps3_write_watch_commit(64,4,PS3_WRITE_HLE);rsx_fifo_wait_resume(1003);
    /* Unfulfilled dependency replaced by another and exit while blocked. */
    put(64,0);rsx_fifo_wait_arm(RSX_WAIT_LABEL,64,10,1,1004);
    rsx_fifo_wait_arm(RSX_WAIT_LABEL,96,11,1,1004);
    CloseHandle(go);CloseHandle(done);CloseHandle(never);
    puts("PASS cross-thread publication observations, predicates, ranges and unknown paths; inspect CSV counts");
    return 0; /* retain guest allocation until diagnostic atexit dump */
}
