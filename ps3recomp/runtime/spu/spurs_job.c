/* spurs_job.c — stage and enter a SPURS jobchain job the way Sony's jm2 does.
 *
 * A job binary is not an SPU ELF and not a policy module: it is a raw image
 * built by the SDK's job_elf-to-bin, linked against Sony's job_start_w_crt.o.
 * That CRT is what every job entry actually runs first — it saves r3/r4, runs
 * the ctors, and calls
 *
 *     void cellSpursJobMain2(CellSpursJobContext2 *jobContext,   // r3
 *                            CellSpursJob256      *job);         // r4
 *
 * (target/spu/include/cell/spurs/job_chain.h). Both are LOCAL STORE pointers,
 * so the manager must have laid the whole working set out in LS before entry.
 * Entering a job without that context is what left LBP's job parked with zero
 * DMA traffic: it reads its ioBuffer pointer out of a zeroed context and stalls.
 *
 * The layout is pinned by _cellSpursCheckJob() in
 * target/common/include/cell/spurs/job_descriptor.h, which sums the exact LS a
 * job consumes. For a jobchain it runs with align = 1024, isScratchStackUnified
 * = 1, maxUsableMemorySize = CELL_SPURS_MAX_SIZE_JOB_MEMORY (234 KB):
 *
 *     align1024(sizeBinary<<4)                                   binary + bss
 *   + align1024(sizeInOrInOut)                                   ioBuffer
 *   + align1024(sizeOut)                                         oBuffer
 *   + align1024((sizeScratch<<4) + (sizeStack ? sizeStack<<4 : 8192))
 *                                                                scratch + stack
 *   + per cache entry: align1024(size)                           cacheBuffer[0..3]
 *
 * We reproduce that arithmetic rather than invent a layout, so a job that
 * range-checks its own buffers sees what it would on hardware.
 *
 * Load address: the job binary is position-independent (job_start_w_crt derives
 * its load delta with a brsl-to-next / subtract-link-time-address idiom), so jm2
 * may place it anywhere. We place it at LS 0 — the lifted C is keyed to
 * link-time addresses, which makes the delta zero and the lifted branch targets
 * exact.
 */

#include "spu_coherency.h"
#include "ps3emu/ps3_ftrace.h"
#include "spu_workload.h"
#include "spu_context.h"
#include "../platform/win32_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/log_first.h"

extern uint8_t* vm_base;
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
/* Optional port-owned execution scope (installed before jobs start). */
int (*g_spurs_native_job_run)(void (*)(spu_context*),spu_context*,int);

/* ---- guest reads (big-endian) ------------------------------------------ */
static uint32_t g32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint16_t g16(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}
static uint64_t g64(uint32_t ea)
{
    return ((uint64_t)g32(ea) << 32) | (uint64_t)g32(ea + 4);
}

/* ---- local-store writes (big-endian) ----------------------------------- */
static void ls32(uint8_t* ls, uint32_t o, uint32_t v)
{
    ls[o] = (uint8_t)(v >> 24); ls[o+1] = (uint8_t)(v >> 16);
    ls[o+2] = (uint8_t)(v >> 8); ls[o+3] = (uint8_t)v;
}
static void ls16(uint8_t* ls, uint32_t o, uint16_t v)
{
    ls[o] = (uint8_t)(v >> 8); ls[o+1] = (uint8_t)v;
}

/* CellSpursJobHeader, 48 B — job_descriptor.h */
enum {
    JH_EA_BINARY           = 0x00,   /* be u64; low bit = CACHE_INHIBIT flag */
    JH_SIZE_BINARY         = 0x08,   /* be u16; bytes >> 4                   */
    JH_SIZE_DMA_LIST       = 0x0A,   /* be u16; input list size in BYTES     */
    JH_EA_HIGH_INPUT       = 0x0C,
    JH_USE_INOUT_BUFFER    = 0x10,
    JH_SIZE_IN_OR_INOUT    = 0x14,
    JH_SIZE_OUT            = 0x18,
    JH_SIZE_STACK          = 0x1C,   /* be u16; quadwords */
    JH_SIZE_SCRATCH        = 0x1E,   /* be u16; quadwords */
    JH_EA_HIGH_CACHE       = 0x20,
    JH_SIZE_CACHE_DMA_LIST = 0x24,   /* be u32; bytes */
    JH_JOB_TYPE            = 0x2C,
    JH_SIZE                = 0x30,
};

/* CellSpursJobContext2, 48 B — job_context_types.h. SPU pointers are 32-bit LS
 * offsets and the struct lives in local store, so every field is big-endian. */
enum {
    JC_IO_BUFFER          = 0x00,
    JC_CACHE_BUFFER       = 0x04,   /* void *cacheBuffer[4] */
    JC_SIZE_JOB_DESC      = 0x14,   /* sizeJobDescriptor:4 + pad:28 */
    JC_NUM_IO_BUFFER      = 0x18,
    JC_NUM_CACHE_BUFFER   = 0x1A,
    JC_O_BUFFER           = 0x1C,
    JC_S_BUFFER           = 0x20,
    JC_DMA_TAG            = 0x24,
    JC_EA_JOB_DESCRIPTOR  = 0x28,   /* be u64 */
    JC_SIZE               = 0x30,
};

#define ALIGN1024(x)  (((x) + 1023u) & ~1023u)
#define JOB_DMA_TAG   20u          /* "one of: {20,21}" — job_context_types.h */
#define JOB_STACK_DEFAULT 8192u    /* _cellSpursCheckJob's sizeStack==0 default */

