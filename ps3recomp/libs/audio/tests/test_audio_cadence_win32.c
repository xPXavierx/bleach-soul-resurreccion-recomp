/* Real WASAPI + the production mixer, with a deterministic synthetic producer.
 * The producer needs 1 ms per block. Batched host callbacks must not cause the
 * mixer to consume its next block before that work can finish. No game data.
 * Build on Windows with LLVM-MinGW, -I include, -lole32, -ffunction-sections
 * -fdata-sections -Wl,--gc-sections. The output device is muted, not disabled.
 */
#include "../cellAudio.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static HANDLE test_events;
static volatile LONG test_running;
static LARGE_INTEGER test_freq, test_start, test_last;
static unsigned test_blocks, test_silent, test_bursts;
static double test_min_ms = 1000.0;

uint32_t sys_event_find_queue_by_key(uint64_t key) { return key ? 1 : 0; }
uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size) { (void)key; (void)size; return 1; }
int sys_event_queue_push_by_id(uint32_t id, uint64_t src, uint64_t a, uint64_t b, uint64_t c)
{
    (void)id; (void)src; (void)a; (void)b; (void)c;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (now.QuadPart - test_start.QuadPart > test_freq.QuadPart / 2) {
        const double ms = 1000.0 * (now.QuadPart - test_last.QuadPart) / test_freq.QuadPart;
        ++test_blocks;
        if (s_mix_buffer[0] == 0.0f) ++test_silent;
        if (ms < 2.0) ++test_bursts;
        if (ms < test_min_ms) test_min_ms = ms;
    }
    test_last = now;
    ReleaseSemaphore(test_events, 1, NULL);
    return 0;
}

static DWORD WINAPI test_producer(void* unused)
{
    (void)unused;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 2, TIMER_ALL_ACCESS);
    assert(timer);
    while (InterlockedCompareExchange(&test_running, 1, 1)) {
        if (WaitForSingleObject(test_events, 50) != WAIT_OBJECT_0) continue;
        mutex_lock(&s_audio_mutex);
        const u32 slot = (u32)vm_read64(16);
        mutex_unlock(&s_audio_mutex);
        LARGE_INTEGER due; due.QuadPart = -10000; /* 1 ms production, no spin */
        assert(SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE));
        WaitForSingleObject(timer, INFINITE);
        mutex_lock(&s_audio_mutex);
        u32* dst = (u32*)s_ports[0].buffer + slot * 512;
        for (unsigned i = 0; i < 512; ++i) dst[i] = 0x0000003e; /* BE 0.125f */
        mutex_unlock(&s_audio_mutex);
    }
    CloseHandle(timer);
    return 0;
}

int main(void)
{
    _putenv("PS3_MUTE=1");
    _putenv("PS3_AUDIO_LEAD=0");
    _putenv("AUDIO_RATE=1");
    vm_base = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(vm_base);
    mutex_init(&s_audio_mutex);
    s_ports[0].in_use = s_ports[0].running = 1;
    s_ports[0].param.nChannel = 2;
    s_ports[0].param.nBlock = 8;
    s_ports[0].param.level = 1.0f;
    s_ports[0].read_idx_addr = 16;
    s_ports[0].buffer = calloc(8 * 512, sizeof(float));
    assert(s_ports[0].buffer);
    s_notify_queues[0].in_use = 1;
    s_notify_queues[0].key = 1;
    assert(audio_backend_init() == 0);
    test_events = CreateSemaphoreW(NULL, 0, 1024, NULL);
    assert(test_events);
    QueryPerformanceFrequency(&test_freq);
    QueryPerformanceCounter(&test_start); test_last = test_start;
    test_running = 1;
    HANDLE producer = CreateThread(NULL, 0, test_producer, NULL, 0, NULL);
    assert(producer && audio_start_mix_thread() == 0);
    Sleep(3000);
    audio_stop_mix_thread();
    InterlockedExchange(&test_running, 0);
    ReleaseSemaphore(test_events, 1, NULL);
    WaitForSingleObject(producer, INFINITE);
    audio_backend_shutdown();
    printf("cadence: blocks=%u silent=%u burst_lt_2ms=%u min_interval_ms=%.3f underruns=%llu\n",
           test_blocks, test_silent, test_bursts, test_min_ms, g_audio_underruns);
    CloseHandle(producer); CloseHandle(test_events);
    free(s_ports[0].buffer); mutex_destroy(&s_audio_mutex);
    VirtualFree(vm_base, 0, MEM_RELEASE);
    /* Permit a rare host scheduling interruption, never repeated bursts. */
    return test_blocks > 400 && test_silent <= 2 && test_bursts <= 2 ? 0 : 1;
}
