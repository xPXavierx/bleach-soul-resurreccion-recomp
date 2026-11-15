/* Exercise the real mixer across ring wraps, without starting a device/thread.
 * macOS: clang -std=gnu17 -I include -I /opt/homebrew/include
 *   libs/audio/tests/test_audio_ring.c -L /opt/homebrew/lib -lSDL2
 *   -Wl,-dead_strip -o /tmp/test_audio_ring && /tmp/test_audio_ring
 * Windows: clang -std=gnu17 -O2 -I include -ffunction-sections -fdata-sections
 *   libs/audio/tests/test_audio_ring.c -Wl,--gc-sections -lole32 -o test_audio_ring.exe
 * Run in separate processes with PS3_AUDIO_LEAD=0, 4, 100, -1 to exercise
 * default, lookahead, upper clamp and negative-input handling, respectively.
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
#ifdef _WIN32
uint32_t sys_event_find_queue_by_key(uint64_t key) { (void)key; return 0; }
uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size) { (void)key; (void)size; return 0; }
int sys_event_queue_push_by_id(uint32_t q, uint64_t s, uint64_t a, uint64_t b, uint64_t c)
{ (void)q; (void)s; (void)a; (void)b; (void)c; return 0; }
#endif

int main(void)
{
#ifdef _WIN32
    vm_base = VirtualAlloc(NULL, 0x100000000ULL, MEM_RESERVE, PAGE_NOACCESS);
    assert(vm_base);
    assert(vm_commit(0, 4096) == CELL_OK);
    assert(vm_commit(0x60000000, 4096) == CELL_OK);
#else
    vm_base = mmap(NULL, 0x100000000ULL, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);
#endif
    mutex_init(&s_audio_mutex);
    for (unsigned blocks = 8; blocks <= 32; blocks *= 2) {
        AudioPortSlot* port = &s_ports[0];
        memset(port, 0, sizeof(*port));
        port->in_use = port->running = 1;
        port->param.nChannel = 8;
        port->param.nBlock = blocks;
        port->param.level = 1.0f;
        port->read_idx_addr = 16;
        port->buffer = calloc(blocks * CELL_AUDIO_BLOCK_SAMPLES * 8, sizeof(float));
        assert(port->buffer);
        for (unsigned i = 1; i <= blocks * 3; ++i) {
            audio_mix_one_block();
            assert(port->read_index == i);
            assert(vm_read64(16) == (i + audio_lead(blocks)) % blocks);
        }
        free(port->buffer);
    }
    memset(s_ports, 0, sizeof(s_ports));
    s_audio_initialized = 1;
    vm_write64(0x100, 8);
    vm_write64(0x108, 32);
    vm_write64(0x60000000, 0x123456789abcdef0ULL);
    for (unsigned i = 0; i < CELL_AUDIO_PORT_MAX; ++i) {
        assert(cellAudioPortOpen((void*)0x100, (void*)0x200) == CELL_OK);
        assert(vm_read32(0x200) == i);
        assert(vm_read64((u32)s_ports[i].read_idx_addr) == audio_lead(32));
        assert(s_ports[i].port_addr >= 0x58000000);
        assert(s_ports[i].read_idx_addr + 8 <= 0x59000000);
        if (i) assert(s_ports[i].port_addr > s_ports[i-1].read_idx_addr);
    }
    assert(vm_read64(0x60000000) == 0x123456789abcdef0ULL);
    u64 first = s_ports[0].port_addr;
    assert(cellAudioPortClose(0) == CELL_OK);
    assert(cellAudioPortOpen((void*)0x100, (void*)0x200) == CELL_OK);
    assert(s_ports[0].port_addr == first);
    assert(!s_ports[0].running);
    audio_mix_one_block();
    assert(vm_read64((u32)s_ports[0].read_idx_addr) == audio_lead(32));
    assert(cellAudioPortStart(0) == CELL_OK);
    assert(cellAudioPortStart(0) == CELL_AUDIO_ERROR_PORT_ALREADY_RUN);
    s_ports[0].read_index = 35;
    s_audio_start_us = 1000000;
    const u64 tag = 35 + audio_lead(32);
    assert(cellAudioGetPortBlockTag(0, tag % 32, (void*)0x300) == CELL_OK);
    assert(vm_read64(0x300) == tag);
    assert(cellAudioGetPortTimestamp(0, tag, (void*)0x308) == CELL_OK);
    assert(vm_read64(0x308) == 1000000 + tag * 256000000ULL / 48000);
    assert(cellAudioGetPortTimestamp(0, tag + 1, (void*)0x308) == CELL_AUDIO_ERROR_TAG_NOT_FOUND);
    assert(cellAudioGetPortBlockTag(0, 32, (void*)0x300) == CELL_AUDIO_ERROR_PARAM);
#ifdef _WIN32
    VirtualFree(vm_base, 0, MEM_RELEASE);
#else
    munmap(vm_base, 0x100000000ULL);
#endif
    puts("audio ring: 3 sizes, 3 wraps each passed");
    return 0;
}
