/*
 * Bleach native host integration.
 *
 * The simulation clock and the D3D12 render worker have independent lifetimes
 * and pacing. The clock delivers the original game's 60 Hz timing pulses;
 * real FIFO and backend completion events govern graphics resource reuse.
 * The runtime supplies ABI, memory, firmware and guest dispatch services.
 * Native geometry execution and memory tracking are scoped here by the port.
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <string>
#include "ps3emu/ps3_ftrace.h"
#ifdef BLEACH_NATIVE_GEOMETRY
#include <xmmintrin.h>
#include "native_job_audit.h"
#include "native_geometry_audit.h"
#include "native_geometry_contract_capture.h"
#include "native_geometry_phase.h"
#include "native_geometry_tasks.h"
#include "native_geometry_handoff_host.h"
#include "native_geometry_lanes.h"
extern "C" {
struct spu_context;
extern int bleach_native_geometry_fp;
extern int spu_run_with_halt(void(*)(spu_context*),spu_context*);
extern int (*g_spurs_native_job_run)(void(*)(spu_context*),spu_context*,int);
}
static int bleach_native_job_run(void(*entry)(spu_context*),spu_context* ctx,int image)
{
    bleach_geometry_tasks::validate_context(ctx,image);
    struct HandoffScope {
        ~HandoffScope(){if(bleach_geometry_handoff_mode)bleach_geometry_handoff::cancel();}
    } handoff_scope;
    bleach_job_audit::Scope audit(ctx,image);
    bleach_geometry_audit::Scope geometry_inputs(ctx,image);
    if (image!=1014 || !bleach_native_geometry_fp) return spu_run_with_halt(entry,ctx);
    const unsigned saved=_mm_getcsr();
    _mm_setcsr((saved & ~_MM_ROUND_MASK) | _MM_ROUND_TOWARD_ZERO |
        _MM_MASK_MASK | _MM_FLUSH_ZERO_ON | 0x40u);
    static thread_local bool reported;
    if (!reported) {
        fprintf(stderr,"[native-float] geometry worker MXCSR=%x original=%x\n",_mm_getcsr(),saved);
        reported=true;
    }
    const int result=spu_run_with_halt(entry,ctx);
    /* spu_run_with_halt catches guest halt/stop unwinds before this restore. */
    _mm_setcsr(saved);
    return result;
}
#endif

#ifndef _WIN32
#include <sys/mman.h>   /* mmap: the guest arena, see alloc_guest_vm */
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#include "native_memory_watch.h"
#include "native_frame_clock.h"
#endif