/* Local store of the most recent job, kept alive past the run.
 *
 * A title can poll the SPU that runs a job chain with sys_spu_thread_read_LS,
 * identifying it by the CHAIN HANDLE rather than an lv2 thread id -- Tokyo
 * Jungle reads its bus counts that way, 45k polls of offset 0 in a 45 s run.
 * A job's executing context must remain private, but its completed store must
 * survive the return to the scheduler. Retain ownership of that store and let
 * the syscall resolve a chain handle to it. */
#if defined(_MSC_VER)
#  define SPURS_JOB_TLS __declspec(thread)
#else
#  define SPURS_JOB_TLS __thread
#endif
/* A completed job owns its private context until publication. The snapshot
 * takes that ownership and returns its previous context to this reusable pool.
 * Readers hold the snapshot lock, so an evicted store cannot be reset while a
 * read is in progress. Storage grows only to the high-water number of executing
 * jobs plus retained chain snapshots; it does not grow with jobs or threads. */
typedef struct JobStorage {
    spu_context context;
    struct JobStorage* next;
    /* LS bytes from here up may be non-zero only in the descriptor area at the
     * top (rewritten by every job). A job writes inside its own regions, so the
     * next one clears [0, ls_dirty) instead of all 256 KiB. */
    uint32_t ls_dirty;
} JobStorage;
static SRWLOCK s_storage_lock = SRWLOCK_INIT;
static JobStorage* s_free_storage;
/* One context kept by each job thread. A job zeroes and fills 256 KiB of its
 * context; taking one last written on another core turns that into thousands
 * of cross-core line transfers per job, and the cost grew with every lane. */
static SPURS_JOB_TLS JobStorage* t_own_storage;
/* Set by the first sys_spu_thread_read_LS on a chain handle. Until a title
 * reads a completed job's store, nothing needs it, so it is not published. */
static volatile long s_snapshot_readers;

static JobStorage* job_storage_take(void)
{
    if (t_own_storage) {
        JobStorage* own = t_own_storage;
        t_own_storage = NULL;
        return own;
    }
    AcquireSRWLockExclusive(&s_storage_lock);
    JobStorage* storage = s_free_storage;
    if (storage) s_free_storage = storage->next;
    ReleaseSRWLockExclusive(&s_storage_lock);
    if (!storage) {
        storage = (JobStorage*)malloc(sizeof(*storage));
        if (storage) storage->ls_dirty = SPU_LS_SIZE;   /* uninitialised */
    }
    if (!storage) {
        fprintf(stderr, "[spurs-job] cannot allocate private job context\n");
        abort();
    }
    return storage;
}

static void job_storage_return(JobStorage* storage)
{
    if (!storage) return;
    if (!t_own_storage) { t_own_storage = storage; return; }
    AcquireSRWLockExclusive(&s_storage_lock);
    storage->next = s_free_storage;
    s_free_storage = storage;
    ReleaseSRWLockExclusive(&s_storage_lock);
}

/* Stable per-chain snapshots retain a complete private store. Publication is
 * a pointer exchange, not a 256 KiB copy that serializes geometry workers. */
static struct JobSnapshot {
    uint32_t handle;
    SRWLOCK lock;
    JobStorage* storage;
} s_job_snapshots[32];
static SRWLOCK s_snapshot_directory = SRWLOCK_INIT;
static SPURS_JOB_TLS struct JobSnapshot* s_current_snapshot;

