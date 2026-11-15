/* Windows: clang -std=gnu17 -O2 -I include -I runtime/spu
 * runtime/spu/tests/test_spu_lockline_contention_win32.c -o lockline-test.exe
 * Verify blocking handoff and exclusion using the production lock. */
#include "../spu_coherency.c"
#include <assert.h>
void spu_ch_wake(spu_context* ctx) { (void)ctx; }
static HANDLE attempted, completed, start;
static unsigned counter;
static volatile LONG inside;
static DWORD WINAPI blocked_waiter(void* unused)
{
    (void)unused;
    SetEvent(attempted);
    spu_lockline_lock();
    counter++;
    spu_lockline_unlock();
    SetEvent(completed);
    return 0;
}
static DWORD WINAPI incrementer(void* unused)
{
    (void)unused;
    assert(WaitForSingleObject(start, 2000) == WAIT_OBJECT_0);
    for (unsigned i=0; i<50000; ++i) {
        spu_lockline_lock();
        assert(InterlockedIncrement(&inside) == 1);
        counter++;
        assert(InterlockedDecrement(&inside) == 0);
        spu_lockline_unlock();
    }
    return 0;
}
int main(void)
{
    attempted=CreateEventW(NULL,TRUE,FALSE,NULL);
    completed=CreateEventW(NULL,TRUE,FALSE,NULL);
    start=CreateEventW(NULL,TRUE,FALSE,NULL);
    assert(attempted && completed && start);
    spu_lockline_lock();
    HANDLE waiter=CreateThread(NULL,0,blocked_waiter,NULL,0,NULL);
    assert(waiter);
    assert(WaitForSingleObject(attempted,2000)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(completed,30)==WAIT_TIMEOUT);
    assert(counter==0);
    spu_lockline_unlock();
    assert(WaitForSingleObject(waiter,2000)==WAIT_OBJECT_0);
    assert(counter==1);
    CloseHandle(waiter);
    HANDLE workers[4];
    for (unsigned i=0;i<4;++i) {
        workers[i]=CreateThread(NULL,0,incrementer,NULL,0,NULL);
        assert(workers[i]);
    }
    SetEvent(start);
    assert(WaitForMultipleObjects(4,workers,TRUE,5000)==WAIT_OBJECT_0);
    assert(counter==200001 && inside==0);
    for (unsigned i=0;i<4;++i) CloseHandle(workers[i]);
    CloseHandle(attempted);CloseHandle(completed);CloseHandle(start);
    puts("PASS: blocked handoff and 200000 concurrent lock-line transactions");
    return 0;
}