#include "native_engine_frame.h"
#include "native_frame_diagnostics.h"
#include "native_graphics.h"

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
uint64_t cellGcmClaimFlipCompletion(void);
void     cellGcmCompleteFlip(uint32_t epoch);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
void     cellGcm_fifo_kick_wait(unsigned ms);
unsigned cellGcm_flip_request_count(void);
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" int rsx_live_draw_enabled(void);
extern "C" int rsx_live_draw_init(void*,uint32_t,uint32_t,const uint8_t*(*)(void*,uint32_t,uint32_t,uint32_t),void*);
extern "C" void rsx_live_draw_present(uint32_t);
extern "C" void rsx_live_draw_dump_present_samples(void);
extern "C" void rsx_live_draw_set_source_watch(int(*)(const void*,size_t,uint64_t*));
extern "C" void rsx_live_draw_set_pixel_constants_filter(void(*)(uint64_t,float,uint32_t*,uint32_t));
extern "C" int rsx_null_backend_init(uint32_t,uint32_t,const char*);
extern "C" int rsx_null_backend_pump_messages(void);
extern "C" void* rsx_null_backend_get_hwnd(void);
extern "C" void rsx_null_backend_suppress_present(int);
extern "C" uint32_t cellGcmGetCurrentDisplayBufferId(void);
extern "C" uint32_t cellGcmResolveLocated(int,uint32_t);
extern "C" uint32_t cellGcmResolveIO(uint32_t);
static bool live_graphics=false;
extern "C" int rsx_live_draw_set_frames_in_flight(uint32_t);
extern "C" uint32_t rsx_live_draw_frames_in_flight(void);
extern "C" uint64_t rsx_live_draw_submitted_fence(void);
extern "C" uint64_t rsx_live_draw_completed_fence(void);
extern "C" void rsx_live_draw_flush(void);
static bool native_present_ack();
static const uint8_t* live_memory(void*,uint32_t location,uint32_t offset,uint32_t size) {
    uint32_t ea=location ? cellGcmResolveIO(offset) : cellGcmResolveLocated(1,offset);
    if(!ea && location) ea=cellGcmResolveLocated(0,offset);
    if(!ea || ea==0xffffffffu || (uint64_t)ea+size>0x100000000ull)return nullptr;
    return vm_base+ea;
}
static int rsx_backend_init(uint32_t w,uint32_t h,const char* title) {
    if(rsx_live_draw_enabled()) {
        const char* async_frames=getenv("BLEACH_ASYNC_FRAMES");
        if(async_frames && !strcmp(async_frames,"1") && native_present_ack())
            rsx_live_draw_set_frames_in_flight(2);
        bleach_graphics_init();
        rsx_live_draw_set_pixel_constants_filter(bleach_graphics_constants);
        if(rsx_null_backend_init(w,h,title)!=0)return -1;
        if(rsx_live_draw_init(rsx_null_backend_get_hwnd(),w,h,live_memory,nullptr)!=0)return -1;
        rsx_null_backend_suppress_present(1);live_graphics=true;return 0;
    }
    return rsx_d3d12_backend_init(w,h,title);
}
static void rsx_backend_present() {
    if(live_graphics)rsx_live_draw_present(cellGcmGetCurrentDisplayBufferId());
    else rsx_d3d12_backend_present();
}
static int rsx_backend_pump(){return live_graphics?rsx_null_backend_pump_messages():rsx_d3d12_backend_pump_messages();}
#  define RSX_BACKEND_NAME    "D3D12 (selected engine)"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;
extern "C" void ppu_gcm_pump(void);
static bool native_present_ack()
{
    static const bool enabled=[] {
        const char* value=getenv("BLEACH_PRESENT_ACK");
        return !value || strcmp(value,"0");
    }();
    return enabled;
}