void spurs_job_set_chain_handle(uint32_t handle)
{
    if (s_current_snapshot && s_current_snapshot->handle == handle) return;
    s_current_snapshot = NULL;
    if (!handle) return;
    AcquireSRWLockExclusive(&s_snapshot_directory);
    for (unsigned i = 0; i < 32; ++i) {
        struct JobSnapshot* p = &s_job_snapshots[i];
        if (!p->handle || p->handle == handle) {
            if (!p->handle) p->handle = handle; /* immutable after publication */
            s_current_snapshot = p;
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_snapshot_directory);
}

int spurs_job_read_ls(uint32_t handle, uint32_t offset, uint32_t size, uint64_t* value)
{
    if (!handle || !value || (size != 1 && size != 2 && size != 4 && size != 8) ||
        offset > SPU_LS_SIZE - size) return 0;
    if (!s_snapshot_readers) InterlockedExchange(&s_snapshot_readers, 1);
    struct JobSnapshot* p = NULL;
    AcquireSRWLockShared(&s_snapshot_directory);
    for (unsigned i = 0; i < 32; ++i)
        if (s_job_snapshots[i].handle == handle) { p = &s_job_snapshots[i]; break; }
    ReleaseSRWLockShared(&s_snapshot_directory);
    if (!p) return 0;
    AcquireSRWLockShared(&p->lock);
    const JobStorage* storage = p->storage;
    int valid = storage != NULL;
    uint64_t v = 0;
    if (valid) for (unsigned i = 0; i < size; ++i) v = (v << 8) | storage->context.ls_store[offset + i];
    ReleaseSRWLockShared(&p->lock);
    *value = v;
    return valid;
}

static void job_storage_publish(struct JobSnapshot* snapshot, JobStorage* completed)
{
    /* No reader yet: keep the context on this thread. The first read finds no
     * snapshot for its handle and reports it invalid, exactly as before any job
     * of that chain had completed; every later job publishes again. */
    if (!snapshot || !s_snapshot_readers) { job_storage_return(completed); return; }
    /* Diagnostic A/B retains the old copy semantics with the same staging
     * path. The normal path transfers ownership without copying guest data. */
    static SPURS_JOB_TLS int copy = -1;
    if (copy < 0) {
        const char* value = getenv("SPURS_JOB_SNAPSHOT_COPY");
        copy = value && !strcmp(value, "1");
    }
    AcquireSRWLockExclusive(&snapshot->lock);
    JobStorage* retired = snapshot->storage;
    if (copy) {
        if (!retired) retired = job_storage_take();
        memcpy(retired->context.ls_store, completed->context.ls_store, SPU_LS_SIZE);
        retired->ls_dirty = SPU_LS_SIZE;
        snapshot->storage = retired;
        retired = completed;
    } else {
        snapshot->storage = completed;
    }
    ReleaseSRWLockExclusive(&snapshot->lock);
    job_storage_return(retired);
}

/* Last outbound mailbox posted by a finished job (see below). */
/* THREAD-LOCAL. A job's answer is read back by the completion event that
 * follows it, and jc_execute signals immediately after each job -- but the walk
 * runs on whichever guest thread drives the chain, and more than one can be in
 * flight. As plain globals these were clobbered between a job finishing and its
 * own completion being pushed, so a query could be answered with an unrelated
 * job's mailbox. Per-thread keeps each job paired with its own answer. */
/* __declspec(thread) for MSVC only: MinGW GCC ignores it, which would make
 * these process-wide. spurs_job.c and cellSpurs.c must agree. */
SPURS_JOB_TLS uint32_t g_spurs_job_mbox, g_spurs_job_mbox_intr;
/* The command this job ran, read from its own command block. Only a query
 * expects an answer back through the completion event. */
SPURS_JOB_TLS uint32_t g_spurs_job_cmd;
SPURS_JOB_TLS int g_spurs_job_mbox_valid;
/* The SPU number a job sees (cellSpursGetCurrentSpuId, kernel LS 0x1C8). A host
 * thread that runs jobs concurrently with others must claim its own: titles
 * key per-SPU resources on it (Edge output rings, one RSX label per SPU), and
 * two jobs that both think they are SPU 0 corrupt each other's ring. */
SPURS_JOB_TLS uint32_t g_spurs_job_spu_num;
/* Descriptor EA of the job this SPU thread is running (debug: SPU_DMA_RANGE names it). */
SPURS_JOB_TLS uint32_t g_spurs_current_job_ea;
void spurs_job_set_spu_num(uint32_t n) { g_spurs_job_spu_num = n; }

/* BLEACH_SPU_HOLD=<hex binary ea>[,<hex binary ea>...] -- jobs of these binaries run once per
 * BUILT FRAME and advance a state they read and write back in main memory (Bleach: the sgx image
 * at 0x422100, whose job integrates the scenery fog particles in place, ~84 KB). The game built
 * 30 frames a second; at r frames per original update that state moved r times too fast.
 * Every run still executes, and what it only writes (the geometry it emits for drawing) is
 * kept; but of each descriptor's runs only one in r keeps the state: on the others every
 * stretch the job read (GET) and then wrote back (PUT) gets its bytes from before the run.
 * r is the host's (bleach_tick_ratio: 1 outside gameplay and at 30 FPS, nothing held then). */
extern int bleach_tick_ratio(void) __attribute__((weak));
SPURS_JOB_TLS int g_spu_hold_capture;
struct SpuHoldGet { uint32_t ea, size, at; };
static SPURS_JOB_TLS struct SpuHoldGet* t_hold_gets;
static SPURS_JOB_TLS uint32_t t_hold_ngets, t_hold_capgets, t_hold_bytes, t_hold_capbytes;
static SPURS_JOB_TLS uint8_t* t_hold_data;
static SPURS_JOB_TLS uint32_t* t_hold_puts;   /* ea, size pairs */
static SPURS_JOB_TLS uint32_t t_hold_nputs, t_hold_capputs;
void spu_hold_note_get(uint32_t ea, const uint8_t* src, uint32_t size)
{
    if (t_hold_ngets == t_hold_capgets) {
        t_hold_capgets = t_hold_capgets ? t_hold_capgets * 2 : 64;
        t_hold_gets = (struct SpuHoldGet*)realloc(t_hold_gets, t_hold_capgets * sizeof *t_hold_gets);
    }
    if (t_hold_bytes + size > t_hold_capbytes) {
        while (t_hold_bytes + size > t_hold_capbytes) t_hold_capbytes = t_hold_capbytes ? t_hold_capbytes * 2 : 0x40000;
        t_hold_data = (uint8_t*)realloc(t_hold_data, t_hold_capbytes);
    }
    memcpy(t_hold_data + t_hold_bytes, src, size);
    t_hold_gets[t_hold_ngets++] = (struct SpuHoldGet){ ea, size, t_hold_bytes };
    t_hold_bytes += size;
}
void spu_hold_note_put(uint32_t ea, uint32_t size)
{
    if (t_hold_nputs + 2 > t_hold_capputs) {
        t_hold_capputs = t_hold_capputs ? t_hold_capputs * 2 : 128;
        t_hold_puts = (uint32_t*)realloc(t_hold_puts, t_hold_capputs * sizeof *t_hold_puts);
    }
    t_hold_puts[t_hold_nputs++] = ea; t_hold_puts[t_hold_nputs++] = size;
}
static int spu_hold_binary(uint32_t ea_bin)
{
    static uint32_t bins[8]; static int n = -1;
    if (n < 0) {
        n = 0;
        for (const char* p = getenv("BLEACH_SPU_HOLD"); p && *p && n < 8;) {
            char* end; uint32_t v = (uint32_t)strtoul(p, &end, 16);
            if (end == p) break;
            bins[n++] = v;
            while (*end && *end != ',') end++;     /* tolerates an old ":object" suffix */
            p = *end == ',' ? end + 1 : end;
        }
    }
    for (int i = 0; i < n; i++) if (bins[i] == ea_bin) return 1;
    return 0;
}
/* Whether this run of the descriptor keeps its state: one run in r. */
static int spu_hold_commits(uint32_t job_ea, int ratio)
{
    static volatile LONG counts[64]; static volatile LONG keys[64];
    for (int i = 0; i < 64; i++) {
        LONG k = keys[i];
        if (k == (LONG)job_ea) return InterlockedIncrement(&counts[i]) % ratio == 0;
        if (!k && InterlockedCompareExchange(&keys[i], (LONG)job_ea, 0) == 0) return InterlockedIncrement(&counts[i]) % ratio == 0;
    }
    return 1;
}

int spu_run_spurs_job(spu_lifted_entry_fn entry, int image_id,
                      uint32_t job_ea, uint32_t job_desc_size)
{
    if (!entry || !job_ea) return -1;

    /* ---- descriptor ---------------------------------------------------- */
    uint32_t ea_bin   = (uint32_t)(g64(job_ea + JH_EA_BINARY) & ~1ull);
    uint32_t size_bin = (uint32_t)g16(job_ea + JH_SIZE_BINARY) << 4;
    uint32_t n_dma    = (uint32_t)g16(job_ea + JH_SIZE_DMA_LIST) / 8u;
    uint32_t use_io   = g32(job_ea + JH_USE_INOUT_BUFFER);
    uint32_t size_io  = g32(job_ea + JH_SIZE_IN_OR_INOUT);
    uint32_t size_out = g32(job_ea + JH_SIZE_OUT);
    uint32_t size_stk = (uint32_t)g16(job_ea + JH_SIZE_STACK) << 4;
    uint32_t size_scr = (uint32_t)g16(job_ea + JH_SIZE_SCRATCH) << 4;
    uint32_t n_cache  = g32(job_ea + JH_SIZE_CACHE_DMA_LIST) / 8u;

    if (n_cache > 4) n_cache = 4;                    /* jm2 caps at 4 */
    if (!size_stk) size_stk = JOB_STACK_DEFAULT;
    if (!size_bin || size_bin > SPU_LS_SIZE) {
        fprintf(stderr, "[spurs-job] job 0x%08X: bad sizeBinary=%u -- not run\n",
                job_ea, size_bin);
        return -1;
    }

    /* Stage directly in the final private LS. A second zeroed 256 KiB store
     * and full copy used to be allocated for every geometry fragment. */
    JobStorage* storage = job_storage_take();
    struct JobSnapshot* snapshot = s_current_snapshot;
    spu_context* ctx = &storage->context;
    /* BLEACH_JOB_LS_AUDIT=1: keep clearing all of LS and report jobs that write
     * outside their regions (the partial clear below relies on there being none). */
    static int s_ls_audit = -1;
    if (s_ls_audit < 0) { const char* e = getenv("BLEACH_JOB_LS_AUDIT"); s_ls_audit = e && *e == '1'; }
    {
        /* spu_context_init without the 256 KiB store: everything else as zeroed state. */
        uint8_t* base = (uint8_t*)ctx;
        const size_t ls_begin = offsetof(spu_context, ls_store), ls_end = ls_begin + SPU_LS_SIZE;
        memset(base, 0, ls_begin);
        memset(base + ls_end, 0, sizeof(*ctx) - ls_end);
        ctx->ls = ctx->ls_store; ctx->spu_id = 0; ctx->status = SPU_STATUS_STOPPED;
        /* As jm2 on the PS3, the job's buffers (I/O, output, scratch, stack) keep what
         * the previous job left; only the gap after the binary (its .bss) starts zeroed.
         * BLEACH_JOB_LS_CLEAR=regions clears every byte a previous job may have written. */
        static int s_hw_clear = -1;
        if (s_hw_clear < 0) { const char* e = getenv("BLEACH_JOB_LS_CLEAR"); s_hw_clear = !(e && !strcmp(e, "regions")); }
        uint32_t dirty = s_ls_audit ? SPU_LS_SIZE : storage->ls_dirty;
        if (s_hw_clear && !s_ls_audit && dirty > ALIGN1024(size_bin)) dirty = ALIGN1024(size_bin);
        if (dirty > size_bin) memset(ctx->ls_store + size_bin, 0, dirty - size_bin);
        memset(ctx->ls_store + SPU_LS_SIZE - 0x440, 0, 0x440);   /* context + descriptor */
    }
    ctx->image_id = image_id;
    uint8_t* ls = ctx->ls;

    /* ---- binary at LS 0 (PIC, so delta 0 keeps the lifted addresses) ---- */
    memcpy(ls, vm_base + ea_bin, size_bin);

    /* ---- regions, following _cellSpursCheckJob's own arithmetic --------- */
    uint32_t p         = ALIGN1024(size_bin);
    /* Same rule as the scratch pointer: these are REGION BASES and stay valid
     * when the region is empty. NULL invites the job to dereference 0 -- and
     * since the job binary loads at LS 0, reading through a null base returns
     * the job's OWN INSTRUCTION WORDS. That is exactly what Tokyo Jungle's
     * wedged jobs were doing: each spun DMAing to the word at offset 0x64 of
     * its own image, which is why the addresses were misaligned and looked
     * like floats -- they were opcodes. */
    uint32_t io_ls     = p;                p += ALIGN1024(size_io);
    uint32_t out_ls    = p;                p += ALIGN1024(size_out);
    if (use_io && !out_ls) out_ls = io_ls;    /* in-out: output shares ioBuffer */
    uint32_t ss_size   = ALIGN1024(size_scr + size_stk);
    /* The scratch pointer is the BASE OF THE SCRATCH+STACK BLOCK, and it is
     * valid even when sizeScratch is 0 -- a zero-length buffer still has an
     * address. Handing the job a NULL here meant it dereferenced 0 and read
     * ITS OWN CODE as data: every wedged job in Tokyo Jungle was loading a
     * word from LS 0x64 and using that SPU instruction word as a DMA address,
     * which is why the addresses looked like misaligned floats. */
    uint32_t s_ls      = p;
    uint32_t stack_top = p + ss_size;
    p += ss_size;

    /* ---- cache-hinted read-only inputs ---------------------------------- */
    uint32_t dma_base   = job_ea + JH_SIZE;          /* input list ... */
    uint32_t cache_base = dma_base + n_dma * 8u;     /* ... then cache list */
    uint32_t cache_ls[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < n_cache; i++) {
        uint64_t e   = g64(cache_base + i * 8u);
        uint32_t sz  = (uint32_t)(e >> 32);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        if (p + sz > SPU_LS_SIZE) break;
        cache_ls[i] = p;
        memcpy(ls + p, vm_base + eal, sz);
        p += ALIGN1024(sz);
    }

    /* SPURS_JOB_FIND=<hex ea>[,<hex ea>] (debug): the first 12 jobs whose input or cache DMA
     * list covers one of these addresses print their descriptor (header + 0x100 bytes). */
    { static uint32_t find[2] = {1, 0}; static volatile long found;
      if (find[0] == 1) { const char* e = getenv("SPURS_JOB_FIND"); find[0] = 0;
          if (e) { char* q; find[0] = (uint32_t)strtoul(e, &q, 16); if (*q == ',') find[1] = (uint32_t)strtoul(q + 1, 0, 16); } }
      if (find[0]) {
          int hit = 0;
          for (uint32_t i = 0; i < n_dma + n_cache && !hit; i++) {
              uint64_t e = g64(dma_base + i * 8u);
              uint32_t sz = (uint32_t)((e >> 32) & 0x7FFFu), eal = (uint32_t)e;
              for (int k = 0; k < 2; k++) if (find[k] && eal <= find[k] && find[k] < eal + sz) hit = 1;
          }
          for (int k = 0; k < 2; k++) if (find[k] && job_ea == find[k]) hit = 1;   /* or the descriptor itself */
          if (hit && InterlockedIncrement(&found) <= 12) {
              fprintf(stderr, "[job-find] job=0x%08X image=%d bin=0x%08X size_bin=%u n_dma=%u n_cache=%u io=%u out=%u type=%08X:",
                      job_ea, image_id, ea_bin, size_bin, n_dma, n_cache, size_io, size_out, g32(job_ea + JH_JOB_TYPE));
              for (uint32_t o = 0x30; o < 0x130; o += 4) { if ((o & 31) == 16) fprintf(stderr, "\n    +%02X:", o); fprintf(stderr, " %08X", g32(job_ea + o)); }
              fprintf(stderr, "\n"); fflush(stderr);
          }
      } }

    /* ---- input DMA list -> ioBuffer, each entry 16-byte aligned ---------- */
    uint32_t io_cur = io_ls, io_used = 0;
    for (uint32_t i = 0; i < n_dma; i++) {
        uint64_t e   = g64(dma_base + i * 8u);
        uint32_t sz  = (uint32_t)((e >> 32) & 0x7FFFu);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        if (!io_ls || io_cur + sz > SPU_LS_SIZE) break;
        memcpy(ls + io_cur, vm_base + eal, sz);
        io_cur  += (sz + 15u) & ~15u;
        io_used += (sz + 15u) & ~15u;
    }

    /* ---- context + descriptor, parked above the job's memory region ------
     * jm2 keeps these in its own bss (outside job memory); we load the job at
     * LS 0, so the top of LS is the equivalent free space. */
    uint32_t desc_ls = SPU_LS_SIZE - 0x400;     /* descriptors are <= 1024 B */
    uint32_t ctx_ls  = desc_ls - 0x40;          /* CellSpursJobContext2, 48 B */
    uint32_t dsz     = job_desc_size ? job_desc_size : 256u;
    if (dsz > 0x400) dsz = 0x400;
    if (p > ctx_ls)
        fprintf(stderr, "[spurs-job] job 0x%08X: WARNING regions reach 0x%X, "
                        "past the context at 0x%X\n", job_ea, p, ctx_ls);
    memcpy(ls + desc_ls, vm_base + job_ea, dsz);

    ls32(ls, ctx_ls + JC_IO_BUFFER, io_ls);
    for (int i = 0; i < 4; i++)
        ls32(ls, ctx_ls + JC_CACHE_BUFFER + (uint32_t)i * 4u, cache_ls[i]);
    /* sizeJobDescriptor:4 is the first bitfield of a big-endian u32, so it
     * occupies the TOP 4 bits. 0 means 64 bytes, otherwise n*128. */
    /* The low 28 bits are not padding to a "JOBCRT Ver13" crt (WWS/Edge jobs,
     * Twisted Metal's JobQueue jobs): jm2 parks an LS pointer to its kernel
     * block there, and the crt returns 0x80410A11 and STOPs before main when
     * it is 0 -- so the job never runs and never clears its completion
     * counter. Hand it the free 16 bytes after the context (zeroed); the crt
     * only stashes the pointer and clears the field again. */
    uint32_t jm_iface = memcmp(ls + 0x30, "JOBCRT Ver13", 12) == 0 ? ctx_ls + 0x30 : 0;
    /* The same crt also reads the SPURS kernel context with ABSOLUTE loads
     * (lqa): spuNum at 0x1C8 (cellSpursGetCurrentSpuId), the trace buffer at
     * 0x210, a trace-enable byte at 0x175. On hardware the kernel owns
     * 0x100-0x2FF; here the job is loaded at 0, so those reads returned the
     * job's own instruction words and it indexed per-SPU tables with an opcode
     * and halted. The lifted code never reads those bytes as data, so overlay
     * the kernel's view: SPU 0, our DMA tag, tracing off. */
    if (jm_iface) {
        memset(ls + 0x170, 0, 0x10);
        memset(ls + 0x1C0, 0, 0x10);
        ls32(ls, 0x1C8, g_spurs_job_spu_num);
        ls32(ls, 0x1CC, JOB_DMA_TAG);
        memset(ls + 0x210, 0, 0x10);
    }
    ls32(ls, ctx_ls + JC_SIZE_JOB_DESC,
         ((dsz == 64 ? 0u : (dsz / 128u) & 0xFu) << 28) | jm_iface);
    ls16(ls, ctx_ls + JC_NUM_IO_BUFFER,    (uint16_t)n_dma);
    ls16(ls, ctx_ls + JC_NUM_CACHE_BUFFER, (uint16_t)n_cache);
    ls32(ls, ctx_ls + JC_O_BUFFER, out_ls);
    ls32(ls, ctx_ls + JC_S_BUFFER, s_ls);
    ls32(ls, ctx_ls + JC_DMA_TAG,  JOB_DMA_TAG);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR,     0);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR + 4, job_ea);

    { static SPURS_JOB_TLS int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr,
              "[spurs-job] job 0x%08X: bin 0x%08X+%u -> LS 0 | io=0x%05X(%u, %u dma, %u used)"
              " out=0x%05X(%u) scratch=0x%05X(%u) stack_top=0x%05X(%u) cache=%u"
              " ctx=0x%05X desc=0x%05X(%u)\n",
              job_ea, ea_bin, size_bin, io_ls, size_io, n_dma, io_used,
              out_ls, size_out, s_ls, size_scr, stack_top, size_stk, n_cache,
              ctx_ls, desc_ls, dsz); }

    /* SPURS_JOB_DESCDUMP=1: hexdump the guest job descriptor. The SPU derives
     * its DMA and atomic EAs from these words, so when a job spins on a
     * lock-line address that is not backed, this is what to read first. */
    { static SPURS_JOB_TLS int _d = 0;
      if (getenv("SPURS_JOB_DESCDUMP") && PS3_LOG_FIRST(_d, 4)) {
          fprintf(stderr, "[spurs-job] desc @0x%08X (%u bytes):", job_ea, dsz);
          for (uint32_t o = 0; o < dsz && o < 128; o += 4) {
              if ((o & 31) == 0) fprintf(stderr, "\n    +%02X:", o);
              fprintf(stderr, " %08X", g32(job_ea + o));
          }
          fprintf(stderr, "\n");
          /* SPURS_JOB_DESCDUMP=2: follow plausible guest EAs in the job user
           * data (past JH_SIZE) and dump what they point at. The job reads its
           * parameters from there, so a wild DMA/atomic address usually
           * originates in one of these blocks. */
          if (atoi(getenv("SPURS_JOB_DESCDUMP")) >= 2) {
              for (uint32_t o = JH_SIZE; o < dsz && o < 128; o += 4) {
                  uint32_t v = g32(job_ea + o);
                  if (v < 0x10000u || v >= 0xD0000000u) continue;
                  fprintf(stderr, "[spurs-job]   user+0x%02X -> 0x%08X:", o, v);
                  for (uint32_t q = 0; q < 8; q++)
                      fprintf(stderr, " %08X", g32(v + q * 4));
                  fprintf(stderr, "\n");
              }
          }
      } }

    /* LBP job protocol probe (SPURS_JOB_DUMP): the game's one shared job binary
     * ("JOBCRT Ver13" crt + main @0x1570) reads a be u64 EA out of the
     * descriptor's USER DATA at +0x30, GETs a 128-byte command block from it,
     * then jump-tables on the command TYPE (word1 of the block, cases 0..5).
     * The header's input-DMA-list/io fields are legitimately zero for it. Dump
     * the user data + that command block so the bail path is attributable. */
    uint32_t dump_cmd = 0;
    if (getenv("SPURS_JOB_DUMP")) {
        uint64_t ud  = g64(job_ea + JH_SIZE);        /* desc+0x30 */
        uint32_t cmd = (uint32_t)ud;                 /* low word = EA */
        fprintf(stderr, "[spurs-job] job 0x%08X userdata: %016llX %016llX %016llX %016llX\n",
                job_ea, (unsigned long long)ud,
                (unsigned long long)g64(job_ea + JH_SIZE + 8),
                (unsigned long long)g64(job_ea + JH_SIZE + 16),
                (unsigned long long)g64(job_ea + JH_SIZE + 24));
        if (cmd && cmd < 0x50000000u) {
            dump_cmd = cmd;
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X:", cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(cmd + o));
            }
            fprintf(stderr, "\n");
            /* cmdblock+0x1C points at a 2048-byte WORK-ITEM table the type-0/1
             * handler GETs (pc=0x1A78) before entering its per-item worker with
             * count = cmdblock+0x24. Dump its head: all-zero means the game
             * never filled the items (upstream/ordering); real entries mean the
             * SPU worker mishandles them. */
            uint32_t tbl = g32(cmd + 0x1C);
            if (tbl && tbl < 0x50000000u) {
                fprintf(stderr, "[spurs-job]   worktable @0x%08X (n=%u):", tbl, g32(cmd + 0x24));
                for (int o = 0; o < 96; o += 4) {
                    if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                    fprintf(stderr, " %08X", g32(tbl + o));
                }
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }

    /* ---- enter the job -------------------------------------------------- */
    /* SPU stack grows down. A function saves its link register in its
     * caller's frame, at 16($sp), so the entry's first store lands 16 bytes
     * above $sp. The cache buffers start right at stack_top: with $sp at
     * stack_top - 16 that save overwrote the first quadword of
     * cacheBuffer[0] (Twisted Metal's Edge jobs read their viewport there and
     * got width = height = 0). Leave the caller's 32-byte frame inside the
     * stack. */
    ctx->gpr[1]._u32[0] = (stack_top - 32u) & ~15u;
    /* SPU ABI R1 word 1 is the remaining stack space, decremented along
     * with the pointer by function prologues. Leaving it zero makes the
     * first stack-checked Edge routine report overflow with ample LS free.
     * Job chains use a unified scratch/stack region. Its entire free span
     * belongs here: using sizeStack alone places the CRT's bottom canary
     * inside the scratch buffer where the job legitimately writes. */
    ctx->gpr[1]._u32[1] = ss_size - 32u;
    /* Link register: where the job returns when it is done. The job manager
     * would pass an address inside itself; 0 makes the job re-enter its own
     * entry at LS 0 and run a bogus second lap. */
    ctx->gpr[0]._u32[0] = 0x3FF00u;                   /* SPU_JOB_RETURN_LS */
    ctx->gpr[3]._u32[0] = ctx_ls;                     /* CellSpursJobContext2* */
    ctx->gpr[4]._u32[0] = desc_ls;                    /* CellSpursJob256*      */

    { static SPURS_JOB_TLS int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr, "[spurs-job] ENTER r3=%08X r4=%08X r1=%08X entry-fn image=%d\n",
                  ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], ctx->gpr[1]._u32[0],
                  image_id); }
    /* NOTE: after its work the job's runtime jumps to LS 0 ("return to the
     * jm2 kernel" -- real layout has the kernel at 0, jobs above; we load the
     * job at 0). That second lap re-enters the job's crt with dead registers
     * and trips its parameter guard, which HALTS -- ending the run. So the
     * HALT-ASSERT heqi 0x1650 seen once per jm2 job is NORMAL COMPLETION
     * noise, not a failure: the job's real work finished before the jump. */
    g_spurs_current_job_ea = job_ea;
    const int hold_ratio = bleach_tick_ratio ? bleach_tick_ratio() : 1;
    const int hold = hold_ratio > 1 && spu_hold_binary(ea_bin);
    if (hold) { t_hold_ngets = t_hold_bytes = t_hold_nputs = 0; g_spu_hold_capture = 1; }
    const uint64_t job_trace_begin = PS3_FT_ON() ? ps3_ft_now() : 0;
    if (g_spurs_native_job_run) g_spurs_native_job_run(entry,ctx,image_id);
    else spu_run_with_halt(entry, ctx);
    if (hold) {
        g_spu_hold_capture = 0;
        if (!spu_hold_commits(job_ea, hold_ratio)) {
            /* Not this descriptor's run of an original update: what it read and wrote back
             * returns to the bytes it read. Stretches it only wrote stay (they are drawn). */
            for (uint32_t i = 0; i < t_hold_nputs; i += 2) {
                const uint32_t pa = t_hold_puts[i], pb = pa + t_hold_puts[i + 1];
                for (uint32_t g = 0; g < t_hold_ngets; g++) {
                    const uint32_t ga = t_hold_gets[g].ea, gb = ga + t_hold_gets[g].size;
                    const uint32_t a = pa > ga ? pa : ga, b = pb < gb ? pb : gb;
                    if (a < b) memcpy(vm_base + a, t_hold_data + t_hold_gets[g].at + (a - ga), b - a);
                }
            }
        }
    }
    {
        uint32_t extent = p > size_bin ? p : size_bin;
        if (s_ls_audit) {
            uint32_t first = 0, bytes = 0;
            for (uint32_t a = extent; a < ctx_ls; a++) if (ls[a]) { if (!bytes++) first = a; }
            if (bytes) {
                static volatile LONG reported;
                if (InterlockedIncrement(&reported) <= 32)
                    fprintf(stderr, "[ls-audit] image=%d job=%08X wrote %u bytes past its regions (0x%05X..0x%05X), first at 0x%05X\n",
                            image_id, job_ea, bytes, extent, ctx_ls, first);
            }
            static volatile LONG audited;
            if ((InterlockedIncrement(&audited) & 0xFFFF) == 0)
                fprintf(stderr, "[ls-audit] %ld jobs checked\n", (long)audited);
        }
        storage->ls_dirty = extent;
    }
    if (job_trace_begin)
        /* Host worker IDs keep independent audio/geometry pools distinct. */
        ps3_ft_span(PS3_FT_SPU_BUSY, (uint32_t)GetCurrentThreadId(), (uint32_t)image_id,
            snapshot ? snapshot->handle : 0,
            job_trace_begin, ps3_ft_now());
    if ((ctx->pc != 0x3FF00u || (ctx->status & SPU_STATUS_STOPPED_BY_HALT)) && getenv("PS3_SPU_STRICT")) {
        const char* dump = getenv("SPURS_JOB_FAIL_DUMP");
        if (dump && *dump) {
            FILE* f = fopen(dump, "wb");
            if (f) {
                fwrite(ctx->gpr, sizeof(ctx->gpr), 1, f);
                fwrite(ctx->ls, 1, SPU_LS_SIZE, f);
                fwrite(ls, 1, SPU_LS_SIZE, f);
                fclose(f);
            }
            fprintf(stderr, "[spurs-job-fail] image=%d job=%08X io=%05X/%u out=%05X/%u scratch=%05X/%u stack=%05X/%u r1=%08X,%08X\n",
                    image_id, job_ea, io_ls, size_io, out_ls, size_out, s_ls, size_scr,
                    stack_top, size_stk, ctx->gpr[1]._u32[0], ctx->gpr[1]._u32[1]);
        }
        fprintf(stderr, "[bleach] STOP: SPU termination requires investigation; image=%d pc=0x%05X stop=0x%X\n",
                image_id, ctx->pc, ctx->stop_code);
        fflush(stderr);
        exit(79);
    }

    /* Capture the job's outbound mailbox. These are LS POINTERS TO STRINGS for
     * the SPU printf service, NOT query results -- value 0x83C0 from the sound
     * job points at " compressor, 1/n..." inside its own image. An earlier
     * comment here claimed they carried the title's bus counts; they do not,
     * and feeding them into the completion event turned 0x40000000 into a 1 GB
     * allocation request. Kept because the printf service needs them. */
    /* TEMP SPURS_JOB_LSWORD=<img>:<lsa>: after a job of that image, print the
     * LS word at lsa and every other LS offset holding the same value. */
    { static SPURS_JOB_TLS int img = -2; static SPURS_JOB_TLS uint32_t lsa; static SPURS_JOB_TLS int n = 0;
      if (img == -2) { const char* e = getenv("SPURS_JOB_LSWORD"); img = -1;
          if (e) { img = atoi(e); const char* c = strchr(e, ':'); lsa = c ? (uint32_t)strtoul(c + 1, 0, 16) : 0; } }
      if (img == image_id && n++ < 3) {
          const uint8_t* q = ctx->ls + (lsa & 0x3FFFC);
          uint32_t v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
          fprintf(stderr, "[lsword] img=%d job=0x%08X LS[0x%05X]=0x%08X io=0x%05X also at:", image_id, job_ea, lsa, v, io_ls);
          for (uint32_t o = 0; o + 4 <= SPU_LS_SIZE; o += 4) {
              const uint8_t* p = ctx->ls + o;
              if (o != (lsa & 0x3FFFC) && ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) == v)
                  fprintf(stderr, " 0x%05X", o);
          }
          fprintf(stderr, "\n");
      } }

    g_spurs_job_mbox      = spu_channel_has_data(&ctx->ch_out_mbox)
                          ? ctx->ch_out_mbox.value : 0;
    g_spurs_job_mbox_intr = spu_channel_has_data(&ctx->ch_out_intr_mbox)
                          ? ctx->ch_out_intr_mbox.value : 0;
    g_spurs_job_mbox_valid = g_spurs_job_mbox || g_spurs_job_mbox_intr;
    { uint32_t _cb = g32(job_ea + 0x4C);
      g_spurs_job_cmd = _cb ? (g32(_cb) >> 16) : 0; }
    { static SPURS_JOB_TLS int s_t = -1; if (s_t < 0) s_t = getenv("SPURS_JOB_MBOX") ? 1 : 0;
      static SPURS_JOB_TLS int n = 0;
      if (s_t && PS3_LOG_FIRST(n, 16) && g_spurs_job_mbox_valid)
          fprintf(stderr, "[spurs-job] job 0x%08X posted mbox=0x%08X intr=0x%08X\n",
                  job_ea, g_spurs_job_mbox, g_spurs_job_mbox_intr); }

    if (getenv("SPURS_JOB_DUMP")) {
        fprintf(stderr, "[spurs-job] job 0x%08X exit: status=0x%X stop=0x%X pc=0x%05X\n",
                job_ea, ctx->status, ctx->stop_code, ctx->pc);
        /* Post-run view of the same command block: the game polls result/status
         * fields the job writes back -- a before/after diff shows whether the
         * handler delivered its completion or the PPU waits on stale state. */
        if (dump_cmd) {
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X AFTER:", dump_cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(dump_cmd + o));
            }
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }

    spu_mfc_release(ctx);
    spu_coh_unregister(ctx);
    /* Publish only after the final access and runtime deregistration. Another
     * worker may evict and reset this context immediately after publication. */
    job_storage_publish(snapshot, storage);
    return 0;
}
