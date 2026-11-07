#include "../rsx_fifo_wait.h"
#include "ps3emu/ps3_write_watch.h"
#include <windows.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
unsigned char* vm_base;
static HANDLE kick,go,done;
static unsigned mode;
static void wake(void){SetEvent(kick);}
static void put(unsigned ea,unsigned v){v=__builtin_bswap32(v);memcpy(vm_base+ea,&v,4);}
static DWORD WINAPI writer(void* ignored)
{
    (void)ignored;
    for(unsigned i=0;i<12000;i++) {
        assert(WaitForSingleObject(go,5000)==WAIT_OBJECT_0);
        put(mode==3?64:32,i+1);
        ps3_write_watch_commit(mode==3?64:32,4,i&1?PS3_WRITE_DMA:PS3_WRITE_ATOMIC);
        SetEvent(done);
    }
    return 0;
}
int main(void)
{
    assert(!_putenv("GCM_FIFO_PUBLISH_WAKE=1"));assert(!_putenv("GCM_FIFO_WAIT_AUDIT="));
    vm_base=calloc(256,1);kick=CreateEventA(NULL,FALSE,FALSE,NULL);
    go=CreateEventA(NULL,FALSE,FALSE,NULL);done=CreateEventA(NULL,FALSE,FALSE,NULL);
    rsx_fifo_wait_init(wake);
    HANDLE worker=CreateThread(NULL,0,writer,NULL,0,NULL);assert(worker);
    for(unsigned i=0;i<12000;i++) {
        ResetEvent(kick);put(32,0);put(64,0);mode=i%4;
        if(mode==0) { /* published before the watch exists */
            SetEvent(go);assert(WaitForSingleObject(done,5000)==WAIT_OBJECT_0);
            rsx_fifo_wait_arm(RSX_WAIT_LABEL,32,i+1,1,i);
        } else if(mode==1) { /* publication while the consumer is entering wait */
            rsx_fifo_wait_arm(RSX_WAIT_LABEL,32,i+1,1,i);SetEvent(go);
        } else if(mode==2) { /* race between watch publication and producer commit */
            SetEvent(go);rsx_fifo_wait_arm(RSX_WAIT_PARK,32,0,0,i);
        } else { /* stale callback must inspect the replacement dependency */
            rsx_fifo_wait_arm(RSX_WAIT_LABEL,32,i+1,1,i);
            rsx_fifo_wait_arm(RSX_WAIT_LABEL,64,i+1,1,i);SetEvent(go);
        }
        assert(WaitForSingleObject(kick,500)==WAIT_OBJECT_0);
        if(mode)assert(WaitForSingleObject(done,5000)==WAIT_OBJECT_0);
        rsx_fifo_wait_resume(i);assert(g_ps3_write_watch.addr==0);
    }
    assert(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0);
    ResetEvent(kick);put(32,0);rsx_fifo_wait_arm(RSX_WAIT_LABEL,32,99,1,12000);
    put(32,98);ps3_write_watch_commit(32,4,PS3_WRITE_DMA);
    assert(WaitForSingleObject(kick,0)==WAIT_TIMEOUT); /* do not wake before ready */
    put(32,99);ps3_write_watch_commit(32,4,PS3_WRITE_DMA);
    Sleep(2);assert(WaitForSingleObject(kick,0)==WAIT_OBJECT_0); /* signal retained */
    rsx_fifo_wait_resume(12000);
    CloseHandle(worker);CloseHandle(kick);CloseHandle(go);CloseHandle(done);
    puts("PASS 12000 DMA/atomic races: publish before arm, before/during wait, replacement, predicates and retained event");
    return 0;
}