struct NativePresentationCompletion { uint64_t fence; uint32_t epoch; };
static std::deque<NativePresentationCompletion> native_present_pending;
static uint64_t native_present_enqueued=0,native_present_retired=0;
static size_t native_present_highwater=0;
static bool native_async_present()
{
#ifdef _WIN32
    return live_graphics && rsx_live_draw_frames_in_flight()>1;
#else
    return false;
#endif
}
static void native_poll_present_completions()
{
#ifdef _WIN32
    if(native_present_pending.empty())return;
    const uint64_t completed=rsx_live_draw_completed_fence();
    bool delivered=false;
    while(!native_present_pending.empty() && native_present_pending.front().fence<=completed) {
        cellGcmCompleteFlip(native_present_pending.front().epoch);
        native_present_pending.pop_front();++native_present_retired;delivered=true;
    }
    if(delivered)ppu_gcm_pump();
#endif
}
static void native_queue_present_completion(uint64_t ticket)
{
#ifdef _WIN32
    if(!ticket)return;
    const uint64_t fence=rsx_live_draw_submitted_fence();
    if(!fence || (!native_present_pending.empty() && fence<=native_present_pending.back().fence)) {
        fprintf(stderr,"[bleach] STOP: presentation has no unique successful GPU submission\n");
        exit(83);
    }
    native_present_pending.push_back({fence,(uint32_t)ticket});++native_present_enqueued;
    if(native_present_pending.size()>native_present_highwater)native_present_highwater=native_present_pending.size();
    native_poll_present_completions();
    // Two resource slots and an ordered queue may never accumulate unbounded latency.
    if(native_present_pending.size()>2) {
        fprintf(stderr,"[bleach] STOP: presentation completion backlog exceeds resource slots\n");
        exit(84);
    }
#else
    (void)ticket;
#endif
}

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static void present_guest_frame(void)
{
    const bool deferred=native_async_present();
    const uint64_t ticket=deferred?cellGcmClaimFlipCompletion():0;
    const uint64_t begin = PS3_FT_ON() ? ps3_ft_now() : 0;
    rsx_backend_present();
    if (begin) ps3_ft_span(PS3_FT_PRESENT, 0, 0, 0, begin, ps3_ft_now());
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
    static const bool timing_diag=getenv("BLEACH_FRAME_TIMING")!=nullptr;
    if (timing_diag && !(g_frames_presented & 127)) {
        const uint32_t graphics=vm_read32(0x004AE670u-0x3C38u);
        const uint32_t loop=vm_read32(0x004AE670u-0x7E84u);
        const uint32_t engine=vm_read32(0x004AE670u-0x7F98u);
        if (graphics && loop && engine) {
            uint32_t bits=vm_read32(engine+0x38u); float dt; memcpy(&dt,&bits,4);
            fprintf(stderr,"[frame-timing] frame=%ld vblank=%u frame_count=%u targets=%u,%u slow_flag=%u last_cpu_dt=%g\n",
                g_frames_presented,vm_read32(graphics+0x10u),vm_read32(graphics+0x18u),
                vm_read32(graphics+0x13CD0Cu),vm_read32(graphics+0x27560Cu),
                (unsigned)vm_base[loop+0x11u],dt);
        }
    }
    /* The native backend has completed this frame and its existing fence.
     * Deliver its completion now instead of waiting for a synthetic vblank.
     * TickFlip still requires the FIFO to have reached the real flip marker. */
    if(deferred) {
        native_queue_present_completion(ticket);
    } else if (native_present_ack()) {
        cellGcmTickFlip();
        ppu_gcm_pump();
    }
}

static void simulation_clock_tick(void*)
{
    cellGcmTickVBlank();
    // The compatibility switch retains the original completion policy for A/B.
    if (!native_present_ack()) cellGcmTickFlip();
    ppu_gcm_pump();
}

extern "C" void ps3_sample_register_thread(int tag);  /* BLEACH_LANE_SAMPLE */
/* The frame is a chain: main PPU scene build -> geometry lanes -> this render thread's
 * drain of the RSX FIFO; the next scene waits for the drain. BLEACH_CRITICAL_PRIORITY=1
 * raises the two chain threads above the lanes so a hybrid CPU keeps them on fast cores. */
