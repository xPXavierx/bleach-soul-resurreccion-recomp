#ifndef PS3EMU_WRITE_WATCH_H
#define PS3EMU_WRITE_WATCH_H
/* Optional observer of a single four-byte publication word. This observes
 * completed stores; it does not make guest data atomic or authorize consumption.
 * A static callback is published before addr, and remains alive until exit. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*ps3_write_watch_callback)(uint32_t, uint32_t, uint32_t);
typedef struct { uint32_t addr, ordered_dma; ps3_write_watch_callback commit; } ps3_write_watch;
#if defined(_MSC_VER)
__declspec(selectany) ps3_write_watch g_ps3_write_watch = {0};
#else
__attribute__((weak)) ps3_write_watch g_ps3_write_watch = {0};
#endif
enum { PS3_WRITE_PPU=1, PS3_WRITE_HLE=2, PS3_WRITE_DMA=3, PS3_WRITE_ATOMIC=4 };
static inline void ps3_write_watch_commit(uint32_t ea, uint32_t size, uint32_t source)
{
    /* DMA publications pair with the consumer's seq-cst watch exchange and
     * recheck. The full barrier also applies when addr is still zero: otherwise
     * both sides could miss each other's buffered store. Plain PPU/HLE stores
     * are best-effort notifications; existing PUT kicks and timeout remain. */
    if(source>=PS3_WRITE_DMA && __atomic_load_n(&g_ps3_write_watch.ordered_dma,__ATOMIC_ACQUIRE))
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t watched = __atomic_load_n(&g_ps3_write_watch.addr, __ATOMIC_ACQUIRE);
    if (watched && size && (uint64_t)ea < (uint64_t)watched+4 &&
        (uint64_t)ea+size > watched)
        g_ps3_write_watch.commit(ea, size, source);
}
#ifdef __cplusplus
}
#endif
#endif