static void bleach_critical_thread()
{
    static const bool on=[]{const char* e=getenv("BLEACH_CRITICAL_PRIORITY");return e&&*e=='1';}();
    if(on)SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
}
static DWORD WINAPI render_worker(LPVOID)
{
    ps3_sample_register_thread(2);
    bleach_critical_thread();
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    bool independent_clock=false;
#ifdef _WIN32
    BleachNativeFrameClock simulation_clock;
    const char* clock_env=getenv("BLEACH_INDEPENDENT_CLOCK");
    if (!clock_env || strcmp(clock_env,"0")) {
        if (!simulation_clock.start(simulation_clock_tick,nullptr)) {
            fprintf(stderr,"[native-clock] cannot start clock worker: %lu\n",GetLastError());
            exit(1);
        }
        independent_clock=true;
    }
#endif

    ULONGLONG next_tick = GetTickCount64();
    /* The FIFO completes labels between render passes. Its polling interval
     * must be independent of the emulated 60 Hz vblank. Keep a sleeping wait;
     * the old four milliseconds accumulated across dependent passes. */
    const char* poll_env = getenv("BLEACH_RSX_POLL_MS");
    DWORD poll_ms = poll_env ? (DWORD)strtoul(poll_env, nullptr, 10) : 1;
    if (poll_ms < 1) poll_ms = 1;
    if (poll_ms > 16) poll_ms = 16;

    for (;;) {
        native_poll_present_completions();
        cellGcm_fifo_kick_wait(poll_ms);
        ULONGLONG now = GetTickCount64();

        int fired = 0;
        while (!independent_clock && (long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            ppu_gcm_pump();  /* deliver interrupts even while guest waits on a semaphore */
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                present_guest_frame();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += 16;             /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
            }
            cellGcm_rsx_process_fifo();
            native_poll_present_completions();
            // Retired user commands release individual engine render stages.
            // Their delivery must not wait for the next simulation/HLE call
            // or display pulse. The runtime serializes callback execution.
            ppu_gcm_pump();

            static const ULONGLONG benchmark_origin=GetTickCount64();
            static const unsigned benchmark_seconds=[] {
                const char* s=getenv("BLEACH_BENCHMARK_SECONDS");return s?(unsigned)strtoul(s,nullptr,10):0u;
            }();
            if (rsx_backend_pump() != 0 || (benchmark_seconds &&
                GetTickCount64()-benchmark_origin>=(ULONGLONG)benchmark_seconds*1000)) {
#ifdef _WIN32
                if(live_graphics && !native_present_pending.empty()) {
                    rsx_live_draw_flush();native_poll_present_completions();
                }
                fprintf(stderr,"[native-submission-summary] queued=%llu retired=%llu pending=%zu highwater=%zu\n",
                    (unsigned long long)native_present_enqueued,(unsigned long long)native_present_retired,
                    native_present_pending.size(),native_present_highwater);
                simulation_clock.stop();
                rsx_live_draw_dump_present_samples();
#endif
                exit(0);                 /* user closed the native window */
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            /* A drain can reach a flip marker. Consume that marker now; a
             * count-only present leaves pending set and presents the same
             * completed batch again on the following iteration. */
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
            } else if (fc == 0) {
                rsx_backend_present();
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    const char* watch_env = getenv("BLEACH_MEMORY_WATCH");
    const bool use_watch = !watch_env || strcmp(watch_env,"0");
    if (use_watch)
        vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE,
            MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (vm_base && bleach_watch_init(vm_base,VM_SIZE))
        rsx_live_draw_set_source_watch(bleach_memory_changed);
    if (!vm_base)
        vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    fprintf(stderr,"[memory-watch] enabled=%d page_size=%zu\n",
            bleach_watch.enabled,bleach_watch.page_size);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is mapped up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left.
     *
     * mmap, not calloc: vm_commit() hands vm_base + addr to mprotect, which
     * takes page-aligned addresses only. glibc serves an allocation this big
     * from its own mmap chunk and returns a pointer 16 bytes past the page
     * start, so every commit failed with EINVAL and sys_memory_allocate
     * answered ENOMEM to every call. mmap's base is page-aligned and zeroed. */
    void* p = mmap(NULL, 0xE0000000u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (p == MAP_FAILED) ? nullptr : (uint8_t*)p;
    ppu_vm_size = 0xE0000000u;
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" const char* g_last_hle_name;
extern "C" void bleach_register_hle(void);
extern "C" int ps3_hle_has(uint32_t);
extern "C" uint32_t prx_resolve_export(uint32_t);

#ifdef _WIN32
static LONG WINAPI bleach_crash_filter(EXCEPTION_POINTERS* e)
{
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    fprintf(stderr, "[bleach-crash] exception=0x%08lX host-rva=0x%llX last-hle=%s\n",
            e->ExceptionRecord->ExceptionCode,
            (unsigned long long)(e->ContextRecord->Rip - base),
            g_last_hle_name ? g_last_hle_name : "none");
    if (e->ExceptionRecord->NumberParameters >= 2)
        fprintf(stderr, "[bleach-crash] operation=%llu address=0x%llX\n",
                (unsigned long long)e->ExceptionRecord->ExceptionInformation[0],
                (unsigned long long)e->ExceptionRecord->ExceptionInformation[1]);
    if (g_active_ctx) {
        fprintf(stderr, "[bleach-crash] guest cia=0x%08X lr=0x%08X ctr=0x%08X\n",
                (unsigned)g_active_ctx->cia, (unsigned)g_active_ctx->lr,
                (unsigned)g_active_ctx->ctr);
        for (int i = 0; i < 32; i += 4)
            fprintf(stderr, "  r%02d..r%02d: %016llX %016llX %016llX %016llX\n", i, i+3,
                    (unsigned long long)g_active_ctx->gpr[i],
                    (unsigned long long)g_active_ctx->gpr[i+1],
                    (unsigned long long)g_active_ctx->gpr[i+2],
                    (unsigned long long)g_active_ctx->gpr[i+3]);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void ps3_sampler_start(void);

int main(int argc, char** argv)
{
    if (!getenv("SPURS_JOB_WORKERS")) {
#ifdef _WIN32
        _putenv_s("SPURS_JOB_WORKERS", "auto");
#else
        setenv("SPURS_JOB_WORKERS", "auto", 0);
#endif
    }
    if (argc < 2) {
#ifdef _WIN32
        /* Double-clicked inside a package (bin/BleachRecomp.exe): the game path,
         * config and environment come from the package launcher, so hand over to it. */
        char exe[MAX_PATH];
        const DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
        if (n && n < MAX_PATH) {
            std::string dir(exe, n);
            dir.resize(dir.find_last_of("\\/"));
            const std::string package = dir.substr(0, dir.find_last_of("\\/") + 1);
            // The graphical launcher when present, else the console script.
            const std::string gui = package + "Bleach Launcher.exe", script = package + "Iniciar-Recomp.cmd";
            const bool has_gui = GetFileAttributesA(gui.c_str()) != INVALID_FILE_ATTRIBUTES;
            std::string cmd = has_gui ? "\"" + gui + "\"" : "cmd.exe /c \"\"" + script + "\"\"";
            STARTUPINFOA si = {}; si.cb = sizeof si;
            PROCESS_INFORMATION pi = {};
            if ((has_gui || GetFileAttributesA(script.c_str()) != INVALID_FILE_ATTRIBUTES) &&
                CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE, has_gui ? 0 : CREATE_NEW_CONSOLE, NULL,
                               package.c_str(), &si, &pi)) {
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                return 0;
            }
        }
#endif
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
    /* Real pixels, not DPI-virtualized ones: at 250% scaling an unaware
     * process sees a 3840x2160 monitor as 1536x864, so a covering window is
     * stretched and exclusive fullscreen switches to that low mode. */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(bleach_crash_filter);
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    /* No EcoQoS. With another window focused (or the game on a second monitor) Windows would
     * otherwise put the whole process on the efficiency cores of a hybrid CPU at lower clocks,
     * and Windows 11 drops the 1 ms timer above for a covered window. */
    {
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
        PROCESS_POWER_THROTTLING_STATE throttling = {};
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        throttling.StateMask = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
    }
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    /* BLEACH_TIMESTEP_PATCH=1 (experiment, with BLEACH_SIMULATION_HZ): the
     * fixed 1/30 s step constants the update code loads from the TOC are
     * rewritten to 1/<simulation Hz>. Each word is checked first; nothing is
     * written unless it still holds exactly 1/30 (0x3D088889). */
    {
        const char* patch = getenv("BLEACH_TIMESTEP_PATCH");
        const char* hz_env = getenv("BLEACH_SIMULATION_HZ");
        const unsigned hz = hz_env ? (unsigned)atoi(hz_env) : 30u;
        if (patch && !strcmp(patch, "1") && hz > 30 && hz <= 240) {
            static const uint32_t steps[] = { 0x4A82B8, 0x4A85D0, 0x4A9F70, 0x4AA004, 0x4AA278,
                                              0x4AC608, 0x4AC79C, 0x4ACAA8, 0x4ADAF8, 0x4ADB4C };
            const float step = 1.0f / (float)hz; uint32_t bits; memcpy(&bits, &step, 4);
            unsigned patched = 0;
            for (uint32_t ea : steps)
                if (vm_read32(ea) == 0x3D088889u) { vm_write32(ea, bits); ++patched; }
            fprintf(stderr, "[timestep] %u/%u fixed 1/30 s steps -> 1/%u s\n", patched,
                    (unsigned)(sizeof steps / sizeof steps[0]), hz);
        }
        /* BLEACH_FRAMERATE_PATCH=<hex EA>[,<hex EA>...]: 30.0f frame-count ->
         * seconds divisors rewritten to the simulation rate (checked first). */
        const char* rates = getenv("BLEACH_FRAMERATE_PATCH");
        if (rates && hz > 30 && hz <= 240) {
            const float rate = (float)hz; uint32_t bits; memcpy(&bits, &rate, 4);
            for (const char* p = rates; *p;) {
                char* end = nullptr; const uint32_t ea = (uint32_t)strtoul(p, &end, 16);
                if (end == p) break;
                const bool ok = vm_read32(ea) == 0x41F00000u;
                if (ok) vm_write32(ea, bits);
                fprintf(stderr, "[timestep] 30.0f at %08X -> %u.0f %s\n", ea, hz, ok ? "patched" : "SKIPPED (not 30.0f)");
                p = *end == ',' ? end + 1 : end;
            }
        }
        /* BLEACH_FLOAT_PATCH=<hex EA>:<expected>:<new>[,...]: experiment hook for one
         * guest float constant at a time; the word is checked against <expected> first. */
        const char* floats = getenv("BLEACH_FLOAT_PATCH");
        for (const char* p = floats; p && *p;) {
            unsigned ea = 0; float expected = 0, value = 0; int used = 0;
            if (sscanf(p, "%x:%f:%f%n", &ea, &expected, &value, &used) != 3) break;
            uint32_t want, bits; memcpy(&want, &expected, 4); memcpy(&bits, &value, 4);
            const bool ok = vm_read32(ea) == want;
            if (ok) vm_write32(ea, bits);
            fprintf(stderr, "[float-patch] %08X %g -> %g %s\n", ea, expected, value, ok ? "patched" : "SKIPPED (mismatch)");
            p += used; if (*p == ',') ++p;
        }
    }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    ppu_recomp_register();   /* lifted function table -> address map */
#ifdef BLEACH_NATIVE_GEOMETRY
    bleach_job_audit::initialize();
    bleach_geometry_audit::initialize();
    bleach_geometry_contract_capture::initialize();
    bleach_phase_initialize();
    bleach_geometry_handoff_initialize();
    bleach_geometry_tasks::initialize();
    bleach_geometry_lanes::initialize();
    g_spurs_native_job_run=bleach_native_job_run;
#endif
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */
    bleach_register_hle();
    /* BLEACH_IMPORT_AUDIT=<file>: one "library nid" per line (the EBOOT imports); prints
     * each import no handler is registered for -- the calls that would stop the title
     * as "unresolved NID" -- then exits. */
    if (const char* audit = getenv("BLEACH_IMPORT_AUDIT")) {
        FILE* f = fopen(audit, "r"); unsigned total = 0, missing = 0; char lib[64]; unsigned nid;
        while (f && fscanf(f, "%63s %x", lib, &nid) == 2) {
            ++total;
            if (!ps3_hle_has(nid) && !prx_resolve_export(nid)) { ++missing; fprintf(stderr, "[import-audit] missing %s 0x%08X\n", lib, nid); }
        }
        if (f) fclose(f);
        fprintf(stderr, "[import-audit] imports=%u missing=%u\n", total, missing);
        return 0;
    }

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    CreateThread(NULL, 4u * 1024 * 1024, render_worker, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    ps3_sampler_start();
    ps3_sample_register_thread(1);
    bleach_critical_thread();
    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
