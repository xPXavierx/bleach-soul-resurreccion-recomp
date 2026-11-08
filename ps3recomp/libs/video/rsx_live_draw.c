/*
 * ps3recomp - Track B live NV4097 draw path (implementation)
 *
 * See rsx_live_draw.h. This is the validated capture-replay D3D12 engine
 * (libs/video/tests/replay_main.c) lifted into a runtime module and driven by
 * the live FIFO consumer instead of an .rxs file:
 *   - rsx_dispatch register-file model (shared, unchanged)
 *   - NV40 VP/FP -> HLSL decompilers (shared, unchanged)
 *   - B1 render/sampler state -> D3D12 PSO + dynamic samplers + mip chains
 *
 * Differences from the harness:
 *   - guest memory comes from an injected resolver (the runtime's vm_base map),
 *     not a private arena;
 *   - present goes to a swap chain bound to the runtime's window, not a PPM
 *     readback;
 *   - the whole engine is gated behind RSX_DRAW (default ON; "0" = off).
 *
 * Clean-room: NV40 ISA/register facts from envytools rnndb + Mesa nv30 +
 * psdevwiki; RPCS3 as a read-only fact oracle only.
 */

#include "rsx_live_draw.h"
#include "rsx_geometry_handoff.h"
#include "rsx_commands.h"
#include "ps3emu/yz_runtime_config.h"
#include "ps3emu/yz_frontier_trace.h"
#include "ps3emu/log_first.h"

static rsx_live_pixel_constants_fn g_pixel_constants_filter;
void rsx_live_draw_set_pixel_constants_filter(rsx_live_pixel_constants_fn callback)
{ g_pixel_constants_filter = callback; }

static rsx_live_source_watch_fn g_source_watch;
void rsx_live_draw_set_source_watch(rsx_live_source_watch_fn callback)
{ g_source_watch = callback; }

#if !defined(_WIN32)

/* Non-Windows: the whole path is a no-op (D3D12 is Windows-only). */
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_surface_view_stats(rsx_surface_view_stats* s)
{ if(s)*s=(rsx_surface_view_stats){0}; }
int  rsx_live_draw_init(void* hwnd, u32 w, u32 h, rsx_live_guest_ptr_fn f, void* u)
{ (void)hwnd; (void)w; (void)h; (void)f; (void)u; return 0; }
void rsx_live_draw_seed_registers(const u32* r, u32 n) { (void)r; (void)n; }
void rsx_live_draw_seed_transform_program(const u32* w, u32 n) { (void)w; (void)n; }
void rsx_live_draw_seed_transform_constants(const u32* w, u32 n) { (void)w; (void)n; }
void rsx_live_draw_set_display_buffer(
    u32 b, u32 l, u32 o, u32 p, u32 w, u32 h)
{ (void)b; (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_live_draw_method(u32 m, u32 a) { (void)m; (void)a; }
int rsx_live_draw_native_ingress(void) { return 0; }
void rsx_live_draw_packet(u32 m,const void* p,u32 n,int i) {(void)m;(void)p;(void)n;(void)i;}
void rsx_live_draw_set_fifo_position(u32 g, u32 p) { (void)g; (void)p; }
void rsx_live_draw_note_inline_transfer(u32 d, u32 o, u32 v)
{ (void)d; (void)o; (void)v; }
int  rsx_live_draw_blit(u32 sl, u32 sa, u32 sp, u32 dl, u32 da, u32 dp, u32 w, u32 h)
{ (void)sl; (void)sa; (void)sp; (void)dl; (void)da; (void)dp; (void)w; (void)h; return 0; }
int  rsx_live_draw_resolve_blit(u32 sl, u32 sa, u32 sp, u32 ds, u32 dt, u32 dl, u32 da,
                                u32 dp, u32 x, u32 y, u32 w, u32 h)
{ (void)sl; (void)sa; (void)sp; (void)ds; (void)dt; (void)dl; (void)da; (void)dp;
  (void)x; (void)y; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 sl, u32 sa, u32 dl, u32 da)
{ (void)sl; (void)sa; (void)dl; (void)da; }
void rsx_live_draw_flush(void) {}
int rsx_live_draw_set_frames_in_flight(u32 count) { return count==1; }
u32 rsx_live_draw_frames_in_flight(void) { return 0; }
u64 rsx_live_draw_submitted_fence(void) { return 0; }
u64 rsx_live_draw_completed_fence(void) { return 0; }
void rsx_live_draw_present(u32 b) { (void)b; }
void rsx_live_draw_set_movie_mode(int on) { (void)on; }
void rsx_live_draw_present_rgba(const uint8_t* r, u32 w, u32 h) { (void)r; (void)w; (void)h; }
u32  rsx_live_draw_get_frames(void) { return 0; }
void* rsx_live_draw_get_present_thread_handle(void) { return NULL; }
u32  rsx_live_draw_get_last_draws(void) { return 0; }
double rsx_live_draw_get_present_fps(void) { return 0.0; }
void rsx_live_draw_dump_present_samples(void) {}
void rsx_live_draw_a010_probe_begin(void) {}
int  rsx_live_draw_a010_probe_active(void) { return 0; }
int  rsx_live_draw_a010_world_ready(void) { return 1; }
int  rsx_live_draw_debug_dump_surface(u32 l, u32 o, const char* p)
{ (void)l; (void)o; (void)p; return -1; }
void rsx_live_draw_set_a010_camera_matrix(const float* m) { (void)m; }
void rsx_live_draw_shutdown(void) {}
void rsx_live_draw_depth_view_stats(rsx_depth_view_stats* stats) { if(stats) *stats=(rsx_depth_view_stats){0}; }

#else /* _WIN32 */

#define _CRT_SECURE_NO_WARNINGS

#include "rsx_dispatch.h"
#include "rsx_fp_decompiler.h"
static unsigned long long g_ld_bind_white = 0, g_ld_bind_real = 0, g_ld_bind_surf = 0;
/* Set once PS1 VRAM actually holds a drawn frame (see the PS1_PC heartbeat).
 * Every surface dump before this is an early-boot snapshot of a framebuffer the
 * PS1 has not drawn into yet -- which is what made nonblack=0 look like a broken
 * pipeline when it was the correct reading of an empty source. */
static volatile long g_ld_ps1_vram_ready = 0;
#include "rsx_restart_cuts.h"
#include "rsx_source_cache.h"
#include "rsx_resolution.h"
#include "rsx_aspect.h"
#include "rsx_aspect_layout.h"
#include "rsx_vertex_compact.h"
#include "rsx_vertex_pull.h"
#include "rsx_vp_decompiler.h"
#include "rsx_native_program.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(YZ_PERF_CLEAN)
#define LD_DIAG_ENABLED(name) 0
#else
/* Cached per call site: several of these sit on per-draw paths. */
#define LD_DIAG_ENABLED(name) ({ static int _ld_diag = -1; \
    if (_ld_diag < 0) _ld_diag = getenv(name) != NULL; _ld_diag; })
#endif
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include "rsx_vertex_formats.h"
#include <sys/stat.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* Armed by the a010 AUTH repair only after its animation palette is resident. */
extern volatile LONG g_yz_a010_reference_camera_active;
/* Published when the Sunshine Orphanage AUTH root becomes live. */
extern volatile LONG g_yz_a010_root_active;
/* Opt-in headless gameplay-proof handshake, owned jointly with cellPad.c. */
extern volatile LONG g_yz_movement_proof_phase;
extern volatile unsigned long long g_yz_auto_start_tick;

/* ---------------------------------------------------------------------------
 * Engine state (module-static; single live RSX)
 * -----------------------------------------------------------------------*/

#define LD_SWAP_BUFFERS  2
#define MAX_SURFACES     64
#define MAX_TEXTURES     1024
#define MAX_RETIRED_TEXTURES 4096
#define MAX_VTEX         64
#define MAX_SAMPLERS     128
/*
 * A full live boot carries shader pairs from startup movies, menus, and AUTH
 * into the orphanage.  The old 256-entry cap was already exhausted there,
 * causing 155/328 sampled a010 groups to return NULL from get_pso().  The
 * one-frame reference alone uses 197 pairs.  A full visible boot through
 * orphanage and into Akiyama measured 2,296 distinct requested PSO keys before
 * the first presentation stall, so use the next power of two with headroom.
 */
#define MAX_PSOS         8192
#define FORMER_MAX_SHADER_BLOBS 2048
#define MAX_SHADER_BLOBS 8192
#define MAX_REJECTED_PSO_KEYS 8192
#define UPLOAD_SIZE      (64u * 1024 * 1024)
#define SHADER_DISK_CACHE_MAGIC   0x43535A59u /* "YZSC" */
/* v2: OP_KIL now emits discard (was a dropped TODO) â€” cached v1 pixel
 * shaders would silently keep the old no-discard codegen. */
#define SHADER_DISK_CACHE_VERSION 2u
#define SHADER_DISK_CACHE_MAX_BLOB (4u * 1024u * 1024u)

#define SRV_WHITE        0
#define SRV_SURFACE_BASE 1
#define SRV_ZDEPTH_BASE  (SRV_SURFACE_BASE + MAX_SURFACES)
#define SRV_TEXTURE_BASE (SRV_ZDEPTH_BASE + MAX_SURFACES)
#define SRV_VTEX_BASE    (SRV_TEXTURE_BASE + MAX_TEXTURES)
#define SRV_HEAP_SLOTS   (SRV_VTEX_BASE + MAX_VTEX)
#define SRV_DEPTH_SOURCE_BASE SRV_HEAP_SLOTS
#define UAV_ZDEPTH_BASE   (SRV_DEPTH_SOURCE_BASE + MAX_SURFACES)
/* Per-surface copy a draw samples when it reads its own render target. */
#define SRV_STENCIL_SOURCE_BASE (UAV_ZDEPTH_BASE + MAX_SURFACES)
#define UAV_PACKED_DEPTH_BASE (SRV_STENCIL_SOURCE_BASE + MAX_SURFACES)
/* Each unit can use a different remap in the same immutable draw table. */
#define SRV_PACKED_DEPTH_BASE (UAV_PACKED_DEPTH_BASE + MAX_SURFACES)
#define SRV_FEEDBACK_BASE (SRV_PACKED_DEPTH_BASE + MAX_SURFACES * SRV_TABLE_SIZE)
/* Top-left windows of a surface for smaller textures that alias it (see surface_window). */
#define LD_SWIN_PER       2
#define SRV_SURF_WIN_BASE (SRV_FEEDBACK_BASE + MAX_SURFACES)
#define LD_VIEW_CACHE_CAP 1024
#define SRV_VIEW_CACHE_BASE (SRV_SURF_WIN_BASE + MAX_SURFACES * LD_SWIN_PER)
#define SRV_HOST_IMAGE_BASE (SRV_VIEW_CACHE_BASE + LD_VIEW_CACHE_CAP)
/* A full-scale surface seen at its guest size by reduced passes (surface_logical_view). */
#define SRV_LOGICAL_VIEW_BASE (SRV_HOST_IMAGE_BASE + 2)
#define SRV_CPU_HEAP_SLOTS (SRV_LOGICAL_VIEW_BASE + MAX_SURFACES)
#define RTV_HOST_IMAGE_BASE (LD_SWAP_BUFFERS + 2 * MAX_SURFACES)
#define RTV_LOGICAL_VIEW_BASE (RTV_HOST_IMAGE_BASE + 2)
#define RTV_VIEW_CACHE_BASE (RTV_LOGICAL_VIEW_BASE + MAX_SURFACES)   /* box-reduced surface views */
#define SRV_TABLE_SIZE   16
#define SRV_RING_TABLES  4096

#define SMP_DEFAULT      0
#define SMP_CACHE_SLOTS  MAX_SAMPLERS
#define SMP_TABLE_SIZE   16
/* A shader-visible SAMPLER heap is hard-capped at 2048 descriptors by D3D12
 * (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE). SMP_RING_TABLES*SMP_TABLE_SIZE
 * must stay <= 2048, else CreateDescriptorHeap fails. Identical sampler
 * tables share entries, so this is 128 unique tables between GPU flushes. */
#define SMP_RING_TABLES  128

#define CB_BLOCK_BYTES   ((512 + 2) * 16)
#define CB_BLOCK_ALIGNED ((CB_BLOCK_BYTES + 255) & ~255u)
#define CB_RING_BYTES    (CB_BLOCK_ALIGNED * SRV_RING_TABLES)
#define PS_CB_RING_BYTES CB_RING_BYTES
#define LD_VARIANT_SET_CAPACITY (MAX_SHADER_BLOBS * 2u)

#define VERT_STRIDE      (16 * 4 * 4)   /* 16 attrs * float4                  */
#define VERT_BUFFER_SIZE (256u * 1024u * 1024u)
#define INDEX_BUFFER_SIZE (64u * 1024u * 1024u)
#define LD_INVALID_SURFACE 0xFFFFFFFFu

/* gcm texture format bytes (mirror rsx_dispatch.h) */
#define TEX_FMT_B8         0x81
#define TEX_FMT_A1R5G5B5   0x82
#define TEX_FMT_A4R4G4B4   0x83
#define TEX_FMT_R5G6B5     0x84
#define TEX_FMT_A8R8G8B8   0x85
#define TEX_FMT_DXT1       0x86
#define TEX_FMT_DXT23      0x87
#define TEX_FMT_DXT45      0x88
#define TEX_FMT_G8B8       0x8B
#define TEX_FMT_DEPTH24_D8 0x90
#define TEX_FMT_LINEAR     0x20
#define TEX_FMT_UNNORM     0x40
#define TEX_FMT_BASE_MASK  0x9F

typedef struct {
    u32 location, offset;
    ID3D12Resource* tex;
    u32 w, h;                 /* logical guest texels; resource extent is physical */
    float scale;              /* physical texels per logical one (ld_target_scale) */
    DXGI_FORMAT fmt;
    ID3D12Resource* fb_tex;   /* feedback copy, see surface_feedback_srv */
    u32 pitch;                /* guest row pitch of colour target 0 (bytes), 0 = unknown */
    /* Top-left windows of the surface for smaller aliasing textures (see surface_window). */
    struct { ID3D12Resource* tex; ID3D12Resource* src; u32 w, h, wgen, used; } swin[LD_SWIN_PER];
    u32 swin_wgen;            /* bumped by every write into the surface; a window older than it is recopied */
    /* The surface at its guest size, for reduced passes (surface_logical_view). */
    ID3D12Resource* lview;
    ID3D12Resource* lview_src;
    u32 lview_wgen;
#if !defined(YZ_PERF_CLEAN)
    u32 resource_serial;
    u32 last_write_generation;
    u32 color_content_generation;
    u32 last_write_kind;
    u32 last_create_generation;
    u32 last_draw_generation;
    u32 last_clear_generation;
    u32 last_copy_generation;
    u32 last_blit_generation;
    u32 last_resolve_generation;
    u32 last_other_generation;
    u32 last_guest_blit_generation;
    u32 last_present_copy_generation;
#endif
} surface_t;
typedef struct {
    u32 location, offset, pitch, width, height;
    int valid;
    int resolve_src;   /* surface slot + 1 a scaled blit resolves into this buffer; 0 = none */
} display_buffer_t;
typedef struct {
    u32 location, offset;
    ID3D12Resource* tex;
    ID3D12Resource* snapshot;
    ID3D12Resource* packed;
    u32 packed_w, packed_h, packed_remap[SRV_TABLE_SIZE];
    u32 packed_remap_mask;
    u32 pitch, depth_format, raster_type, content_generation;
    D3D12_RESOURCE_STATES packed_state;
    int packed_valid;
    u64 packed_resolves, packed_binds;
    u32 w, h;
    u32 snapshot_w, snapshot_h; /* logical SRV window; GPU dispatch uses physical extent */
    D3D12_RESOURCE_STATES snapshot_state;
    int cleared;
    int had_write;
    int snapshot_valid;
    u64 draws;
    u64 depth_test_draws;
    u64 depth_write_draws;
    u64 depth_both_draws;
    int reject_logged;
} zdepth_t;
typedef struct {
    u32 location, offset, format, width, height, pitch, remap;
    u32 cubemap, mipmaps, dimension;
    rsx_source_snapshot source;
    uint64_t source_epoch;
    ID3D12Resource* tex;
    u64 content_hash;
    u32 last_hash_frame;
    u64 last_use_serial;
} texcache_t;
typedef struct {
    u32 location, offset, format, width, height, pitch;
    ID3D12Resource* tex;
    u64 content_hash;
    u32 last_hash_frame;
} vtexcache_t;
/* Resource dependencies belong to the executable shader, not to stale RSX
 * enable bits. Keep this immutable plan with the PSO, including all branches.
 * It drives uploads, alias resolution, snapshots and resource barriers. */
typedef struct {
    u32 fragment_textures, vertex_textures;
    u64 program; /* immutable fragment-program structure for native policies */
    int position_block; /* c[n..n+3] that ATTR0 is multiplied by, -2 libfont, or -1 (ld_vp_position_block) */
} ld_shader_resources;
typedef struct {
    u64 key;
    ID3D12PipelineState* pso;
    ld_shader_resources resources;
} psocache_t;
typedef struct {
    u64 hash;
    u32 source_length;
    char* source;
    ID3DBlob* blob;
} shader_blob_t;
typedef struct {
    shader_blob_t entries[MAX_SHADER_BLOBS];
    u32 count;
    u64 retained_source_bytes;
    u64 retained_blob_bytes;
} shader_blob_cache_t;
typedef struct {
    u32 magic;
    u32 version;
    u32 stage;
    u32 source_length;
    u32 blob_length;
    u32 reserved;
    u64 source_hash;
} shader_disk_cache_header;

#include "rsx_live_frame_resources.h"
#include "rsx_native_draw_packet.h"
#include "rsx_geometry_gpu_runtime.h"
#include "rsx_native_recording.h"

typedef struct {
    ld_frame_resources frames[2];
    ld_frame_resources* frame;
    u32 frame_index, frame_count;
    int              enabled;    /* RSX_DRAW resolved                     */
    int              ready;      /* device + resources up                    */
    rsx_resolution_domain resolution;
    struct { ID3D12Resource* tex; u32 width, height; } host_image[2];

    ID3D12Device*              dev;
    ID3D12CommandQueue*        queue;
    ID3D12Fence*               fence;
    HANDLE                     fence_event;
    u64                        fence_value;
#if defined(YZ_PERF_PROFILE)
    u64                        timing_frequency;
#endif

    IDXGISwapChain3*           swap;
    UINT                       present_interval;
    BOOL                       allow_tearing;
    ID3D12Resource*            backbuf[LD_SWAP_BUFFERS];
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_step;

    surface_t                  surfaces[MAX_SURFACES];
    u32                        n_surfaces;
    display_buffer_t           display_buffers[8];

    ID3D12DescriptorHeap*      dsv_heap;
    u32                        dsv_step;
    ID3D12Resource*            depth;
    int                        depth_cleared;
    zdepth_t                   zdepths[MAX_SURFACES];
    u32                        n_zdepths;

    ID3D12DescriptorHeap*      srv_cpu_heap;
    u32                        srv_step;
    ID3D12Resource*            white_tex;
    texcache_t                 textures[MAX_TEXTURES];
    u32                        n_textures;
    vtexcache_t                vtex[MAX_VTEX];
    u32                        n_vtex;

    /* Optional host-movie UI compositor. The guest keeps rendering captions
     * into its ordinary offscreen surface. Each guest flip reads back the
     * latest sparse overlay for blending over each 30 Hz host movie frame. */
    ID3D12Resource*            movie_upload;
    u8*                        movie_upload_mapped;
    ID3D12Resource*            movie_overlay_readback;
    u8*                        movie_overlay_rgba;
    u8*                        movie_overlay_mask;
    u32                        movie_overlay_pitch;
    int                        movie_overlay_valid;
    u64                        movie_overlay_frames;

    ID3D12DescriptorHeap*      smp_cpu_heap;
    u32                        smp_step;
    u64                        smp_keys[SMP_CACHE_SLOTS];
    D3D12_SAMPLER_DESC          smp_descs[SMP_CACHE_SLOTS];
    u32                        n_samplers;

    ID3D12RootSignature*       rootsig_x;
    ID3D12RootSignature*       depth_snapshot_rootsig;
    ID3D12PipelineState*       depth_snapshot_pso;
    ID3D12PipelineState*       packed_depth_pso;
    ID3D12RootSignature*       downsample_rootsig;
    ID3D12PipelineState*       downsample_pso[4];
    DXGI_FORMAT                downsample_fmt[4];
    psocache_t                 psos[MAX_PSOS];
    u32                        pso_index[MAX_PSOS * 2];
    u32                        n_psos;
    shader_blob_cache_t        vs_blobs;
    shader_blob_cache_t        ps_blobs;


    /* Separate transient b1 arena for buffered fragment constants. It uses
     * the existing constant-buffer ring budget rather than changing any
     * texture or vertex upload-ring capacity. */
    rsx_fp_constant_block      fp_constants;
    float                      fp_alpha_ref;
    char                       fp_constant_mode;


    u32                        width, height;
    rsx_live_guest_ptr_fn      guest_ptr;
    void*                      guest_user;

    rsx_dispatch               rsx;
    /*
     * Startup-selected Stage-1 isolation mode.  The hot draw path only reads
     * these bits; environment parsing is completed once in init.
     *
     * L = legacy
     * H = hoisted descriptor/base fetch
     * M = H + masked shader signature/input layout
     * C = M + compact payload/stride
     */
    u32                        vertex_features;
    char                       vertex_mode;
    char                       vertex_diag_dir[MAX_PATH];
} ld_state;

static ld_state g;
static u32 g_requested_frames_in_flight=1;
int rsx_live_draw_set_frames_in_flight(u32 count)
{
    if(g.ready || (count!=1 && count!=2))return 0;
    g_requested_frames_in_flight=count;return 1;
}
u32 rsx_live_draw_frames_in_flight(void) { return g.ready?g.frame_count:0; }
u64 rsx_live_draw_submitted_fence(void) { return g.ready?g.fence_value:0; }
u64 rsx_live_draw_completed_fence(void)
{
    if(!g.ready || !g.fence)return 0;
    const u64 value=g.fence->lpVtbl->GetCompletedValue(g.fence);
    /* UINT64_MAX means device removal, never successful completion. */
    return value==UINT64_MAX?0:value;
}

/* Native presentation policy is independent of the title's simulation and
 * FIFO completion. Vsync remains the default; the capability-checked mode
 * removes the monitor's pacing from throughput measurements and supports VRR.
 * Resource lifetime is still governed by the existing D3D12 fence. */
/* BLEACH_PRESENT_PACE_HZ=<hz>: hold each Present to a fixed <hz> grid (an RTSS-style
 * limiter at the presentation edge). Frame start is already paced, but build time varies
 * by ~1 ms, and under VRR/tearing that variance is what the display shows. The grid's
 * phase follows the latest arrivals: a frame up to 1.5 ms late moves the grid to it, a
 * larger isolated spike presents at once without moving it, and while frames arrive
 * early the grid creeps 10 us/frame earlier, so the added latency stays at the arrival
 * jitter. Waits sleep on a high-resolution timer, then spin the last
 * BLEACH_LIMITER_SPIN_US (default 2000) on QPC. */
static void ld_present_pace(void)
{
    static int init; static LONGLONG period, next, spin, step, late_shift, freq; static HANDLE timer;
    if(!init) {
        init=1;
        const char* e=getenv("BLEACH_PRESENT_PACE_HZ"); const int hz=e?atoi(e):0;
        if(hz<=0)return;
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq=f.QuadPart;
        period=freq/hz;
        e=getenv("BLEACH_LIMITER_SPIN_US"); spin=freq*(LONGLONG)(e?atoi(e):2000)/1000000;
        step=freq/100000; late_shift=freq*3/2000;
        timer=CreateWaitableTimerExW(NULL,NULL,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        if(!timer)timer=CreateWaitableTimerW(NULL,FALSE,NULL);
        fprintf(stderr,"[present-pace] %d Hz spin=%lldus\n",hz,(long long)(spin*1000000/freq));
    }
    if(!period)return;
    static LONGLONG waited, waited_max; static unsigned frames, shifts, spikes;
    LARGE_INTEGER now, arrived; QueryPerformanceCounter(&now); arrived=now;
    if(!next||now.QuadPart-next>period) next=now.QuadPart;          /* first frame or a stall: resync */
    else if(now.QuadPart>next) { if(now.QuadPart-next<=late_shift){next=now.QuadPart;++shifts;} else ++spikes; }
    else {
        if(next-now.QuadPart>spin&&timer) {
            LARGE_INTEGER due; due.QuadPart=-(LONGLONG)((next-now.QuadPart-spin)*10000000/freq);
            if(SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE))WaitForSingleObject(timer,INFINITE);
        }
        do { YieldProcessor(); QueryPerformanceCounter(&now); } while(now.QuadPart<next);
        next-=step;
    }
    next+=period;
    /* The added latency: how long frames were held back. */
    { const LONGLONG w=now.QuadPart-arrived.QuadPart; waited+=w; if(w>waited_max)waited_max=w;
      /* Exclusive fullscreen only: how many monitor refreshes each frame stayed on screen
       * (DXGI frame statistics; windowed swap chains report nothing). */
      static UINT last_present, last_sync; static unsigned held[5];
      DXGI_FRAME_STATISTICS st;
      if(g.swap && SUCCEEDED(g.swap->lpVtbl->GetFrameStatistics(g.swap,&st))) {
          if(last_present && st.PresentCount>last_present) {
              const UINT r=(st.SyncRefreshCount-last_sync)/(st.PresentCount-last_present);
              ++held[r<4?r:4];
          }
          last_present=st.PresentCount; last_sync=st.SyncRefreshCount;
      }
      if(++frames==1800) {
          fprintf(stderr,"[present-pace] last %u frames: held avg %.2f ms max %.2f ms, grid shifts %u, spikes %u;"
                  " refreshes per frame 1:%u 2:%u 3:%u 4+:%u\n",
                  frames,waited*1000.0/frames/freq,waited_max*1000.0/freq,shifts,spikes,held[1],held[2],held[3],held[4]);
          waited=waited_max=0; frames=shifts=spikes=0; memset(held,0,sizeof held);
      } }
}
static HRESULT ld_present_swapchain(void)
{
    UINT flags=0;
    ld_present_pace();
    if(!g.present_interval && g.allow_tearing) {
        BOOL fullscreen=TRUE;
        if(SUCCEEDED(g.swap->lpVtbl->GetFullscreenState(g.swap,&fullscreen,NULL)) && !fullscreen)
            flags=DXGI_PRESENT_ALLOW_TEARING;
    }
    return g.swap->lpVtbl->Present(g.swap,g.present_interval,flags);
}
static u32 ld_physical(u32 logical) { return rsx_resolution_extent(g.resolution, logical); }
static LONG ld_physical_edge(LONG logical) { return rsx_resolution_edge(g.resolution, logical); }

/* Scale of a colour target, from its logical extent alone, so the target, its
 * MRT siblings and the pipelines drawn into it always agree.
 *
 * A target no larger than half the display in both dimensions is a
 * reduced-resolution intermediate (bloom/glow chains, blur pyramids) and
 * keeps its guest size. Those passes address their source in whole logical
 * texels -- Bleach's bloom blurs with taps 1/256 apart in a 256-wide level --
 * and at scale 3 such a tap skips two of every three physical texels: the
 * blur becomes a comb that leaves every third row empty, and each light gets
 * striped ghost copies (the halo around lamps, totems and Ignition auras).
 * At their guest size the taps cover the texels they were written for. The
 * passes that read them back into full-size targets sample with normalized
 * coordinates, so they are unaffected. Same idea as RPCS3's minimum scalable
 * dimension. RSX_SCALE_REDUCED_TARGETS=1 scales them anyway (A/B testing). */
static float ld_target_scale(u32 w, u32 h)
{
    static int scale_all = -1;
    if (scale_all < 0) scale_all = getenv("RSX_SCALE_REDUCED_TARGETS") != NULL;
    if (g.resolution.scale == 1 || scale_all || !g.width || !g.height)
        return g.resolution.scale;
    return w * 2u <= g.width && h * 2u <= g.height ? 1.0f : g.resolution.scale;
}
/* A reduced target first drawn with a logic op is a glyph scratch, not a blur
 * level. libfont's vector path (Bleach's text) fills each glyph outline into a
 * 96x96 target by inverting alpha over a triangle fan, adds the curve
 * triangles, then composites the top-left WxH onto the screen at about half
 * size with bilinear filtering -- its only antialiasing. Kept at guest size,
 * the glyph is rasterized for 720p and magnified 1.5x at 4K: blurred
 * staircases. At the display scale the composite minifies 2:1 at every
 * resolution, as on the console. No sampling pass addresses these targets in
 * logical texels. RSX_LOGIC_TARGET_SCALE=0 keeps them at guest size. */
static int g_ld_logic_target;       /* set by current_surface around surface_get */
static int ld_flat_shading(void);
static int ld_logic_target_scaled(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_LOGIC_TARGET_SCALE"); on = !(e && e[0] == '0'); }
    return on && (rsx_dsp_reg(&g.rsx, 0x0374u /* M_LOGIC_OP_ENABLE, defined further down */) & 1u);
}
/* Glyph scratches are drawn at twice the display scale and reduced 2x2 by box
 * filter when the composite samples them (surface_view): every edge pixel on
 * screen averages four coverage samples on top of the game's own 2:1 bilinear
 * minification. RSX_GLYPH_SUPERSAMPLE=1 draws them at the display scale. */
static float ld_logic_target_scale(void)
{
    static int factor = 0;
    if (!factor) { const char* e = getenv("RSX_GLYPH_SUPERSAMPLE"); factor = e && e[0] == '1' ? 1 : 2; }
    return g.resolution.scale * (float)factor;
}
/* The scale of the target a draw renders into: the surface's own once it exists
 * (later passes into a glyph scratch have no logic op), else what surface_get
 * will create. The scratch is created by the clear before the first glyph, with
 * the logic op still off; surface_get rescales it at the first logic-op draw. */
static float ld_draw_target_scale(const rsx_dsp_surface* sf)
{
    const u32 w = sf->clip_w ? sf->clip_w : g.width, h = sf->clip_h ? sf->clip_h : g.height;
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].tex && g.surfaces[i].location == sf->color_location[0] &&
            g.surfaces[i].offset == sf->color_offset[0] && g.surfaces[i].w == w && g.surfaces[i].h == h)
            return ld_logic_target_scaled() ? ld_logic_target_scale() : g.surfaces[i].scale;
    return ld_logic_target_scaled() ? ld_logic_target_scale() : ld_target_scale(w, h);
}
static rsx_resolution_domain ld_surface_domain(const surface_t* s)
{
    const rsx_resolution_domain d = { s->scale };
    return d;
}
static u32 ld_surface_physical(const surface_t* s, u32 logical)
{ return rsx_resolution_extent(ld_surface_domain(s), logical); }
static LONG ld_surface_edge(const surface_t* s, LONG logical)
{ return rsx_resolution_edge(ld_surface_domain(s), logical); }

#define LD_VERTEX_HOIST_FETCH       (1u << 0)
#define LD_VERTEX_MASK_SIGNATURE    (1u << 1)
#define LD_VERTEX_COMPACT_PAYLOAD   (1u << 2)

static int ld_vertex_hoist_fetch(void)
{
    return (g.vertex_features & LD_VERTEX_HOIST_FETCH) != 0;
}

static int ld_vertex_mask_signature(void)
{
    return (g.vertex_features & LD_VERTEX_MASK_SIGNATURE) != 0;
}

static int ld_vertex_compact_payload(void)
{
    return (g.vertex_features & LD_VERTEX_COMPACT_PAYLOAD) != 0;
}

static u64 g_pull_draws, g_pull_declined, g_pull_vertices, g_pull_bytes;
static void ld_pull_report(void) {
    fprintf(stderr,"[native-vertex-pull] draws=%llu declined=%llu vertices=%llu bytes=%llu\n",
        (unsigned long long)g_pull_draws,(unsigned long long)g_pull_declined,
        (unsigned long long)g_pull_vertices,(unsigned long long)g_pull_bytes);
}
static int ld_vertex_pull(void) {
    static int enabled=-1;
    if(enabled<0){const char* e=getenv("RSX_NATIVE_VERTEX_PULL");
        enabled=e && !strcmp(e,"1") && !getenv("RSX_DRAW_CSV") && !getenv("RSX_A010_GEOM");
        if(enabled)atexit(ld_pull_report);}
    return enabled;
}

static const char* ld_vertex_mode_name(void)
{
    switch (g.vertex_mode) {
    case 'H': return "H-hoist";
    case 'M': return "M-mask";
    case 'C': return "C-compact";
    default:  return "L-legacy";
    }
}

static int ld_fp_constants_buffered(void)
{
    return g.fp_constant_mode == 'B';
}

static const char* ld_fp_constant_mode_name(void)
{
    return ld_fp_constants_buffered() ? "buffered" : "literal";
}

static int g_ld_dred_dumped = 0;
static u32 g_ld_frames = 0;
static ID3D12InfoQueue* g_ld_info_queue = NULL;
static int g_ld_debug_layer_enabled = 0;

typedef struct {
    int valid;
    u64 key;
    u64 vp_hash;
    u32 vp_start, vp_instrs;
    u32 fp_location, fp_offset, fp_size, fp_control;
    u32 cube_mask, vtex_mask, txl_mask;
    u32 input_mask, input_stride;
    u32 packed_offsets;
    ld_shader_resources resources;
} ld_pso_metadata;

static ld_pso_metadata g_ld_current_pso;

static int ld_shader_resources_enabled(void)
{
    static int enabled=-1;
    if (enabled<0) {
        const char* value=getenv("BLEACH_SHADER_RESOURCES");
        enabled=!value || strcmp(value,"0");
    }
    return enabled;
}
static u64 g_ld_flip_requested = 0;
static u64 g_ld_flip_consumed = 0;
static u32 g_ld_last_requested_buffer = UINT32_MAX;
static u32 g_ld_last_consumed_buffer = UINT32_MAX;
static u32 g_ld_last_present_target = UINT32_MAX;
static u64 g_ld_last_dump_fingerprint = 0;
#if !defined(YZ_PERF_CLEAN)
static u32 g_ld_surface_generation = 0;
static u32 g_ld_surface_resource_serial = 0;
static u32 g_ld_guest_blit_generation = 0;
static u32 g_ld_present_copy_generation = 0;
static u64 g_ld_vertex_constant_ring_recycles = 0;
static u32 g_ld_frame_vertex_constant_ring_recycles = 0;
static u32 g_ld_fifo_get = 0;
static u32 g_ld_fifo_put = 0;
static volatile LONG g_ld_diag_post_movie_pending = 0;
static volatile LONG g_ld_diag_post_movie_presents = 0;

enum {
    LD_SURFACE_WRITE_NONE = 0,
    LD_SURFACE_WRITE_CREATE = 1,
    LD_SURFACE_WRITE_DRAW = 2,
    LD_SURFACE_WRITE_CLEAR = 3,
    LD_SURFACE_WRITE_COPY = 4,
    LD_SURFACE_WRITE_BLIT = 5,
    LD_SURFACE_WRITE_RESOLVE = 6,
    LD_SURFACE_WRITE_OTHER = 7
};

#define M_COLOR_MASK         0x0324
static void ld_surface_note_write(u32 target, u32 kind)
{
    if (target >= MAX_SURFACES)
        return;
    surface_t* surface = &g.surfaces[target];
    const u32 generation = ++g_ld_surface_generation;
    surface->swin_wgen++;
    surface->last_write_generation = generation;
    if (kind != LD_SURFACE_WRITE_CREATE &&
        (kind != LD_SURFACE_WRITE_DRAW || rsx_dsp_reg(&g.rsx, M_COLOR_MASK)))
        surface->color_content_generation = generation;
    surface->last_write_kind = kind;
    switch (kind) {
    case LD_SURFACE_WRITE_CREATE:
        surface->last_create_generation = generation;
        break;
    case LD_SURFACE_WRITE_DRAW:
        surface->last_draw_generation = generation;
        break;
    case LD_SURFACE_WRITE_CLEAR:
        surface->last_clear_generation = generation;
        break;
    case LD_SURFACE_WRITE_COPY:
        surface->last_copy_generation = generation;
        break;
    case LD_SURFACE_WRITE_BLIT:
        surface->last_blit_generation = generation;
        break;
    case LD_SURFACE_WRITE_RESOLVE:
        surface->last_resolve_generation = generation;
        break;
    default:
        surface->last_other_generation = generation;
        break;
    }
}
#else
#define ld_surface_note_write(target, kind) \
    do { if ((target) < MAX_SURFACES) g.surfaces[(target)].swin_wgen++; } while (0)
#endif

#define LD_LAYOUT_CACHE_CAP 256u
typedef struct {
    int valid;
    rsx_vertex_layout_plan plan;
} ld_layout_cache_entry;

static ld_layout_cache_entry g_ld_layout_cache[LD_LAYOUT_CACHE_CAP];
static u32 g_ld_layout_cache_count = 0;

static void ld_layout_plan_get(u32 mask, rsx_vertex_layout_plan* out)
{
    mask &= 0xFFFFu;
    for (u32 i = 0; i < g_ld_layout_cache_count; i++) {
        if (g_ld_layout_cache[i].valid &&
            g_ld_layout_cache[i].plan.mask == mask) {
            *out = g_ld_layout_cache[i].plan;
            return;
        }
    }
    rsx_vertex_layout_plan_init(out, mask);
    if (g_ld_layout_cache_count < LD_LAYOUT_CACHE_CAP) {
        ld_layout_cache_entry* entry =
            &g_ld_layout_cache[g_ld_layout_cache_count++];
        entry->valid = 1;
        entry->plan = *out;
    }
}

#define LD_RECENT_DRAW_CAP 128u
typedef struct {
    u64 serial;
    u64 pso_key;
    u64 descriptor_signature;
    u32 frame;
    u32 vp_start, vp_instrs;
    u32 fp_location, fp_offset, fp_size, fp_control;
    u32 cube_mask, vtex_mask, txl_mask, texture_mask;
    u32 primitive, vertices, target, zslot;
    u32 clip_x, clip_y, clip_w, clip_h;
    u32 srv_ring_used, sampler_ring_used, cb_used, vb_used;
} ld_recent_draw;

static ld_recent_draw g_ld_recent_draws[LD_RECENT_DRAW_CAP];
static u64 g_ld_recent_draw_total = 0;

/*
 * Isolated pre-cleanup benchmark measurement. Record only successful
 * swap-chain presents: one QPC call and fixed-ring stores, with no allocation,
 * configuration lookup, sorting, or logging in the presentation path.
 */
#define LD_PRESENT_RING_CAP (1u << 20)   /* ~2 h at 150 FPS: whole play sessions */
typedef struct {
    u64 present_id;
    u64 new_draws;
    u32 guest_frame;
    LONGLONG qpc;
    u64 process_kernel_100ns;
    u64 process_user_100ns;
    u64 present_thread_kernel_100ns;
    u64 present_thread_user_100ns;
    u32 present_thread_id;
} ld_present_sample;
static ld_present_sample g_ld_present_ring[LD_PRESENT_RING_CAP];
static u64 g_ld_present_total = 0;
static LONGLONG g_ld_present_origin_qpc = 0;
static u64 g_ld_present_previous_draws = 0;
static LONGLONG g_ld_qpc_frequency = 0;
/* Title telemetry is separate from the renderer-owned diagnostic samples. */
static SRWLOCK g_ld_title_present_lock = SRWLOCK_INIT;
static LONGLONG g_ld_title_present_qpc[LD_PRESENT_RING_CAP];
static LONGLONG g_ld_title_first_present_qpc = 0;
static u64 g_ld_title_present_total = 0;
static int g_ld_present_dumped = 0;
static int g_ld_schedule_diag = 0;
#if defined(YZ_PPU_SAMPLE)
static HANDLE g_ld_present_thread_handle = NULL;
#endif

static void ld_present_measure_dump(void);
#if defined(YZ_PERF_PROFILE)
extern void spu_perf_window_begin(u32 guest_frame);
extern void spu_perf_window_dump(u32 guest_frame);
extern void spu_perf_frame_sample_record(u32 guest_frame);
extern void spu_perf_frame_sample_dump(u32 guest_start, u32 guest_end);
#endif
#if defined(YZ_PPU_SAMPLE)
extern void yz_ppu_perf_window_begin(u32 guest_frame);
extern void yz_ppu_perf_window_dump(u32 guest_frame);
#endif

static void ld_present_measure_init(void)
{
    LARGE_INTEGER frequency;
    memset(g_ld_present_ring, 0, sizeof(g_ld_present_ring));
    g_ld_present_total = 0;
    g_ld_present_origin_qpc = 0;
    AcquireSRWLockExclusive(&g_ld_title_present_lock);
    g_ld_title_present_total = 0;
    g_ld_title_first_present_qpc = 0;
    ReleaseSRWLockExclusive(&g_ld_title_present_lock);
    g_ld_present_previous_draws = g_ld_recent_draw_total;
    g_ld_present_dumped = 0;
    g_ld_schedule_diag = getenv("RSX_SCHEDULE_DIAG") != NULL;
    if (QueryPerformanceFrequency(&frequency))
        g_ld_qpc_frequency = frequency.QuadPart;
    else
        g_ld_qpc_frequency = 0;
}

static void ld_present_measure_record(u32 guest_frame)
{
#if defined(YZ_PPU_SAMPLE)
    if (!g_ld_present_thread_handle) {
        HANDLE duplicate = NULL;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                            GetCurrentProcess(), &duplicate, 0, FALSE,
                            DUPLICATE_SAME_ACCESS)) {
            HANDLE previous = (HANDLE)InterlockedCompareExchangePointer(
                (PVOID volatile*)&g_ld_present_thread_handle, duplicate, NULL);
            if (previous)
                CloseHandle(duplicate);
        }
    }
#endif
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now))
        return;
    AcquireSRWLockExclusive(&g_ld_title_present_lock);
    if (!g_ld_title_present_total)
        g_ld_title_first_present_qpc = now.QuadPart;
    g_ld_title_present_qpc[g_ld_title_present_total & (LD_PRESENT_RING_CAP - 1u)] = now.QuadPart;
    ++g_ld_title_present_total;
    ReleaseSRWLockExclusive(&g_ld_title_present_lock);
    const u64 present_id = g_ld_present_total + 1u;
    if (present_id == 1u) g_ld_present_origin_qpc = now.QuadPart;
    ld_present_sample* sample =
        &g_ld_present_ring[(present_id - 1u) & (LD_PRESENT_RING_CAP - 1u)];
    sample->present_id = present_id;
    sample->new_draws = g_ld_recent_draw_total - g_ld_present_previous_draws;
    g_ld_present_previous_draws = g_ld_recent_draw_total;
    sample->guest_frame = guest_frame;
    sample->qpc = now.QuadPart;
    if (g_ld_schedule_diag) {
        FILETIME creation = {0}, exit = {0}, kernel = {0}, user = {0};
        ULARGE_INTEGER value;
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exit,
                            &kernel, &user)) {
            value.LowPart = kernel.dwLowDateTime;
            value.HighPart = kernel.dwHighDateTime;
            sample->process_kernel_100ns = value.QuadPart;
            value.LowPart = user.dwLowDateTime;
            value.HighPart = user.dwHighDateTime;
            sample->process_user_100ns = value.QuadPart;
        }
        if (GetThreadTimes(GetCurrentThread(), &creation, &exit,
                           &kernel, &user)) {
            value.LowPart = kernel.dwLowDateTime;
            value.HighPart = kernel.dwHighDateTime;
            sample->present_thread_kernel_100ns = value.QuadPart;
            value.LowPart = user.dwLowDateTime;
            value.HighPart = user.dwHighDateTime;
            sample->present_thread_user_100ns = value.QuadPart;
        }
        sample->present_thread_id = GetCurrentThreadId();
    }
    g_ld_present_total = present_id;
#if defined(YZ_PPU_SAMPLE)
    {
        static int sample_present = -1;
        static u32 sample_start = 1578u;
        static u32 sample_end = 2178u;
        if (sample_present < 0) {
            sample_present =
                getenv("PPU_SAMPLE_RUN") != NULL &&
                getenv("PPU_SAMPLE_PRESENT") != NULL;
            const char* start_text = getenv("PPU_SAMPLE_START");
            const char* frames_text = getenv("PPU_SAMPLE_FRAMES");
            if (start_text) {
                const unsigned long parsed = strtoul(start_text, NULL, 0);
                if (parsed <= UINT32_MAX)
                    sample_start = (u32)parsed;
            }
            if (frames_text) {
                unsigned long parsed = strtoul(frames_text, NULL, 0);
                if (parsed < 4u) parsed = 4u;
                if (parsed > 600u) parsed = 600u;
                sample_end = sample_start + (u32)parsed;
            }
        }
        const int game_present = guest_frame == g_ld_frames + 1u;
#if defined(YZ_PERF_PROFILE)
        if (sample_present && game_present &&
            guest_frame >= sample_start && guest_frame <= sample_end)
            spu_perf_frame_sample_record(guest_frame);
#endif
        if (sample_present && game_present && guest_frame == sample_start)
            yz_ppu_perf_window_begin(guest_frame);
        else if (sample_present && game_present && guest_frame == sample_end) {
            yz_ppu_perf_window_dump(guest_frame);
#if defined(YZ_PERF_PROFILE)
            spu_perf_frame_sample_dump(sample_start, sample_end);
#endif
        }
    }
#endif
#if defined(YZ_PERF_PROFILE) && defined(YZ_PPU_SAMPLE)
    /* These callbacks belong to the frontend's optional instruction sampler;
     * renderer timing alone must not require frontend-specific SPU hooks. */
    if (guest_frame == 1578u)
        spu_perf_window_begin(guest_frame);
    else if (guest_frame == 1582u)
        spu_perf_window_dump(guest_frame);
#endif
    /*
     * The controlled A010 benchmark ends at guest frame 2293.  Flush the
     * already-recorded fixed ring exactly at that endpoint so an unrelated
     * host-window teardown cannot truncate the authoritative raw samples.
     * This is outside the measured timestamp operation and runs once.
     */
#if defined(YZ_PPU_SAMPLE)
    if (guest_frame == 2293u)
        ld_present_measure_dump();
#endif
}

static void ld_present_measure_dump(void)
{
    if (g_ld_present_dumped || g_ld_present_total == 0 ||
        g_ld_qpc_frequency <= 0)
        return;
    g_ld_present_dumped = 1;

    char path[1024];
    const char* requested_path = getenv("RSX_PRESENT_LOG");
    if (requested_path && *requested_path)
        snprintf(path, sizeof(path), "%s", requested_path);
    else
        snprintf(path, sizeof(path), "scratch\\present_qpc_%lu.csv",
                 (unsigned long)GetCurrentProcessId());
    FILE* f = fopen(path, "wb");
    if (!f)
        return;

    fprintf(f, "# qpc_frequency=%lld\n", g_ld_qpc_frequency);
    if (g_ld_schedule_diag) {
        fprintf(f, "present_id,guest_frame,qpc,process_kernel_100ns,"
                   "process_user_100ns,present_thread_kernel_100ns,"
                   "present_thread_user_100ns,present_thread_id,new_draws,first_present_qpc\n");
    } else {
        fprintf(f, "present_id,guest_frame,qpc,new_draws,first_present_qpc\n");
    }
    const u64 first =
        g_ld_present_total > LD_PRESENT_RING_CAP
            ? g_ld_present_total - LD_PRESENT_RING_CAP + 1u : 1u;
    for (u64 present_id = first; present_id <= g_ld_present_total;
         ++present_id) {
        const ld_present_sample* sample =
            &g_ld_present_ring[(present_id - 1u) &
                               (LD_PRESENT_RING_CAP - 1u)];
        if (sample->present_id == present_id) {
            if (g_ld_schedule_diag) {
                fprintf(f, "%llu,%u,%lld,%llu,%llu,%llu,%llu,%u,%llu,%lld\n",
                        (unsigned long long)sample->present_id,
                        sample->guest_frame, sample->qpc,
                        (unsigned long long)sample->process_kernel_100ns,
                        (unsigned long long)sample->process_user_100ns,
                        (unsigned long long)sample->present_thread_kernel_100ns,
                        (unsigned long long)sample->present_thread_user_100ns,
                        sample->present_thread_id, (unsigned long long)sample->new_draws,
                        g_ld_present_origin_qpc);
            } else {
                fprintf(f, "%llu,%u,%lld,%llu,%lld\n",
                        (unsigned long long)sample->present_id,
                        sample->guest_frame, sample->qpc, (unsigned long long)sample->new_draws,
                        g_ld_present_origin_qpc);
            }
        }
    }
    fclose(f);
    fprintf(stderr,
            "[present-qpc] preserved %llu successful presents at %lld Hz in %s\n",
            (unsigned long long)(g_ld_present_total - first + 1u),
            g_ld_qpc_frequency, path);
}

static u64 g_ld_texture_cache_full = 0;
static u64 g_ld_texture_use_serial = 0;

static const char* ld_d3d_severity_name(D3D12_MESSAGE_SEVERITY severity)
{
    switch (severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "corruption";
        case D3D12_MESSAGE_SEVERITY_ERROR: return "error";
        case D3D12_MESSAGE_SEVERITY_WARNING: return "warning";
        case D3D12_MESSAGE_SEVERITY_INFO: return "info";
        default: return "message";
    }
}

static void ld_drain_info_queue(const char* where)
{
    if (!g_ld_info_queue) return;
    const u64 count =
        g_ld_info_queue->lpVtbl->GetNumStoredMessages(g_ld_info_queue);
    for (u64 i = 0; i < count; i++) {
        SIZE_T bytes = 0;
        if (FAILED(g_ld_info_queue->lpVtbl->GetMessage(
                g_ld_info_queue, i, NULL, &bytes)) ||
            !bytes || bytes > 1024u * 1024u)
            continue;
        D3D12_MESSAGE* message = (D3D12_MESSAGE*)malloc(bytes);
        if (!message) break;
        if (SUCCEEDED(g_ld_info_queue->lpVtbl->GetMessage(
                g_ld_info_queue, i, message, &bytes))) {
            fprintf(stderr,
                    "[d3d-debug] where=%s frame=%u severity=%s id=%u %s\n",
                    where, g_ld_frames,
                    ld_d3d_severity_name(message->Severity),
                    (unsigned)message->ID,
                    message->pDescription
                        ? message->pDescription : "<no description>");
        }
        free(message);
    }
    if (count)
        g_ld_info_queue->lpVtbl->ClearStoredMessages(g_ld_info_queue);
}

static void ld_dump_recent_draws(void)
{
    const u64 first =
        g_ld_recent_draw_total > LD_RECENT_DRAW_CAP
            ? g_ld_recent_draw_total - LD_RECENT_DRAW_CAP + 1u : 1u;
    fprintf(stderr,
            "[draw-history] total=%llu retained=%llu..%llu\n",
            (unsigned long long)g_ld_recent_draw_total,
            (unsigned long long)first,
            (unsigned long long)g_ld_recent_draw_total);
    for (u64 serial = first; serial <= g_ld_recent_draw_total; serial++) {
        const ld_recent_draw* draw =
            &g_ld_recent_draws[(serial - 1u) & (LD_RECENT_DRAW_CAP - 1u)];
        if (draw->serial != serial) continue;
        fprintf(stderr,
                "[draw-history] serial=%llu frame=%u key=%016llX "
                "vp=%u+%u fp=%u:0x%X+%u ctrl=0x%X "
                "mask{tex=%04X cube=%04X vtex=%04X txl=%04X} "
                "prim=%u verts=%u target=%u z=%u "
                "clip=%u,%u %ux%u desc=%016llX "
                "ring{srv=%u smp=%u cb=%u vb=%u}\n",
                (unsigned long long)draw->serial, draw->frame,
                (unsigned long long)draw->pso_key,
                draw->vp_start, draw->vp_instrs,
                draw->fp_location, draw->fp_offset, draw->fp_size,
                draw->fp_control, draw->texture_mask, draw->cube_mask,
                draw->vtex_mask, draw->txl_mask, draw->primitive,
                draw->vertices, draw->target, draw->zslot,
                draw->clip_x, draw->clip_y, draw->clip_w, draw->clip_h,
                (unsigned long long)draw->descriptor_signature,
                draw->srv_ring_used, draw->sampler_ring_used,
                draw->cb_used, draw->vb_used);
    }
}

static const char* ld_dred_op_name(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op) {
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRTV";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDSV";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
        case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
        default: return "other";
    }
}

static void ld_enable_dred(void)
{
    ID3D12DeviceRemovedExtendedDataSettings* settings = NULL;
    const HRESULT hr = D3D12GetDebugInterface(
        &IID_ID3D12DeviceRemovedExtendedDataSettings, (void**)&settings);
    if (FAILED(hr) || !settings) {
        fprintf(stderr, "[dred] settings unavailable hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    settings->lpVtbl->SetAutoBreadcrumbsEnablement(
        settings, D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->lpVtbl->SetPageFaultEnablement(
        settings, D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->lpVtbl->Release(settings);
    fprintf(stderr, "[dred] auto-breadcrumbs and page-fault tracking enabled\n");
}

static void ld_enable_debug_layer(void)
{
    if (!getenv("RSX_D3D_DEBUG")) return;
    ID3D12Debug* debug = NULL;
    const HRESULT hr = D3D12GetDebugInterface(
        &IID_ID3D12Debug, (void**)&debug);
    if (FAILED(hr) || !debug) {
        fprintf(stderr,
                "[d3d-debug] validation layer unavailable hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    debug->lpVtbl->EnableDebugLayer(debug);
    debug->lpVtbl->Release(debug);
    g_ld_debug_layer_enabled = 1;
    fprintf(stderr, "[d3d-debug] validation layer enabled\n");
}

static void ld_open_info_queue(void)
{
    if (!g_ld_debug_layer_enabled || !g.dev) return;
    const HRESULT hr = g.dev->lpVtbl->QueryInterface(
        g.dev, &IID_ID3D12InfoQueue, (void**)&g_ld_info_queue);
    if (FAILED(hr) || !g_ld_info_queue) {
        fprintf(stderr,
                "[d3d-debug] info queue unavailable hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }
    D3D12_MESSAGE_SEVERITY severities[] = {
        D3D12_MESSAGE_SEVERITY_CORRUPTION,
        D3D12_MESSAGE_SEVERITY_ERROR,
        D3D12_MESSAGE_SEVERITY_WARNING
    };
    D3D12_INFO_QUEUE_FILTER filter = {0};
    filter.AllowList.NumSeverities =
        (UINT)(sizeof(severities) / sizeof(severities[0]));
    filter.AllowList.pSeverityList = severities;
    g_ld_info_queue->lpVtbl->AddStorageFilterEntries(
        g_ld_info_queue, &filter);
    g_ld_info_queue->lpVtbl->SetMessageCountLimit(
        g_ld_info_queue, 65536u);
    fprintf(stderr,
            "[d3d-debug] info queue attached (warning/error/corruption)\n");
}

static void ld_dump_dred(const char* where, HRESULT trigger_hr)
{
    if (g_ld_dred_dumped++ || !g.dev) return;
    ld_drain_info_queue(where);
    ld_dump_recent_draws();
    const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
    fprintf(stderr,
            "[dred] trigger=%s hr=0x%08lX removed=0x%08lX frame=%u "
            "surfaces=%u zetas=%u textures=%u psos=%u\n",
            where, (unsigned long)trigger_hr, (unsigned long)removed,
            g_ld_frames, g.n_surfaces, g.n_zdepths, g.n_textures, g.n_psos);

    ID3D12DeviceRemovedExtendedData1* dred = NULL;
    HRESULT hr = g.dev->lpVtbl->QueryInterface(
        g.dev, &IID_ID3D12DeviceRemovedExtendedData1, (void**)&dred);
    if (FAILED(hr) || !dred) {
        fprintf(stderr, "[dred] query failed hr=0x%08lX\n",
                (unsigned long)hr);
        return;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {0};
    hr = dred->lpVtbl->GetAutoBreadcrumbsOutput1(dred, &breadcrumbs);
    fprintf(stderr, "[dred] breadcrumbs hr=0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        const D3D12_AUTO_BREADCRUMB_NODE1* node =
            breadcrumbs.pHeadAutoBreadcrumbNode;
        for (u32 node_index = 0; node && node_index < 16;
             node = node->pNext, node_index++) {
            const u32 last = node->pLastBreadcrumbValue
                ? *node->pLastBreadcrumbValue : 0;
            fprintf(stderr,
                    "[dred] node=%u list=%s queue=%s count=%u last=%u\n",
                    node_index,
                    node->pCommandListDebugNameA
                        ? node->pCommandListDebugNameA : "<unnamed>",
                    node->pCommandQueueDebugNameA
                        ? node->pCommandQueueDebugNameA : "<unnamed>",
                    node->BreadcrumbCount, last);
            if (!node->pCommandHistory || !node->BreadcrumbCount) continue;
            const u32 first = last > 8 ? last - 8 : 0;
            u32 end = last + 8;
            if (end >= node->BreadcrumbCount)
                end = node->BreadcrumbCount - 1;
            for (u32 i = first; i <= end; i++) {
                const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
                fprintf(stderr,
                        "[dred]   op[%u]%s=%u(%s)\n",
                        i, i == last ? "*" : "",
                        (unsigned)op, ld_dred_op_name(op));
            }
        }
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {0};
    hr = dred->lpVtbl->GetPageFaultAllocationOutput1(dred, &fault);
    fprintf(stderr,
            "[dred] page-fault hr=0x%08lX va=0x%016llX\n",
            (unsigned long)hr,
            (unsigned long long)fault.PageFaultVA);
    if (SUCCEEDED(hr)) {
        const D3D12_DRED_ALLOCATION_NODE1* lists[2] = {
            fault.pHeadExistingAllocationNode,
            fault.pHeadRecentFreedAllocationNode
        };
        const char* labels[2] = {"existing", "recent-freed"};
        for (u32 list = 0; list < 2; list++) {
            const D3D12_DRED_ALLOCATION_NODE1* allocation = lists[list];
            for (u32 i = 0; allocation && i < 16;
                 allocation = allocation->pNext, i++) {
                fprintf(stderr,
                        "[dred] %s[%u] type=%u name=%s object=%p\n",
                        labels[list], i, (unsigned)allocation->AllocationType,
                        allocation->ObjectNameA
                            ? allocation->ObjectNameA : "<unnamed>",
                        (const void*)allocation->pObject);
            }
        }
    }
    dred->lpVtbl->Release(dred);
    fflush(stderr);
}
static u64 g_ld_texture_cache_evictions = 0;
static u64 g_ld_texture_decode_fail = 0;
static u64 g_ld_zdepth_srv_binds = 0;
static u64 g_ld_zdepth_srv_reject_no_write = 0;
static u64 g_ld_vtex_binds = 0;
static u64 g_ld_vtex_uploads = 0;
static u64 g_ld_vtex_refreshes = 0;
static u64 g_ld_vtex_unsupported = 0;
static u64 g_ld_vtex_enabled = 0;
static u64 g_ld_vtex_missing_for_txl = 0;
static u64 g_ld_divider_fetches = 0;
static char g_ld_shader_disk_dir[MAX_PATH];
static int g_ld_shader_disk_ready = -1;
static u64 g_ld_shader_disk_hits[2] = {0, 0};
static u64 g_ld_shader_disk_misses[2] = {0, 0};
static u64 g_ld_shader_disk_writes[2] = {0, 0};
static u64 g_ld_shader_disk_rejects = 0;

/*
 * Profile-lane-only renderer accounting.  These counters deliberately live
 * outside ld_state so the clean benchmark lane keeps the exact same state
 * layout and hot path.  Emit only slow/interesting frames plus a sparse
 * heartbeat; the orphanage window is therefore visible without turning the
 * log itself into a benchmark cost.
 */
typedef enum {
    LD_FLUSH_PRESENT = 0,
    LD_FLUSH_GUEST_REFERENCE,
    LD_FLUSH_VERTEX_RING,
    LD_FLUSH_VERTEX_CONSTANT_RING,
    LD_FLUSH_RETIRE_QUEUE,
    LD_FLUSH_MOVIE,
    LD_FLUSH_MOVIE_PRESENT,
    LD_FLUSH_READBACK,
    LD_FLUSH_PIXEL_CONSTANT_RING,
    LD_FLUSH_DESCRIPTOR_RING,
    LD_FLUSH_SHUTDOWN,
    LD_FLUSH_REASON_COUNT
} ld_flush_reason;

#if defined(YZ_PERF_PROFILE)
typedef enum {
    LD_REJECT_WORLD = 0,
    LD_REJECT_CHARACTER,
    LD_REJECT_UI,
    LD_REJECT_OTHER,
    LD_REJECT_CLASS_COUNT
} ld_reject_class;

typedef struct {
    u64 pso_lookups;
    u64 pso_hits;
    u64 pso_misses;
    u64 pso_probes;
    u64 pso_full;
    u64 vs_blob_lookups;
    u64 vs_blob_hits;
    u64 vs_blob_misses;
    u64 vs_blob_inserts;
    u64 vs_blob_full_rejects;
    u64 ps_blob_lookups;
    u64 ps_blob_hits;
    u64 ps_blob_misses;
    u64 ps_blob_inserts;
    u64 ps_blob_full_rejects;
    u64 vs_compile_calls;
    u64 ps_compile_calls;
    u64 vs_unique;
    u64 ps_unique;
    u64 vs_post_boundary_distinct;
    u64 vs_post_boundary_repeats;
    u64 ps_post_boundary_distinct;
    u64 ps_post_boundary_repeats;
    u64 create_pso_calls;
    u64 decompile_qpc;
    u64 vs_blob_lookup_qpc;
    u64 ps_blob_lookup_qpc;
    u64 vs_compile_qpc;
    u64 ps_compile_qpc;
    u64 create_pso_qpc;
    u64 flush_qpc;
    u64 fence_wait_qpc;
    u64 gpu_ticks;
    u64 flush_reason[LD_FLUSH_REASON_COUNT];
    u64 flush_reason_qpc[LD_FLUSH_REASON_COUNT];
    u64 fence_reason_qpc[LD_FLUSH_REASON_COUNT];
    u64 input_vertices;
    u64 expanded_vertices;
    u64 used_attribute_draws;
    u64 used_attribute_sum;
    u64 legacy_vertex_upload_bytes;
    u64 vertex_upload_bytes;
    u64 vertex_fetch_pack_qpc;
    u64 sink_end_qpc;
    u64 pso_total_qpc;
    u64 texture_hash_qpc;
    u64 texture_hash_bytes;
    u64 texture_upload_bytes;
    u64 texture_decode_calls;
    u64 texture_decode_qpc;
    u64 texture_resolve_qpc;
    u64 command_encode_qpc;
    u64 texture_dependencies;
    u64 texture_unused_bindings;
    u64 ps_constant_allocations;
    u64 ps_constant_upload_bytes;
    u64 ps_constant_upload_qpc;
    u64 ps_constant_capacity_failures;
} ld_profile_counts;

typedef struct {
    ld_profile_counts total;
    ld_profile_counts previous;
    u64 vs_hashes[MAX_SHADER_BLOBS];
    u64 ps_hashes[MAX_SHADER_BLOBS];
    u64 ps_raw_exact_hashes[LD_VARIANT_SET_CAPACITY];
    u64 ps_raw_constant_hashes[LD_VARIANT_SET_CAPACITY];
    u64 ps_exact_source_hashes[LD_VARIANT_SET_CAPACITY];
    u64 ps_constant_specialized_hashes[LD_VARIANT_SET_CAPACITY];
    u64 ps_canonical_hashes[LD_VARIANT_SET_CAPACITY];
    u32 n_ps_raw_exact_hashes;
    u32 n_ps_raw_constant_hashes;
    u32 n_vs_hashes;
    u32 n_ps_hashes;
    u32 n_ps_exact_source_hashes;
    u32 n_ps_constant_specialized_hashes;
    u32 n_ps_canonical_hashes;
    u64 ps_variant_tracking_overflow;
    u32 frame_upload_high;
    u32 frame_vb_high;
    u32 frame_retired_high;
    u32 total_upload_high;
    u32 total_vb_high;
    u32 total_retired_high;
    u32 frame_used_attribute_max;
    u32 total_used_attribute_max;
    u64 measured_frames;
    u64 compile_free_frames;
    double total_frame_ms;
    double compile_free_frame_ms;
    u64 previous_packets;
    u64 previous_groups;
    u64 previous_executed;
    u64 previous_evictions;
    u64 previous_refreshes;
    LONGLONG previous_present_qpc;
    LONGLONG qpc_frequency;
    IDXGIAdapter3* adapter;
    u64 rejected_pso_keys[MAX_REJECTED_PSO_KEYS];
    u8 rejected_pso_class_masks[MAX_REJECTED_PSO_KEYS];
    u32 n_rejected_pso_keys;
    u64 rejected_pso_requests[LD_REJECT_CLASS_COUNT];
    u32 rejected_pso_unique[LD_REJECT_CLASS_COUNT];
    u32 first_pso_full_frame;
} ld_profile_state;

static ld_profile_state g_ld_profile;

static LONGLONG ld_profile_qpc(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

static void ld_profile_note_ring_highwater(void)
{
    if (g.frame->upload_used > g_ld_profile.frame_upload_high)
        g_ld_profile.frame_upload_high = g.frame->upload_used;
    if (g.frame->upload_used > g_ld_profile.total_upload_high)
        g_ld_profile.total_upload_high = g.frame->upload_used;
    if (g.frame->vb_used > g_ld_profile.frame_vb_high)
        g_ld_profile.frame_vb_high = g.frame->vb_used;
    if (g.frame->vb_used > g_ld_profile.total_vb_high)
        g_ld_profile.total_vb_high = g.frame->vb_used;
    if (g.frame->n_retired_textures > g_ld_profile.frame_retired_high)
        g_ld_profile.frame_retired_high = g.frame->n_retired_textures;
    if (g.frame->n_retired_textures > g_ld_profile.total_retired_high)
        g_ld_profile.total_retired_high = g.frame->n_retired_textures;
}
#else
static void ld_profile_note_ring_highwater(void) {}
#endif

static volatile LONG g_ld_a010_probe_active = 0;
static u32 g_ld_a010_probe_start_frame = 0;
static u32 g_ld_a010_probe_sample = 0;
static u64 g_ld_a010_probe_touched = 0;
static volatile LONG g_ld_a010_camera_ready = 0;
static volatile LONG g_ld_a010_world_ready = 0;
static u32 g_ld_a010_camera_bits[16];

static void ld_movement_camera_snapshot(LONG phase)
{
    fprintf(stderr,
            "[movement-frontier-rsx] phase=%ld frame=%u root=%ld "
            "reference_active=%ld camera_ready=%ld\n",
            phase, g_ld_frames,
            InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0),
            InterlockedCompareExchange(
                &g_yz_a010_reference_camera_active, 0, 0),
            InterlockedCompareExchange(&g_ld_a010_camera_ready, 0, 0));
    for (u32 row = 0; row < 4u; ++row) {
        fprintf(stderr,
                "[movement-frontier-rsx-camera] phase=%ld row=%u "
                "constant=%08X/%08X/%08X/%08X "
                "fallback=%08X/%08X/%08X/%08X\n",
                phase, row,
                g.rsx.constants[108u + row][0],
                g.rsx.constants[108u + row][1],
                g.rsx.constants[108u + row][2],
                g.rsx.constants[108u + row][3],
                g_ld_a010_camera_bits[row * 4u + 0u],
                g_ld_a010_camera_bits[row * 4u + 1u],
                g_ld_a010_camera_bits[row * 4u + 2u],
                g_ld_a010_camera_bits[row * 4u + 3u]);
    }
    fflush(stderr);
}
/* Host movie presentation and the FIFO consumer live on different threads but
 * share one D3D12 command list. Normal gameplay remains single-producer and
 * bypasses this lock; the active-reader handshake closes the movie-mode
 * transition race without putting every ordinary RSX method behind an SRW
 * lock. Host-frame priority prevents a flood of guest caption commands from
 * starving Present when the window is backgrounded. */
static SRWLOCK g_ld_access_lock = SRWLOCK_INIT;
static volatile LONG g_ld_guest_active = 0;
static volatile LONG g_ld_host_waiting = 0;

static const u8* guest_ptr(u32 location, u32 offset, u32 min_bytes)
{
    if (!g.guest_ptr) return NULL;
    return g.guest_ptr(g.guest_user, location, offset, min_bytes);
}
static u64 fnv1a(const void* data, u32 n, u64 h);
static u64 ld_dump_surface_ppm(const char* path, const surface_t* surface);
static u64 live_legacy_pso_key(void);
static void ld_vertex_diag_emit(const char* reason, int dump_surface);

/* ---------------------------------------------------------------------------
 * enable gate
 * -----------------------------------------------------------------------*/
int rsx_live_draw_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        /* Opt-in in this tree. Upstream (caner's Yakuza fork) defaults this ON
         * because that runner has no other renderer; here rsx_d3d12_backend.c
         * is still the default for every other title, so the live engine has
         * to be asked for. (The YZ_RSX_DRAW alias his scripts used went with
         * the title-hack drain; RSX_LIVE_DRAW is the one spelling.) */
        const char* e = getenv("RSX_LIVE_DRAW");
        cached = (e && e[0] != '0') ? 1 : 0;
    }
    return cached;
}

/* ---------------------------------------------------------------------------
 * B1 render/sampler state decode (identical facts to replay_main.c)
 * -----------------------------------------------------------------------*/
#define M_BLEND_ENABLE       0x0310
#define M_ALPHA_TEST_ENABLE  0x0304
#define M_ALPHA_FUNC         0x0308
#define M_ALPHA_REF          0x030C
#define M_BLEND_SFACTOR      0x0314
#define M_BLEND_DFACTOR      0x0318
#define M_BLEND_EQUATION     0x0320
#define M_DEPTH_FUNC         0x0A6C
#define M_DEPTH_WRITE        0x0A70
#define M_DEPTH_TEST_ENABLE  0x0A74
#define M_CULL_FACE          0x1830
#define M_FRONT_FACE         0x1834
#define M_CULL_FACE_ENABLE   0x183C
#define M_LOGIC_OP_ENABLE    0x0374
#define M_LOGIC_OP           0x0378
/* Stencil + scissor (raw NV4097 offsets, same convention as above; RPCS3
 * gcm_enums.h consulted as a read-only numbering oracle). */
#define M_STENCIL_TEST_ENABLE 0x0328
#define M_STENCIL_MASK        0x032C
#define M_STENCIL_FUNC        0x0330
#define M_STENCIL_FUNC_REF    0x0334
#define M_STENCIL_FUNC_MASK   0x0338
#define M_STENCIL_OP_FAIL     0x033C
#define M_STENCIL_OP_ZFAIL    0x0340
#define M_STENCIL_OP_ZPASS    0x0344
#define M_TWO_SIDED_STENCIL   0x0348
#define M_BACK_STENCIL_FUNC   0x0350
#define M_BACK_STENCIL_OP_FAIL  0x035C
#define M_BACK_STENCIL_OP_ZFAIL 0x0360
#define M_BACK_STENCIL_OP_ZPASS 0x0364
#define M_SCISSOR_HORIZONTAL  0x08C0
#define M_SCISSOR_VERTICAL    0x08C4
#define M_ZSTENCIL_CLEAR      0x1D8C
#define M_ZMIN_MAX_CONTROL    0x1D78

static D3D12_COMPARISON_FUNC gcm_cmp(u32 f)
{
    switch (f) {
    case 0x0200: return D3D12_COMPARISON_FUNC_NEVER;
    case 0x0201: return D3D12_COMPARISON_FUNC_LESS;
    case 0x0202: return D3D12_COMPARISON_FUNC_EQUAL;
    case 0x0203: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 0x0204: return D3D12_COMPARISON_FUNC_GREATER;
    case 0x0205: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 0x0206: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    default:     return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}
static D3D12_BLEND gcm_blend_factor(u32 f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}
static D3D12_BLEND_OP gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:     return D3D12_BLEND_OP_ADD;
    }
}

/* NV4097 uses the GL 0x1500..0x150F truth-table operation numbers. */
static D3D12_LOGIC_OP gcm_logic_op(u32 op)
{
    static const D3D12_LOGIC_OP ops[16] = {
        D3D12_LOGIC_OP_CLEAR, D3D12_LOGIC_OP_AND,
        D3D12_LOGIC_OP_AND_REVERSE, D3D12_LOGIC_OP_COPY,
        D3D12_LOGIC_OP_AND_INVERTED, D3D12_LOGIC_OP_NOOP,
        D3D12_LOGIC_OP_XOR, D3D12_LOGIC_OP_OR,
        D3D12_LOGIC_OP_NOR, D3D12_LOGIC_OP_EQUIV,
        D3D12_LOGIC_OP_INVERT, D3D12_LOGIC_OP_OR_REVERSE,
        D3D12_LOGIC_OP_COPY_INVERTED, D3D12_LOGIC_OP_OR_INVERTED,
        D3D12_LOGIC_OP_NAND, D3D12_LOGIC_OP_SET
    };
    return op >= 0x1500 && op <= 0x150F ? ops[op - 0x1500] : D3D12_LOGIC_OP_COPY;
}

/* SURFACE_COLOR_TARGET selector -> number of colour targets written, A first.
 * 0x02 (B alone) and anything unknown draw to A only, as before. */
static u32 ld_rt_count(u32 sel)
{
    switch (sel) {
    case 0x13: return 2;
    case 0x17: return 3;
    case 0x1F: return 4;
    default:   return 1;
    }
}

typedef struct {
    u32 alpha_test_enable, alpha_func, alpha_ref_raw, alpha_ref_format;
    u32 blend_enable, sf_rgb, df_rgb, sf_a, df_a, eq_rgb, eq_a;
    u32 depth_test, depth_write, depth_func, depth_clip;
    u32 cull_enable, cull_face, front_face;
    u32 color_mask;
    u32 logic_enable, logic_op;
    /* LINE_SMOOTH_ENABLE on a line draw: coverage-weighted alpha along the line
     * (libfont feathers each glyph's edge with a smooth outline strip). */
    u32 line_smooth;
    /* TM: this title renders HDR into FP16 (SURFACE_FORMAT 0xB) targets;
     * the RTV format is PSO state, so it belongs in the key. */
    u32 rt_fp16;
    /* Colour targets the draw writes (SURFACE_COLOR_TARGET: A, AB, ABC,
     * ABCD). GH3's venue pass writes A and B; the post pass then samples B.
     * With only A bound, B stayed empty and that pass painted the scene
     * black. NumRenderTargets is PSO state, so it is keyed too. */
    u32 rt_count;
    /* Stencil is PSO state in D3D12, so it lives here and feeds the PSO key
     * like every other field. The reference value is dynamic
     * (OMSetStencilRef) and deliberately NOT part of this struct. */
    u32 stencil_enable, stencil_two_sided;
    u32 s_func, s_func_mask, s_write_mask, s_fail, s_zfail, s_zpass;
    u32 bs_func, bs_fail, bs_zfail, bs_zpass;
} render_state_t;

static void decode_render_state(render_state_t* rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->alpha_test_enable = rsx_dsp_reg(&g.rsx, M_ALPHA_TEST_ENABLE) & 1;
    rs->alpha_func = rsx_dsp_reg(&g.rsx, M_ALPHA_FUNC);
    rs->alpha_ref_raw = rsx_dsp_reg(&g.rsx, M_ALPHA_REF);
    rsx_dsp_surface alpha_surface;
    rsx_dsp_get_surface(&g.rsx, &alpha_surface);
    rs->alpha_ref_format = alpha_surface.color_format;
    rs->rt_fp16 = alpha_surface.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16;
    rs->rt_count = ld_rt_count(alpha_surface.color_target);
    rs->blend_enable = rsx_dsp_reg(&g.rsx, M_BLEND_ENABLE) & 1;
    const u32 sf = rsx_dsp_reg(&g.rsx, M_BLEND_SFACTOR);
    const u32 df = rsx_dsp_reg(&g.rsx, M_BLEND_DFACTOR);
    const u32 eq = rsx_dsp_reg(&g.rsx, M_BLEND_EQUATION);
    rs->sf_rgb = sf & 0xFFFF; rs->sf_a = sf >> 16;
    rs->df_rgb = df & 0xFFFF; rs->df_a = df >> 16;
    rs->eq_rgb = eq & 0xFFFF; rs->eq_a = eq >> 16;
    rs->depth_test  = rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1;
    /* LD_NO_DEPTH=1: disable the depth test on every draw. A probe for draws
     * that vanish -- Tornado Outbreak's UI text fails its depth test here (not
     * yet understood), and this is how its menus are readable meanwhile. */
    { static int nd = -1; if (nd < 0) nd = getenv("LD_NO_DEPTH") ? 1 : 0; if (nd) rs->depth_test = 0; }
    rs->depth_write = rsx_dsp_reg(&g.rsx, M_DEPTH_WRITE) & 1;
    rs->depth_func  = rsx_dsp_reg(&g.rsx, M_DEPTH_FUNC);
    /* ZMIN_MAX_CONTROL: clip in low nibble, clamp in the next nibble.
     * Explicit clamp takes precedence. D3D12's zero-initialized rasterizer
     * disables clipping, so leaving this unmapped clamps every guest pass. */
    const u32 zcontrol = rsx_dsp_reg(&g.rsx, M_ZMIN_MAX_CONTROL);
    rs->depth_clip = (zcontrol & 0xFu) && !(zcontrol & 0xF0u);
    rs->cull_enable = rsx_dsp_reg(&g.rsx, M_CULL_FACE_ENABLE) & 1;
    rs->cull_face   = rsx_dsp_reg(&g.rsx, M_CULL_FACE);
    rs->front_face  = rsx_dsp_reg(&g.rsx, M_FRONT_FACE);
    /* s31 (scratch/s31_blue_emitter.md): honor the RAW register â€” 0 is a
     * legitimate game-written "write no color channels" (the character
     * shadow-mask depth-prime pass). rsx_dispatch_init seeds the nv40
     * reset default (0x01010101), so never-written reads as all-on. */
    rs->color_mask  = rsx_dsp_reg(&g.rsx, M_COLOR_MASK);
    rs->logic_enable = rsx_dsp_reg(&g.rsx, M_LOGIC_OP_ENABLE) & 1;
    rs->logic_op = rs->logic_enable ? rsx_dsp_reg(&g.rsx, M_LOGIC_OP) : 0;
    { static int smooth = -1;
      if (smooth < 0) { const char* e = getenv("RSX_LINE_SMOOTH"); smooth = !(e && e[0] == '0'); }
      rs->line_smooth = smooth && g.rsx.current_primitive == RSX_PRIMITIVE_LINE_STRIP &&
          (rsx_dsp_reg(&g.rsx, 0x03BCu /* SET_LINE_SMOOTH_ENABLE */) & 1u); }
    rs->stencil_enable    = rsx_dsp_reg(&g.rsx, M_STENCIL_TEST_ENABLE) & 1;
    rs->stencil_two_sided = rsx_dsp_reg(&g.rsx, M_TWO_SIDED_STENCIL) & 1;
    rs->s_func       = rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC);
    rs->s_func_mask  = rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_MASK) & 0xFF;
    rs->s_write_mask = rsx_dsp_reg(&g.rsx, M_STENCIL_MASK) & 0xFF;
    rs->s_fail       = rsx_dsp_reg(&g.rsx, M_STENCIL_OP_FAIL);
    rs->s_zfail      = rsx_dsp_reg(&g.rsx, M_STENCIL_OP_ZFAIL);
    rs->s_zpass      = rsx_dsp_reg(&g.rsx, M_STENCIL_OP_ZPASS);
    rs->bs_func      = rsx_dsp_reg(&g.rsx, M_BACK_STENCIL_FUNC);
    rs->bs_fail      = rsx_dsp_reg(&g.rsx, M_BACK_STENCIL_OP_FAIL);
    rs->bs_zfail     = rsx_dsp_reg(&g.rsx, M_BACK_STENCIL_OP_ZFAIL);
    rs->bs_zpass     = rsx_dsp_reg(&g.rsx, M_BACK_STENCIL_OP_ZPASS);
}

/* GL stencil-op enums (the NV4097 payload) -> D3D12. Zero (never seeded by
 * the game before first use) and GL_KEEP both map to KEEP. */
static D3D12_STENCIL_OP gcm_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return D3D12_STENCIL_OP_ZERO;      /* GL_ZERO */
    case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;  /* GL_INCR (clamped) */
    case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;  /* GL_DECR (clamped) */
    case 0x150A: return D3D12_STENCIL_OP_INVERT;
    case 0x8507: return D3D12_STENCIL_OP_INCR;      /* GL_INCR_WRAP */
    case 0x8508: return D3D12_STENCIL_OP_DECR;      /* GL_DECR_WRAP */
    case 0x1E00: default: return D3D12_STENCIL_OP_KEEP;
    }
}

static void apply_render_state(D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd,
                               const render_state_t* rs)
{
    D3D12_RENDER_TARGET_BLEND_DESC* b = &pd->BlendState.RenderTarget[0];
    /* nv40_3d COLOR_MASK byte layout (Mesa/nouveau nv40_3d.xml.h, MIT/X11):
     * B=[0:7] G=[8:15] R=[16:23] A=[24:31]; any nonzero byte = channel on.
     * (s31: was hardcoded ENABLE_ALL â€” the blue-character class' live twin.) */
    b->RenderTargetWriteMask =
        (((rs->color_mask       ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_BLUE  : 0) |
        (((rs->color_mask >>  8 ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
        (((rs->color_mask >> 16 ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_RED   : 0) |
        (((rs->color_mask >> 24 ) & 0xFF) ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0);
    /* Logic operations take precedence over blending on RSX. In particular,
     * font outlines rasterized as triangle fans invert the alpha channel
     * to form an even-odd coverage mask; the leftover blend equation must not
     * replace that operation. D3D12 also requires the enables to be exclusive. */
    if (rs->logic_enable) {
        b->BlendEnable = FALSE;
        b->LogicOpEnable = TRUE;
        b->LogicOp = gcm_logic_op(rs->logic_op);
        { static unsigned reported;
          if (getenv("LD_LOGIC_DIAG") && reported++ < 8)
              fprintf(stderr, "[logic-op] enable=1 op=0x%X cmask=0x%08X blend=%u eq=0x%X/0x%X\n",
                      rs->logic_op, rs->color_mask, rs->blend_enable, rs->eq_rgb, rs->eq_a); }
    } else if (rs->blend_enable) {
        b->BlendEnable   = TRUE;
        b->SrcBlend      = gcm_blend_factor(rs->sf_rgb, 0);
        b->DestBlend     = gcm_blend_factor(rs->df_rgb, 0);
        b->BlendOp       = gcm_blend_op(rs->eq_rgb);
        b->SrcBlendAlpha = gcm_blend_factor(rs->sf_a, 1);
        b->DestBlendAlpha= gcm_blend_factor(rs->df_a, 1);
        b->BlendOpAlpha  = gcm_blend_op(rs->eq_a);
    }
    pd->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd->RasterizerState.DepthClipEnable = rs->depth_clip ? TRUE : FALSE;
    if (rs->cull_enable && rs->cull_face) {
        const u32 f = rs->cull_face;
        pd->RasterizerState.CullMode = (f == 0x0404) ? D3D12_CULL_MODE_FRONT
                                     : (f == 0x0405) ? D3D12_CULL_MODE_BACK
                                                     : D3D12_CULL_MODE_NONE;
    } else {
        pd->RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    }
    /* Y-negating viewport epilogue mirrors winding; take the front sense
     * straight from the guest value (validated in B1). */
    pd->RasterizerState.FrontCounterClockwise = (rs->front_face == 0x0901);
    /* D3D12's alpha-antialiased lines (single-sampled targets only) multiply
     * the shader's alpha by the line's coverage, as RSX line smoothing does. */
    pd->RasterizerState.AntialiasedLineEnable = rs->line_smooth ? TRUE : FALSE;
    if (g.depth) {
        pd->DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        pd->DepthStencilState.DepthEnable = rs->depth_test ? TRUE : FALSE;
        pd->DepthStencilState.DepthWriteMask =
            rs->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pd->DepthStencilState.DepthFunc = gcm_cmp(rs->depth_func);
        if (rs->stencil_enable) {
            pd->DepthStencilState.StencilEnable    = TRUE;
            pd->DepthStencilState.StencilReadMask  = (UINT8)rs->s_func_mask;
            pd->DepthStencilState.StencilWriteMask = (UINT8)rs->s_write_mask;
            pd->DepthStencilState.FrontFace.StencilFunc = gcm_cmp(rs->s_func);
            pd->DepthStencilState.FrontFace.StencilFailOp = gcm_stencil_op(rs->s_fail);
            pd->DepthStencilState.FrontFace.StencilDepthFailOp = gcm_stencil_op(rs->s_zfail);
            pd->DepthStencilState.FrontFace.StencilPassOp = gcm_stencil_op(rs->s_zpass);
            /* Two-sided off: nv40 applies the front state to both faces. */
            if (rs->stencil_two_sided) {
                pd->DepthStencilState.BackFace.StencilFunc = gcm_cmp(rs->bs_func);
                pd->DepthStencilState.BackFace.StencilFailOp = gcm_stencil_op(rs->bs_fail);
                pd->DepthStencilState.BackFace.StencilDepthFailOp = gcm_stencil_op(rs->bs_zfail);
                pd->DepthStencilState.BackFace.StencilPassOp = gcm_stencil_op(rs->bs_zpass);
            } else {
                pd->DepthStencilState.BackFace = pd->DepthStencilState.FrontFace;
            }
        } else {
            pd->DepthStencilState.StencilEnable = FALSE;
        }
    }
}

#include "rsx_live_sampler.h"

/* ---------------------------------------------------------------------------
 * descriptor heap helpers
 * -----------------------------------------------------------------------*/
static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(u32 idx)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.rtv_heap, &h);
    h.ptr += (size_t)idx * g.rtv_step;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.srv_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.srv_cpu_heap, &h);
    h.ptr += (size_t)slot * g.srv_step;
    return h;
}
static void srv_write(u32 slot, ID3D12Resource* tex)
{
    D3D12_RESOURCE_DESC rd;
    tex->lpVtbl->GetDesc(tex, &rd);
    if (rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = rd.MipLevels;
        g.dev->lpVtbl->CreateShaderResourceView(g.dev, tex, &sd, srv_cpu(slot));
        return;
    }
    g.dev->lpVtbl->CreateShaderResourceView(g.dev, tex, NULL, srv_cpu(slot));
}
static void srv_write_zdepth(u32 slot, ID3D12Resource* tex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    /* The replay-proven depth snapshot broadcasts depth to RGB. Mirror that
     * mapping directly in the native SRV and force alpha to one. */
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(slot));
}
static void srv_write_depth_source(u32 slot, ID3D12Resource* tex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(slot));
}
static void uav_write_zdepth(u32 slot, ID3D12Resource* tex)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {0};
    ud.Format = DXGI_FORMAT_R32_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.dev->lpVtbl->CreateUnorderedAccessView(
        g.dev, tex, NULL, &ud, srv_cpu(slot));
}
static D3D12_GPU_DESCRIPTOR_HANDLE srv_table(const u32 slots[SRV_TABLE_SIZE])
{
    if (g.frame->srv_ring_used >= SRV_RING_TABLES) {
        D3D12_GPU_DESCRIPTOR_HANDLE invalid = {0};
        return invalid;
    }
    const u32 base = g.frame->srv_ring_used++ * SRV_TABLE_SIZE;
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    D3D12_CPU_DESCRIPTOR_HANDLE src[SRV_TABLE_SIZE];
    const UINT dst_size = SRV_TABLE_SIZE;
    g.frame->srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.frame->srv_heap, &dst);
    dst.ptr += (size_t)base * g.srv_step;
    for (u32 i = 0; i < SRV_TABLE_SIZE; i++)
        src[i] = srv_cpu(slots[i]);
    g.dev->lpVtbl->CopyDescriptors(
        g.dev, 1, &dst, &dst_size, SRV_TABLE_SIZE, src, NULL,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    g.frame->srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g.frame->srv_heap, &h);
    h.ptr += (u64)base * g.srv_step;
    return h;
}
static int depth_snapshot_descriptors(
    u32 zindex, int packed, D3D12_GPU_DESCRIPTOR_HANDLE* out_source,
    D3D12_GPU_DESCRIPTOR_HANDLE* out_destination)
{
    if (zindex >= MAX_SURFACES || g.frame->srv_ring_used >= SRV_RING_TABLES)
        return 0;
    const u32 base = g.frame->srv_ring_used++ * SRV_TABLE_SIZE;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    g.frame->srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(
        g.frame->srv_heap, &cpu);
    cpu.ptr += (size_t)base * g.srv_step;
    g.dev->lpVtbl->CopyDescriptorsSimple(
        g.dev, 1, cpu, srv_cpu(SRV_DEPTH_SOURCE_BASE + zindex),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpu.ptr += g.srv_step;
    g.dev->lpVtbl->CopyDescriptorsSimple(
        g.dev, 1, cpu, srv_cpu(SRV_STENCIL_SOURCE_BASE + zindex),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpu.ptr += g.srv_step;
    g.dev->lpVtbl->CopyDescriptorsSimple(
        g.dev, 1, cpu, srv_cpu((packed ? UAV_PACKED_DEPTH_BASE : UAV_ZDEPTH_BASE) + zindex),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    g.frame->srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
        g.frame->srv_heap, &gpu);
    gpu.ptr += (u64)base * g.srv_step;
    *out_source = gpu;
    gpu.ptr += 2u * g.srv_step;
    *out_destination = gpu;
    return 1;
}
static D3D12_CPU_DESCRIPTOR_HANDLE smp_cpu(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.smp_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.smp_cpu_heap, &h);
    h.ptr += (size_t)slot * g.smp_step;
    return h;
}
static u32 sampler_slot(const rsx_dsp_texture* t)
{
    const D3D12_SAMPLER_DESC sd = decode_sampler(t);
    const u64 key = fnv1a(&sd, sizeof(sd), 1469598103934665603ull);
    for (u32 i = 0; i < g.n_samplers; i++)
        if (g.smp_keys[i] == key && !memcmp(&g.smp_descs[i], &sd, sizeof(sd))) return 1 + i;
    if (g.n_samplers >= SMP_CACHE_SLOTS) return SMP_DEFAULT;
    const u32 slot = 1 + g.n_samplers;
    g.dev->lpVtbl->CreateSampler(g.dev, &sd, smp_cpu(slot));
    g.smp_descs[g.n_samplers] = sd;
    g.smp_keys[g.n_samplers++] = key;
    return slot;
}
static D3D12_GPU_DESCRIPTOR_HANDLE sampler_table(const u32 slots[SMP_TABLE_SIZE])
{
    for (u32 i = 0; i < g.frame->smp_ring_used; i++) {
        if (memcmp(g.frame->smp_ring_slots[i], slots, sizeof(g.frame->smp_ring_slots[i])) == 0) {
            D3D12_GPU_DESCRIPTOR_HANDLE cached;
            g.frame->smp_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(
                g.frame->smp_heap, &cached);
            cached.ptr += (u64)(i * SMP_TABLE_SIZE) * g.smp_step;
            return cached;
        }
    }
    /* The caller preflights capacity before recording render-state commands,
     * because a command-list flush here would discard that state. */
    if (g.frame->smp_ring_used >= SMP_RING_TABLES) {
        D3D12_GPU_DESCRIPTOR_HANDLE invalid = {0};
        return invalid;
    }
    const u32 base = g.frame->smp_ring_used++ * SMP_TABLE_SIZE;
    memcpy(g.frame->smp_ring_slots[base / SMP_TABLE_SIZE], slots,
           sizeof(g.frame->smp_ring_slots[0]));
    D3D12_CPU_DESCRIPTOR_HANDLE dst;
    D3D12_CPU_DESCRIPTOR_HANDLE src[SMP_TABLE_SIZE];
    const UINT dst_size = SMP_TABLE_SIZE;
    g.frame->smp_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.frame->smp_heap, &dst);
    dst.ptr += (size_t)base * g.smp_step;
    for (u32 i = 0; i < SMP_TABLE_SIZE; i++)
        src[i] = smp_cpu(slots[i]);
    g.dev->lpVtbl->CopyDescriptors(
        g.dev, 1, &dst, &dst_size, SMP_TABLE_SIZE, src, NULL,
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    g.frame->smp_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g.frame->smp_heap, &h);
    h.ptr += (u64)base * g.smp_step;
    return h;
}

/* ---------------------------------------------------------------------------
 * Submit command lists; recycle a slot only after its actual GPU fence.
 * -----------------------------------------------------------------------*/
static void ld_flush(ld_flush_reason reason)
{
#if defined(YZ_PERF_PROFILE)
    const LONGLONG flush_begin = ld_profile_qpc();
    g_ld_profile.total.flush_reason[reason]++;
    ld_profile_note_ring_highwater();
    if (g.frame->timing_heap && g.frame->timing_readback && g.timing_frequency) {
        rsx_rec_EndQuery(g.frame, g.frame->timing_heap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        rsx_rec_ResolveQueryData(g.frame, g.frame->timing_heap,
            D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, g.frame->timing_readback, 0);
    }
#else
    (void)reason;
#endif
    rsx_rec_drain(); /* Recording must finish before API submission/reset. */
    const HRESULT close_hr = g.frame->list->lpVtbl->Close(g.frame->list);
    if (FAILED(close_hr)) {
        fprintf(stderr,
                "[d3d-fail] command-list Close hr=0x%08lX frame=%u\n",
                (unsigned long)close_hr, g_ld_frames);
        ld_dump_dred("Close", close_hr);
        g.ready = 0;
        return;
    }
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.frame->list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    const u64 v = ++g.fence_value;
    const HRESULT signal_hr =
        g.queue->lpVtbl->Signal(g.queue, g.fence, v);
    if (FAILED(signal_hr)) {
        fprintf(stderr,
                "[d3d-fail] queue Signal hr=0x%08lX frame=%u\n",
                (unsigned long)signal_hr, g_ld_frames);
        ld_dump_dred("Signal", signal_hr);
        g.ready = 0;
        return;
    }
    g.frame->submitted_fence=v;
    /* Only presentation can overlap. Readback, explicit reference, movie and
     * capacity flushes wait for this submission, which orders all older work
     * on the same queue as well. No guest completion is generated here. */
    const int rotate=reason==LD_FLUSH_PRESENT && g.frame_count>1;
    if(rotate) {
        g.frame_index=(g.frame_index+1)%g.frame_count;
        g.frame=&g.frames[g.frame_index];
    }
    const u64 wait_value=g.frame->submitted_fence;
    const u64 completed=g.fence->lpVtbl->GetCompletedValue(g.fence);
    if(completed==UINT64_MAX) {
        ld_dump_dred("GetCompletedValue",DXGI_ERROR_DEVICE_REMOVED);
        g.ready=0;return;
    }
    if (completed < wait_value) {
#if defined(YZ_PERF_PROFILE)
        const LONGLONG wait_begin = ld_profile_qpc();
#endif
        const HRESULT event_hr =
            g.fence->lpVtbl->SetEventOnCompletion(
                g.fence, wait_value, g.fence_event);
        if (FAILED(event_hr)) {
            fprintf(stderr,
                    "[d3d-fail] fence SetEventOnCompletion "
                    "hr=0x%08lX frame=%u\n",
                    (unsigned long)event_hr, g_ld_frames);
            ld_dump_dred("SetEventOnCompletion", event_hr);
            g.ready = 0;
            return;
        }
        if(WaitForSingleObject(g.fence_event,INFINITE)!=WAIT_OBJECT_0 ||
           g.fence->lpVtbl->GetCompletedValue(g.fence)==UINT64_MAX) {
            fprintf(stderr,"[d3d-fail] submission fence wait failed\n");
            g.ready=0;return;
        }
#if defined(YZ_PERF_PROFILE)
        const u64 wait_ticks = (u64)(ld_profile_qpc() - wait_begin);
        g_ld_profile.total.fence_wait_qpc += wait_ticks;
        g_ld_profile.total.fence_reason_qpc[reason] += wait_ticks;
#endif
    }
    ld_drain_info_queue("flush");
#if defined(YZ_PERF_PROFILE)
    if (g.frame->submitted_fence && g.frame->timing_heap && g.frame->timing_readback && g.timing_frequency) {
        u64* ticks = NULL;
        const D3D12_RANGE read = {0, 16}, written = {0, 0};
        if (SUCCEEDED(g.frame->timing_readback->lpVtbl->Map(g.frame->timing_readback, 0, &read, (void**)&ticks))) {
            if (ticks[1] >= ticks[0]) g_ld_profile.total.gpu_ticks += ticks[1] - ticks[0];
            g.frame->timing_readback->lpVtbl->Unmap(g.frame->timing_readback, 0, &written);
        }
    }
#endif
    /* Dynamic guest textures can replace a cached D3D resource while an
     * earlier draw in this command list still references the old one.  The
     * fence above is the first safe point at which those old resources may be
     * released. */
    for (u32 i = 0; i < g.frame->n_retired_textures; i++)
        g.frame->retired_textures[i]->lpVtbl->Release(g.frame->retired_textures[i]);
    g.frame->n_retired_textures = 0;
    if(FAILED(g.frame->alloc->lpVtbl->Reset(g.frame->alloc)) ||
       FAILED(g.frame->list->lpVtbl->Reset(g.frame->list,g.frame->alloc,NULL))) {
        fprintf(stderr,"[d3d-fail] completed submission slot reset failed\n");
        g.ready=0;return;
    }
    g.frame->bindings.valid=0; /* Reset discards all recorded bindings. */
    if(rotate) {
        g.frame->vb_used=g.frame->ib_used=g.frame->cb_used=g.frame->ps_cb_used=0;
        g.frame->srv_ring_used=g.frame->smp_ring_used=0;
    }
#if defined(YZ_PERF_PROFILE)
    if (g.frame->timing_heap && g.frame->timing_readback && g.timing_frequency)
        rsx_rec_EndQuery(g.frame, g.frame->timing_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
#endif
    g.frame->upload_used = 0;
    g.frame->geometry_input_used=g.frame->geometry_output_used=0;
#if defined(YZ_PERF_PROFILE)
    const u64 flush_ticks = (u64)(ld_profile_qpc() - flush_begin);
    g_ld_profile.total.flush_qpc += flush_ticks;
    g_ld_profile.total.flush_reason_qpc[reason] += flush_ticks;
#endif
}

/* Public wrapper for the RSX SET_REFERENCE / sync fence: block until the GPU
 * has finished all queued draws (mirrors RPCS3 nv406e::set_reference's sync(),
 * RSXThread.cpp), so the game's REF poll advances only after the GPU has really
 * caught up. Without it our async consumer writes REF instantly and races ahead
 * of real GPU time (measured: ours skips the fence wait RPCS3 performs). Gated
 * at the call site by RSX_FENCE_SYNC. */
void rsx_live_draw_flush(void)
{
    if (g.ready) ld_flush(LD_FLUSH_GUEST_REFERENCE);
}

static void retire_texture(ID3D12Resource* tex)
{
    if (!tex) return;
    if (g.frame->n_retired_textures >= MAX_RETIRED_TEXTURES)
        ld_flush(LD_FLUSH_RETIRE_QUEUE);
    g.frame->retired_textures[g.frame->n_retired_textures++] = tex;
    ld_profile_note_ring_highwater();
}

/* ---------------------------------------------------------------------------
 * texture upload (single-level + mip)
 * -----------------------------------------------------------------------*/
static u32 log2_u32(u32 v) { u32 n = 0; while (v > 1) { v >>= 1; n++; } return n; }
static u32 morton_index(u32 x, u32 y, u32 lw, u32 lh)
{
    u32 idx = 0, shift = 0;
    while (lw || lh) {
        if (lw) { idx |= (x & 1) << shift; x >>= 1; shift++; lw--; }
        if (lh) { idx |= (y & 1) << shift; y >>= 1; shift++; lh--; }
    }
    return idx;
}
static u8 remap_comp(const u8 s[4], u32 remap, u32 comp)
{
    const u32 op  = (remap >> (8 + comp * 2)) & 3;
    const u32 sel = (remap >> (comp * 2)) & 3;
    if (op == 0) return 0;
    if (op == 1) return 255;
    return s[sel];
}
static void decode_texel(u32 base_fmt, const u8* p, u32 remap, u8 d[4])
{
    u8 s[4];
    switch (base_fmt) {
    /* B8 is one channel. Broadcasting it to alpha as well is arguably more
     * faithful (the YUV movie combine shader samples each plane's .w), but
     * DO NOT do it until the USM decode actually fills those planes: while
     * they are empty it only makes the garbage VISIBLE, putting green noise
     * behind the title screen and every other movie-backed UI. Tried
     * 2026-09-01, reverted the same session. See ydkj-live-draw-rebuild. */
    case TEX_FMT_B8: s[0] = 255; s[1] = s[2] = s[3] = p[0]; break;
    case TEX_FMT_A4R4G4B4: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = (u8)(((v >> 12) & 0xF) * 17); s[1] = (u8)(((v >> 8) & 0xF) * 17);
        s[2] = (u8)(((v >> 4) & 0xF) * 17);  s[3] = (u8)((v & 0xF) * 17); break;
    }
    case TEX_FMT_A1R5G5B5: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        /* LD_FORCE_A1_OPAQUE=1: experiment, not a fix. A PS1 15-bit pixel uses
         * the top bit as a semi-transparency FLAG, and it is clear for ordinary
         * opaque pixels -- so decoding it as A1R5G5B5 alpha gives alpha=0 for
         * essentially the whole framebuffer. If the pipeline then blends on
         * source alpha, every pixel is discarded and the surface reads black,
         * which is exactly what LD_SURF_DUMP reports for all nine surfaces.
         * This flips alpha to opaque so that theory can be tested in one run
         * instead of argued about. */
        { static int fo = -1;
          if (fo < 0) fo = getenv("LD_FORCE_A1_OPAQUE") ? 1 : 0;
          s[0] = fo ? 255 : ((v & 0x8000) ? 255 : 0); }
        s[1] = (u8)(((v >> 10) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x1F) * 255 / 31);
        s[3] = (u8)((v & 0x1F) * 255 / 31); break;
    }
    case TEX_FMT_R5G6B5: {
        const u16 v = (u16)((p[0] << 8) | p[1]);
        s[0] = 255;
        s[1] = (u8)(((v >> 11) & 0x1F) * 255 / 31);
        s[2] = (u8)(((v >> 5) & 0x3F) * 255 / 63);
        s[3] = (u8)((v & 0x1F) * 255 / 31); break;
    }
    case TEX_FMT_G8B8:
        s[0] = 255;
        s[1] = s[2] = p[0];
        s[3] = p[1];
        break;
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT: {
        /* Four big-endian halves, X Y Z W -> R G B A. Values are linear and
         * may exceed 1.0 (these are render targets), so clamp rather than
         * wrap. Decoded to 8-bit because the cache is RGBA8 throughout. */
        for (int c = 0; c < 4; c++) {
            const u16 hbits = (u16)((p[c * 2] << 8) | p[c * 2 + 1]);
            const int  exp  = (hbits >> 10) & 0x1F;
            const int  man  = hbits & 0x3FF;
            float f;
            if (exp == 0)        f = (float)man * (1.0f / 16384.0f) * (1.0f / 64.0f);
            else if (exp == 31)  f = 1.0f;
            else {
                float m = 1.0f + (float)man / 1024.0f;
                int   e = exp - 15;
                while (e > 0) { m *= 2.0f; e--; }
                while (e < 0) { m *= 0.5f; e++; }
                f = m;
            }
            if (hbits & 0x8000) f = 0.0f;   /* negative -> clamp to zero */
            if (f > 1.0f) f = 1.0f;
            /* s[] is A,R,G,B; the halves arrive X,Y,Z,W = R,G,B,A. */
            const u8 v = (u8)(f * 255.0f + 0.5f);
            if (c == 3) s[0] = v; else s[c + 1] = v;
        }
        break;
    }
    case TEX_FMT_DEPTH24_D8: s[0] = 255; s[1] = s[2] = s[3] = p[0]; break;
    default: s[0] = p[0]; s[1] = p[1]; s[2] = p[2]; s[3] = p[3]; break;
    }
    d[0] = remap_comp(s, remap, 1);
    d[1] = remap_comp(s, remap, 2);
    d[2] = remap_comp(s, remap, 3);
    d[3] = remap_comp(s, remap, 0);
}

typedef struct { u32 w, h; const u8* data; u32 row_bytes, rows; } tex_level_t;

static ID3D12Resource* create_texture_mipped(DXGI_FORMAT fmt, const tex_level_t* lv, u32 n)
{
    if (n == 0) return NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = lv[0].w; rd.Height = lv[0].h; rd.DepthOrArraySize = 1;
    rd.MipLevels = (u16)n; rd.Format = fmt; rd.SampleDesc.Count = 1;
    ID3D12Resource* tex = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                      &IID_ID3D12Resource, (void**)&tex)))
        return NULL;
    for (u32 m = 0; m < n; m++) {
        const u32 pitch = (lv[m].row_bytes + 255) & ~255u;
        const u32 start = (g.frame->upload_used + 511) & ~511u;
        if ((u64)start + (u64)pitch * lv[m].rows > UPLOAD_SIZE) break;
        for (u32 y = 0; y < lv[m].rows; y++)
            memcpy(g.frame->upload_mapped + start + (size_t)y * pitch,
                   lv[m].data + (size_t)y * lv[m].row_bytes, lv[m].row_bytes);
        D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
        src.pResource = g.frame->upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = start;
        src.PlacedFootprint.Footprint.Format = fmt;
        src.PlacedFootprint.Footprint.Width = lv[m].w;
        src.PlacedFootprint.Footprint.Height = lv[m].h;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m;
        rsx_rec_CopyTextureRegion(g.frame, &dst, 0, 0, 0, &src, NULL);
        g.frame->upload_used = start + pitch * lv[m].rows;
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.texture_upload_bytes +=
            (u64)pitch * lv[m].rows;
#endif
        ld_profile_note_ring_highwater();
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    rsx_rec_ResourceBarrier(g.frame, 1, &b);
    return tex;
}

/* Cubemap faces are stored face-major in guest memory. D3D12 subresources use
 * that same order: face * mip_count + mip. */
static ID3D12Resource* create_texture_cube(
    DXGI_FORMAT fmt, const tex_level_t* lv, u32 n_mips)
{
    if (!n_mips) return NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = lv[0].w; rd.Height = lv[0].h; rd.DepthOrArraySize = 6;
    rd.MipLevels = (u16)n_mips; rd.Format = fmt; rd.SampleDesc.Count = 1;
    ID3D12Resource* tex = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&tex)))
        return NULL;
    for (u32 face = 0; face < 6; face++) {
        for (u32 mip = 0; mip < n_mips; mip++) {
            const tex_level_t* level = &lv[face * n_mips + mip];
            const u32 pitch = (level->row_bytes + 255) & ~255u;
            const u32 start = (g.frame->upload_used + 511) & ~511u;
            if ((u64)start + (u64)pitch * level->rows > UPLOAD_SIZE) {
                /* Earlier faces may already be queued/recorded. Keep their
                 * destination alive through recording AND GPU completion. */
                retire_texture(tex);
                return NULL;
            }
            for (u32 y = 0; y < level->rows; y++)
                memcpy(g.frame->upload_mapped + start + (size_t)y * pitch,
                       level->data + (size_t)y * level->row_bytes,
                       level->row_bytes);
            D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
            src.pResource = g.frame->upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = start;
            src.PlacedFootprint.Footprint.Format = fmt;
            src.PlacedFootprint.Footprint.Width = level->w;
            src.PlacedFootprint.Footprint.Height = level->h;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = pitch;
            dst.pResource = tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = face * n_mips + mip;
            rsx_rec_CopyTextureRegion(g.frame, &dst, 0, 0, 0, &src, NULL);
            g.frame->upload_used = start + pitch * level->rows;
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.texture_upload_bytes +=
                (u64)pitch * level->rows;
#endif
            ld_profile_note_ring_highwater();
        }
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    rsx_rec_ResourceBarrier(g.frame, 1, &b);
    return tex;
}

static ID3D12Resource* create_texture_rgba(const u8* rgba, u32 w, u32 h)
{
    tex_level_t lv = { w, h, rgba, w * 4, h };
    return create_texture_mipped(DXGI_FORMAT_R8G8B8A8_UNORM, &lv, 1);
}

static u32 texture_source_span(const rsx_dsp_texture* t)
{
    const u32 base_fmt = t->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    const int linear = (t->format & TEX_FMT_LINEAR) != 0;
    u32 texel_size = 0, block_size = 0;
    switch (base_fmt) {
    case TEX_FMT_DXT1:  block_size = 8; break;
    case TEX_FMT_DXT23:
    case TEX_FMT_DXT45: block_size = 16; break;
    case TEX_FMT_B8: texel_size = 1; break;
    case TEX_FMT_A4R4G4B4:
    case TEX_FMT_A1R5G5B5:
    case TEX_FMT_R5G6B5:
    case TEX_FMT_G8B8: texel_size = 2; break;
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT: texel_size = 8; break;
    case TEX_FMT_A8R8G8B8:
    case TEX_FMT_DEPTH24_D8: texel_size = 4; break;
    default: return 0;
    }
    if (!t->width || !t->height || t->width > 4096 || t->height > 4096 ||
        t->dimension != 2)
        return 0;
    u32 n_mips = t->mipmaps ? t->mipmaps : 1;
    if (n_mips > 14) n_mips = 14;
    /* A pitch-linear (LN) texture cannot carry a mip chain -- the RSX samples
     * only its base level and ignores the mipmap count in SET_TEXTURE_FORMAT.
     * Honouring that count instead walks `off += pitch * mh` through whatever
     * guest memory follows the base image and builds every lower level out of
     * it, and a minified draw then samples that garbage.
     *
     * Cherry-picked by hand from flow/live-draw 3f4bdb9, where flOw's EULA
     * showed "banded olive stripes with a ghosted second copy of the page".
     * Twisted Metal binds PS1 VRAM pitch-linear (fmt 0xE2/0xE1 carry
     * TEX_FMT_LINEAR) and shows banded green/magenta stripes, which is the same
     * symptom on the same mechanism. */
    if (linear) n_mips = 1;
    if (t->cubemap && block_size) {
        n_mips = 1;
        for (u32 d = (t->width < t->height ? t->width : t->height) / 4;
             d > 1; d >>= 1)
            n_mips++;
        if (t->mipmaps && n_mips > t->mipmaps)
            n_mips = t->mipmaps;
        if (n_mips > 14) n_mips = 14;
    }
    u32 mw = t->width, mh = t->height, span = 0;
    for (u32 m = 0; m < n_mips; m++) {
        if (block_size)
            span += ((mw + 3) / 4) * block_size * ((mh + 3) / 4);
        else {
            const u32 pitch = (m == 0 && linear && t->pitch)
                ? t->pitch : mw * texel_size;
            span += pitch * mh;
        }
        if (mw == 1 && mh == 1) break;
        mw = mw > 1 ? mw >> 1 : 1;
        mh = mh > 1 ? mh >> 1 : 1;
    }
    return t->cubemap ? span * 6 : span;
}

static u64 texture_content_hash(const rsx_dsp_texture* t, int* readable)
{
#if defined(YZ_PERF_PROFILE)
    const u64 hash_begin = ld_profile_qpc();
#endif
    const u32 span = texture_source_span(t);
    const u8* src = span ? guest_ptr(t->location, t->offset, span) : NULL;
    if (!src) {
        *readable = 0;
        return 0;
    }
    /* LD_TEXSRC_DBG=1: is the guest data there AT ALL?
     *
     * "Draws execute, forced green fills the screen, the sampled texture is
     * black" keeps arriving as the same question, and the decoded-RGBA dump
     * cannot answer it: that dump lives on the uncompressed path, so a DXT
     * texture -- which is most of a real game's atlas -- never reaches it.
     * Counting non-zero SOURCE bytes covers every format and separates "the
     * upload never happened" from "decode ate it". */
    { static int ts = -1; static int tsn = 0;
      if (ts < 0) ts = getenv("LD_TEXSRC_DBG") ? 1 : 0;
      if (ts && tsn < 16) {
          u32 nz = 0;
          for (u32 q = 0; q < span; q++) if (src[q]) nz++;
          char head[80]; int hn = 0;
          for (u32 q = 0; q < 16 && q < span; q++)
              hn += snprintf(head + hn, sizeof head - hn, "%02X", src[q]);
          fprintf(stderr, "[tex-src] loc=%u off=0x%08X %ux%u fmt=0x%02X mips=%u"
                          " remap=0x%08X filter=0x%08X head=%s"
                          " span=%u nonzero=%u\n",
                  t->location, t->offset, t->width, t->height,
                  t->format & 0xFF, t->mipmaps, t->remap, t->filter, head,
                  span, nz);
          fflush(stderr);
          tsn++;
      } }

    /* One hash per cached texture per presented frame.  Word-at-a-time FNV is
     * deliberately cheap; this is a mutation detector, not a content ID. */
    u64 hash = 1469598103934665603ull;
    u32 i = 0;
    for (; i + 8 <= span; i += 8) {
        u64 word;
        memcpy(&word, src + i, sizeof(word));
        hash ^= word;
        hash *= 1099511628211ull;
    }
    for (; i < span; i++) {
        hash ^= src[i];
        hash *= 1099511628211ull;
    }
    *readable = 1;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.texture_hash_qpc += ld_profile_qpc() - hash_begin;
    g_ld_profile.total.texture_hash_bytes += span;
#endif
    return hash;
}

static ID3D12Resource* decode_guest_texture(const rsx_dsp_texture* t, u32 remap)
{
    const u32 base_fmt = t->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    const int linear = (t->format & TEX_FMT_LINEAR) != 0;
    const u32 w = t->width, h = t->height;
    if (!w || !h || w > 4096 || h > 4096 || t->dimension != 2)
        return NULL;
    u32 n_mips = t->mipmaps ? t->mipmaps : 1;
    if (n_mips > 14) n_mips = 14;
    /* A pitch-linear (LN) texture cannot carry a mip chain -- the RSX samples
     * only its base level and ignores the mipmap count in SET_TEXTURE_FORMAT.
     * Honouring that count instead walks `off += pitch * mh` through whatever
     * guest memory follows the base image and builds every lower level out of
     * it, and a minified draw then samples that garbage.
     *
     * Cherry-picked by hand from flow/live-draw 3f4bdb9, where flOw's EULA
     * showed "banded olive stripes with a ghosted second copy of the page".
     * Twisted Metal binds PS1 VRAM pitch-linear (fmt 0xE2/0xE1 carry
     * TEX_FMT_LINEAR) and shows banded green/magenta stripes, which is the same
     * symptom on the same mechanism. */
    if (linear) n_mips = 1;

    if (base_fmt == TEX_FMT_DXT1 || base_fmt == TEX_FMT_DXT23 ||
        base_fmt == TEX_FMT_DXT45) {
        const DXGI_FORMAT dxgi = base_fmt == TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                               : base_fmt == TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                           : DXGI_FORMAT_BC3_UNORM;
        const u32 block = base_fmt == TEX_FMT_DXT1 ? 8 : 16;
        const u32 total = texture_source_span(t);
        const u8* src = guest_ptr(t->location, t->offset, total);
        if (!src) return NULL;
        if (t->cubemap) {
            n_mips = 1;
            for (u32 d = (w < h ? w : h) / 4; d > 1; d >>= 1)
                n_mips++;
            if (t->mipmaps && n_mips > t->mipmaps)
                n_mips = t->mipmaps;
            if (n_mips > 14) n_mips = 14;
            const u32 face_span = total / 6;
            tex_level_t cube_levels[6 * 14];
            u32 level = 0;
            for (u32 face = 0; face < 6; face++) {
                u32 mw = w, mh = h, off = 0;
                for (u32 mip = 0; mip < n_mips; mip++) {
                    const u32 bw = (mw + 3) / 4, bh = (mh + 3) / 4;
                    cube_levels[level].w = (mw + 3) & ~3u;
                    cube_levels[level].h = (mh + 3) & ~3u;
                    cube_levels[level].data =
                        src + (size_t)face * face_span + off;
                    cube_levels[level].row_bytes = bw * block;
                    cube_levels[level].rows = bh;
                    level++;
                    off += bw * block * bh;
                    mw = mw > 1 ? mw >> 1 : 1;
                    mh = mh > 1 ? mh >> 1 : 1;
                }
            }
            return create_texture_cube(dxgi, cube_levels, n_mips);
        }
        tex_level_t levels[14];
        u32 mw = w, mh = h, off = 0, n = 0;
        for (u32 m = 0; m < n_mips; m++) {
            const u32 bw = (mw + 3) / 4, bh = (mh + 3) / 4;
            levels[n].w = (mw + 3) & ~3u;
            levels[n].h = (mh + 3) & ~3u;
            levels[n].data = src + off;
            levels[n].row_bytes = bw * block;
            levels[n].rows = bh;
            n++;
            off += bw * block * bh;
            if (mw == 1 && mh == 1) break;
            mw = mw > 1 ? mw >> 1 : 1;
            mh = mh > 1 ? mh >> 1 : 1;
        }
        return create_texture_mipped(dxgi, levels, n);
    }

    u32 texel_size;
    switch (base_fmt) {
    case TEX_FMT_B8: texel_size = 1; break;
    case TEX_FMT_A4R4G4B4:
    case TEX_FMT_A1R5G5B5:
    case TEX_FMT_R5G6B5:
    case TEX_FMT_G8B8: texel_size = 2; break;
    case TEX_FMT_A8R8G8B8:
    case TEX_FMT_DEPTH24_D8: texel_size = 4; break;
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT: texel_size = 8; break;
    default: return NULL;
    }
    if (!linear && ((w & (w - 1)) || (h & (h - 1)))) return NULL;
    const u32 span = texture_source_span(t);
    const u8* src = guest_ptr(t->location, t->offset, span);
    if (!src) return NULL;

    if (t->cubemap) {
        u32 available_mips = 1;
        for (u32 mw = w, mh = h; mw > 1 || mh > 1; available_mips++) {
            mw = mw > 1 ? mw >> 1 : 1;
            mh = mh > 1 ? mh >> 1 : 1;
        }
        if (n_mips > available_mips) n_mips = available_mips;
        const u32 face_span = span / 6;
        u8* rgba[6 * 14] = {0};
        tex_level_t cube_levels[6 * 14];
        u32 level = 0;
        int oom = 0;
        for (u32 face = 0; face < 6 && !oom; face++) {
            u32 mw = w, mh = h, off = 0;
            for (u32 mip = 0; mip < n_mips; mip++) {
                const u32 pitch = (mip == 0 && linear && t->pitch)
                    ? t->pitch : mw * texel_size;
                rgba[level] = (u8*)malloc((size_t)mw * mh * 4);
                if (!rgba[level]) {
                    oom = 1;
                    break;
                }
                const u32 lw = log2_u32(mw), lh = log2_u32(mh);
                const u8* level_src =
                    src + (size_t)face * face_span + off;
                for (u32 y = 0; y < mh; y++)
                    for (u32 x = 0; x < mw; x++) {
                        const u8* pixel = linear
                            ? level_src + (size_t)y * pitch +
                                (size_t)x * texel_size
                            : level_src +
                                (size_t)morton_index(x, y, lw, lh) *
                                    texel_size;
                        decode_texel(
                            base_fmt, pixel, remap,
                            rgba[level] + ((size_t)y * mw + x) * 4);
                    }
                cube_levels[level].w = mw;
                cube_levels[level].h = mh;
                cube_levels[level].data = rgba[level];
                cube_levels[level].row_bytes = mw * 4;
                cube_levels[level].rows = mh;
                level++;
                off += pitch * mh;
                if (mw == 1 && mh == 1) break;
                mw = mw > 1 ? mw >> 1 : 1;
                mh = mh > 1 ? mh >> 1 : 1;
            }
        }
        ID3D12Resource* resource = (!oom && level == 6 * n_mips)
            ? create_texture_cube(
                DXGI_FORMAT_R8G8B8A8_UNORM, cube_levels, n_mips)
            : NULL;
        for (u32 i = 0; i < level; i++) free(rgba[i]);
        return resource;
    }

    u8* rgba[14] = {0};
    tex_level_t levels[14];
    u32 mw = w, mh = h, off = 0, n = 0;
    int oom = 0;
    for (u32 m = 0; m < n_mips; m++) {
        const u32 pitch = (m == 0 && linear && t->pitch)
            ? t->pitch : mw * texel_size;
        rgba[n] = (u8*)malloc((size_t)mw * mh * 4);
        if (!rgba[n]) { oom = 1; break; }
        const u32 lw = log2_u32(mw), lh = log2_u32(mh);
        const u8* level_src = src + off;
        /* LD_PLANE_DUMP=1: write the first few big single-channel (B8) planes
         * straight out as PGM, exactly as the guest laid them down. Settles
         * "is the video decode producing an image?" without guessing from the
         * composited frame. ponytail: one-shot debug, costs nothing when off. */
        { static int pd = -1; static int pdn = 0;
          if (pd < 0) { const char* e = getenv("LD_PLANE_DUMP"); pd = e ? atoi(e) : 0; }
          if (pd && (int)g_ld_frames >= pd && m == 0 && base_fmt == TEX_FMT_B8 && mw >= 320 && pdn < 6) {
              char fn[128];
              snprintf(fn, sizeof fn, "scratch/plane_%02d_%ux%u_p%u.pgm", pdn, mw, mh, pitch);
              FILE* pf = fopen(fn, "wb");
              if (pf) {
                  fprintf(pf, "P5\n%u %u\n255\n", mw, mh);
                  for (u32 yy = 0; yy < mh; yy++)
                      fwrite(level_src + (size_t)yy * pitch, 1, mw, pf);
                  fclose(pf);
                  fprintf(stderr, "[plane-dump] %s linear=%d\n", fn, linear);
              }
              pdn++;
          } }
        for (u32 y = 0; y < mh; y++)
            for (u32 x = 0; x < mw; x++) {
                const u8* pixel = linear
                    ? level_src + (size_t)y * pitch + (size_t)x * texel_size
                    : level_src + (size_t)morton_index(x, y, lw, lh) * texel_size;
                decode_texel(base_fmt, pixel, remap,
                             rgba[n] + ((size_t)y * mw + x) * 4);
            }
        /* LD_TEXRGBA_DUMP=1: the DECODED level-0 RGBA, with a non-black count
         * and the remap that produced it.
         *
         * Forcing the fragment shader to green fills every surface completely
         * (368,640 = 720x512 and 921,600 = 1280x720), so the draws reach the
         * GPU and write every pixel -- the only thing left is that the sampled
         * texture comes back black. The guest MEMORY has content (PS1 VRAM
         * carries the BIOS boot screen), so the question is whether decode
         * preserves it. remap is applied here, and a remap that zeroes channels
         * would produce exactly this. */
        { static int td = -1; static int tdn = 0; static u32 tdf = 0;
          /* LD_TEXRGBA_DUMP=<hex fmt>: only that base format -- the 12-dump
           * cap otherwise fills with whatever decodes first. */
          if (td < 0) { const char* e = getenv("LD_TEXRGBA_DUMP");
                        td = e ? 1 : 0; tdf = e ? (u32)strtoul(e, 0, 16) : 0; }
          const int td_this = td && !(tdf > 1 && base_fmt != tdf);
          /* No size floor: the floor was picked for one title's full-screen
           * textures and silently skipped every UI atlas, which is the size
           * a 2D layer is made of. The dump is opt-in; dump what was asked. */
          if (td_this && m == 0 && tdn < 12 && mw && mh) {
              u64 nb = 0, na = 0;
              for (u32 q = 0; q < mw * mh; q++) {
                  const u8* px = rgba[n] + (size_t)q * 4;
                  if (px[0] || px[1] || px[2]) nb++;
                  if (px[3] > 0x1A) na++;
              }
              char fn[160];
              snprintf(fn, sizeof fn, "scratch/tex_%02d_%ux%u_f%02X.ppm",
                       tdn, mw, mh, base_fmt);
              FILE* tf = fopen(fn, "wb");
              if (tf) {
                  fprintf(tf, "P6\n%u %u\n255\n", mw, mh);
                  for (u32 q = 0; q < mw * mh; q++)
                      fwrite(rgba[n] + (size_t)q * 4, 1, 3, tf);
                  fclose(tf);
              }
              fprintf(stderr, "[tex-rgba] %s fmt=0x%02X remap=0x%04X linear=%d"
                              " pitch=%u nonblack=%llu/%u alpha>0x1A=%llu\n",
                      fn, base_fmt, remap, linear, pitch,
                      (unsigned long long)nb, mw * mh, (unsigned long long)na);
              tdn++;
          } }
        levels[n].w = mw;
        levels[n].h = mh;
        levels[n].data = rgba[n];
        levels[n].row_bytes = mw * 4;
        levels[n].rows = mh;
        n++;
        off += pitch * mh;
        if (mw == 1 && mh == 1) break;
        mw = mw > 1 ? mw >> 1 : 1;
        mh = mh > 1 ? mh >> 1 : 1;
    }
    ID3D12Resource* resource = (!oom && n)
        ? create_texture_mipped(DXGI_FORMAT_R8G8B8A8_UNORM, levels, n) : NULL;
    for (u32 m = 0; m < n; m++) free(rgba[m]);
    return resource;
}

static ID3D12Resource* decode_guest_texture_profiled(
    const rsx_dsp_texture* texture, u32 remap)
{
#if defined(YZ_PERF_PROFILE)
    const LONGLONG begin = ld_profile_qpc();
    g_ld_profile.total.texture_decode_calls++;
#endif
    ID3D12Resource* result = decode_guest_texture(texture, remap);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.texture_decode_qpc +=
        (u64)(ld_profile_qpc() - begin);
#endif
    return result;
}

static void write_texture_srv(u32 index, const texcache_t* entry)
{
    const u32 base_fmt = entry->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
    const int compressed = base_fmt == TEX_FMT_DXT1 ||
                           base_fmt == TEX_FMT_DXT23 ||
                           base_fmt == TEX_FMT_DXT45;
    if (entry->cubemap) {
        static const u32 sel2d3d[4] = { 3, 0, 1, 2 };
        static const u32 out2comp[4] = { 1, 2, 3, 0 };
        u32 mapping[4];
        for (u32 out = 0; out < 4; out++) {
            const u32 comp = out2comp[out];
            const u32 op = (entry->remap >> (8 + comp * 2)) & 3;
            const u32 sel = (entry->remap >> (comp * 2)) & 3;
            mapping[out] = op == 0 ? 4 : op == 1 ? 5 : sel2d3d[sel];
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
        desc.Format = !compressed ? DXGI_FORMAT_R8G8B8A8_UNORM
                    : base_fmt == TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                    : base_fmt == TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                : DXGI_FORMAT_BC3_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        desc.Shader4ComponentMapping =
            !compressed || entry->remap == 0xAAE4
            ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
            : mapping[0] | (mapping[1] << 3) | (mapping[2] << 6) |
              (mapping[3] << 9) | (1u << 12);
        desc.TextureCube.MipLevels = (UINT)-1;
        g.dev->lpVtbl->CreateShaderResourceView(
            g.dev, entry->tex, &desc, srv_cpu(SRV_TEXTURE_BASE + index));
    } else if (compressed && entry->remap != 0xAAE4) {
        static const u32 sel2d3d[4] = { 3, 0, 1, 2 };
        static const u32 out2comp[4] = { 1, 2, 3, 0 };
        u32 mapping[4];
        for (u32 out = 0; out < 4; out++) {
            const u32 comp = out2comp[out];
            const u32 op = (entry->remap >> (8 + comp * 2)) & 3;
            const u32 sel = (entry->remap >> (comp * 2)) & 3;
            mapping[out] = op == 0 ? 4 : op == 1 ? 5 : sel2d3d[sel];
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {0};
        desc.Format = base_fmt == TEX_FMT_DXT1 ? DXGI_FORMAT_BC1_UNORM
                    : base_fmt == TEX_FMT_DXT23 ? DXGI_FORMAT_BC2_UNORM
                                                : DXGI_FORMAT_BC3_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = mapping[0] | (mapping[1] << 3) |
                                       (mapping[2] << 6) | (mapping[3] << 9) |
                                       (1u << 12);
        desc.Texture2D.MipLevels = (UINT)-1;
        g.dev->lpVtbl->CreateShaderResourceView(
            g.dev, entry->tex, &desc, srv_cpu(SRV_TEXTURE_BASE + index));
    } else {
        srv_write(SRV_TEXTURE_BASE + index, entry->tex);
    }
}

/* Decode a guest texture descriptor into a cached SRV slot.  Unlike the old
 * descriptor-only cache, re-hash the source once per frame and refresh the
 * D3D resource when the game rewrites the same guest address. */
/* Texture source copies make unchanged-frame validation a vectorized memcmp.
 * All bytes are checked; memory is bounded, with the existing hash fallback. */
static size_t g_ld_source_bytes;
static u64 g_ld_source_compares, g_ld_source_fallbacks, g_ld_source_clean;
static size_t ld_source_limit(void)
{
    static size_t limit = (size_t)-1;
    if (limit == (size_t)-1) {
        const char* e = getenv("BLEACH_TEXTURE_SNAPSHOT_MB");
        unsigned mb = e ? (unsigned)strtoul(e, NULL, 10) : 128u;
        if (mb > 512u) mb = 512u;
        limit = (size_t)mb * 1024u * 1024u;
    }
    return limit;
}
static rsx_source_snapshot ld_capture_source(const rsx_dsp_texture* t)
{
    const u32 span = texture_source_span(t);
    const u8* source = span ? guest_ptr(t->location, t->offset, span) : NULL;
    return rsx_source_capture(source, span, &g_ld_source_bytes, ld_source_limit());
}
static uint64_t ld_watch_source(const rsx_dsp_texture* t)
{
    uint64_t token = 0;
    const u32 span = texture_source_span(t);
    const u8* source = span ? guest_ptr(t->location, t->offset, span) : NULL;
    if (g_source_watch) g_source_watch(source, span, &token);
    return token;
}
static int ld_compare_source(const texcache_t* entry, const rsx_dsp_texture* t,
                             uint64_t* token)
{
#if defined(YZ_PERF_PROFILE)
    const u64 begin = ld_profile_qpc();
#endif
    const u32 span = texture_source_span(t);
    const u8* source = span ? guest_ptr(t->location, t->offset, span) : NULL;
    if (g_source_watch && g_source_watch(source, span, token) == 0) {
        g_ld_source_clean++;
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.texture_hash_qpc += ld_profile_qpc() - begin;
#endif
        return 0;
    }
    const int result = rsx_source_compare(&entry->source, source, span);
    if (result >= 0) g_ld_source_compares++; else g_ld_source_fallbacks++;
#if defined(YZ_PERF_PROFILE)
    if (result >= 0) g_ld_profile.total.texture_hash_bytes += span;
    g_ld_profile.total.texture_hash_qpc += ld_profile_qpc() - begin;
#endif
    return result;
}
static u32 texture_srv_slot(const rsx_dsp_texture* t)
{
    const u32 remap = t->remap & 0xFFFF;
    static int refresh_enabled = -1;
    if (refresh_enabled < 0)
        refresh_enabled = getenv("RSX_NO_TEX_REFRESH") ? 0 : 1;

    for (u32 i = 0; i < g.n_textures; i++) {
        texcache_t* entry = &g.textures[i];
        if (entry->location != t->location || entry->offset != t->offset ||
            entry->format != t->format || entry->width != t->width ||
            entry->height != t->height || entry->pitch != t->pitch ||
            entry->remap != remap || entry->cubemap != t->cubemap ||
            entry->mipmaps != t->mipmaps || entry->dimension != t->dimension)
            continue;
        if (refresh_enabled && entry->tex &&
            entry->last_hash_frame != g_ld_frames) {
            int readable = 1;
            uint64_t source_epoch = entry->source_epoch;
            const int source_changed = ld_compare_source(entry, t, &source_epoch);
            const u64 hash = source_changed == 0 ? entry->content_hash
                : texture_content_hash(t, &readable);
            /* LD_TEX_RACE=1: hash the SAME source twice, back to back. The two
             * reads are microseconds apart, so they can only disagree if a
             * guest thread is writing this texture right now -- i.e. we are
             * decoding a half-written image. Pure observation; it changes
             * nothing. Counts per source so one torn texture is attributable. */
            { static int race = -1;
              if (race < 0) race = getenv("LD_TEX_RACE") ? 1 : 0;
              if (race && readable) {
                  int r2 = 0;
                  const u64 again = texture_content_hash(t, &r2);
                  static unsigned long long checks = 0, torn = 0;
                  checks++;
                  if (r2 && again != hash) torn++;
                  if ((checks % 256) == 0)
                      fprintf(stderr,
                              "[tex-race] src=%u:0x%08X %ux%u  torn %llu/%llu reads"
                              " (%.1f%%)\n",
                              t->location, t->offset, t->width, t->height,
                              (unsigned long long)torn,
                              (unsigned long long)checks,
                              100.0 * (double)torn / (double)checks);
              } }
            /* LD_FBDBG=1: account for the PS1 framebuffer specifically
             * (location 1, offset 0x400000, the 1024x512 fmt 0xE2 texture the
             * PS3 side composites). Its guest memory demonstrably fills up --
             * PS1 VRAM goes from 0 to 75,527 non-zero words -- yet
             * [tex-refresh] fires only 6 times in 90 seconds. Three different
             * things produce that, and they need different fixes: the texture
             * is rarely BOUND, the span is unreadable so `readable` is 0, or
             * the hash genuinely does not change. Count all three. */
            { static int fbd = -1;
              if (fbd < 0) fbd = getenv("LD_FBDBG") ? 1 : 0;
              if (fbd && t->location == 1u && t->offset == 0x400000u) {
                  static unsigned long long checks = 0, unread = 0, changed = 0;
                  checks++;
                  if (!readable) unread++;
                  if (readable && hash != entry->content_hash) changed++;
                  if ((checks % 64) == 0 || checks < 4)
                      fprintf(stderr, "[fbdbg] ps1fb checks=%llu unreadable=%llu"
                                      " changed=%llu span=%u %ux%u pitch=%u\n",
                              checks, unread, changed,
                              texture_source_span(t), t->width, t->height,
                              t->pitch);
              } }
            entry->last_hash_frame = g_ld_frames;
            if (readable && (source_changed > 0 || hash != entry->content_hash)) {
                rsx_source_snapshot source = ld_capture_source(t);
                ID3D12Resource* replacement =
                    decode_guest_texture_profiled(t, remap);
                if (replacement) {
                    ID3D12Resource* old = entry->tex;
                    entry->tex = replacement;
                    entry->content_hash = hash;
                    entry->source_epoch = source_epoch;
                    rsx_source_release(&entry->source, &g_ld_source_bytes);
                    entry->source = source;
                    source.bytes = NULL; source.size = 0;
                    write_texture_srv(i, entry);
                    retire_texture(old);
                    static u32 refresh_count = 0;
                    refresh_count++;
                    if (refresh_count <= 64 || (refresh_count & 255) == 0)
                        fprintf(stderr,
                                "[tex-refresh] n=%u frame=%u unit-src=%u:0x%08X "
                                "fmt=0x%02X %ux%u\n",
                                refresh_count, g_ld_frames, t->location, t->offset,
                                t->format, t->width, t->height);
                }
                rsx_source_release(&source, &g_ld_source_bytes);
            } else if (readable) {
                entry->source_epoch = source_epoch;
            }
        }
        entry->last_use_serial = ++g_ld_texture_use_serial;
        return entry->tex ? SRV_TEXTURE_BASE + i : SRV_WHITE;
    }

    u32 index;
    ID3D12Resource* evicted = NULL;
    const int cache_was_full = g.n_textures >= MAX_TEXTURES;
    if (!cache_was_full) {
        index = g.n_textures++;
    } else {
        /*
         * A full boot plus a010 binds far more than 128 distinct guest
         * texture descriptors. Returning SRV_WHITE after the fixed cache
         * filled made the recovered orphanage render as flat green/black
         * geometry. Reuse the least-recently-used descriptor while keeping
         * its resource alive until the open command list reaches a fence.
         * The shader-visible draw table already contains a descriptor copy,
         * so rewriting this CPU cache slot cannot alter earlier draws.
         */
        g_ld_texture_cache_full++;
        index = 0;
        for (u32 i = 1; i < g.n_textures; i++)
            if (g.textures[i].last_use_serial <
                g.textures[index].last_use_serial)
                index = i;
        evicted = g.textures[index].tex;
        g_ld_texture_cache_evictions++;
        if (g_ld_texture_cache_full <= 16 ||
            (g_ld_texture_cache_full & (g_ld_texture_cache_full - 1)) == 0)
            fprintf(stderr,
                    "[texture-cache] FULL cap=%u lru-evictions=%llu\n",
                    MAX_TEXTURES,
                    (unsigned long long)g_ld_texture_cache_evictions);
    }

    texcache_t replacement;
    memset(&replacement, 0, sizeof(replacement));
    replacement.location = t->location;
    replacement.offset = t->offset;
    replacement.format = t->format;
    replacement.width = t->width;
    replacement.height = t->height;
    replacement.pitch = t->pitch;
    replacement.remap = remap;
    replacement.cubemap = t->cubemap;
    replacement.mipmaps = t->mipmaps;
    replacement.dimension = t->dimension;
    replacement.source_epoch = ld_watch_source(t);
    replacement.source = ld_capture_source(t);
    replacement.last_hash_frame = g_ld_frames;
    replacement.last_use_serial = ++g_ld_texture_use_serial;
    {
        int readable = 0;
        replacement.content_hash = texture_content_hash(t, &readable);
    }
    replacement.tex = decode_guest_texture_profiled(t, remap);
    if (replacement.tex) {
        texcache_t* entry = &g.textures[index];
        if (evicted)
            retire_texture(evicted);
        rsx_source_release(&entry->source, &g_ld_source_bytes);
        *entry = replacement;
        write_texture_srv(index, entry);
    } else {
        rsx_source_release(&replacement.source, &g_ld_source_bytes);
        if (!cache_was_full)
            g.textures[index] = replacement;
        g_ld_texture_decode_fail++;
        if (g_ld_texture_decode_fail <= 16 ||
            (g_ld_texture_decode_fail & (g_ld_texture_decode_fail - 1)) == 0)
            fprintf(stderr,
                    "[texture-cache] decode failed n=%llu src=%u:0x%08X "
                    "fmt=0x%02X %ux%u pitch=%u\n",
                    (unsigned long long)g_ld_texture_decode_fail,
                    t->location, t->offset, t->format, t->width, t->height,
                    t->pitch);
    }
    return replacement.tex ? SRV_TEXTURE_BASE + index : SRV_WHITE;
}

typedef struct {
    DXGI_FORMAT dxgi;
    u32 bytes_per_texel;
    u32 component_bytes;
} vertex_texture_format_t;

static int vertex_texture_format(
    const rsx_dsp_vertex_texture* vt, vertex_texture_format_t* out)
{
    if (!vt || vt->dimension != 2 || vt->cubemap)
        return 0;
    vertex_texture_format_t format = {DXGI_FORMAT_UNKNOWN, 0, 0};
    switch (vt->format & TEX_FMT_BASE_MASK) {
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT:
        format.dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT;
        format.bytes_per_texel = 8;
        format.component_bytes = 2;
        break;
    case RSX_TEX_FMT_W32Z32Y32X32_FLOAT:
        format.dxgi = DXGI_FORMAT_R32G32B32A32_FLOAT;
        format.bytes_per_texel = 16;
        format.component_bytes = 4;
        break;
    case RSX_TEX_FMT_X32_FLOAT:
        format.dxgi = DXGI_FORMAT_R32_FLOAT;
        format.bytes_per_texel = 4;
        format.component_bytes = 4;
        break;
    case RSX_TEX_FMT_Y16X16_FLOAT:
        format.dxgi = DXGI_FORMAT_R16G16_FLOAT;
        format.bytes_per_texel = 4;
        format.component_bytes = 2;
        break;
    default:
        return 0;
    }
    if (out)
        *out = format;
    return 1;
}

static int vertex_texture_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("RSX_VTEX_TRACE") ? 1 : 0;
    return enabled;
}

static u32 vertex_texture_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        if (!vt.enabled) continue;
        g_ld_vtex_enabled++;
        if (vertex_texture_format(&vt, NULL)) {
            mask |= 1u << u;
        } else {
            g_ld_vtex_unsupported++;
            static u32 warned = 0;
            if (PS3_LOG_FIRST(warned, 16) && vertex_texture_trace_enabled())
                fprintf(stderr,
                        "[vtex] enabled but unsupported unit=%u off=0x%08X "
                        "fmt=0x%02X dim=%u cube=%u %ux%u pitch=%u ctl=0x%08X\n",
                        u, vt.offset, vt.format, vt.dimension, vt.cubemap,
                        vt.width, vt.height, vt.pitch, vt.control0);
        }
    }
    return mask;
}

static u64 vertex_texture_hash(const rsx_dsp_vertex_texture* vt,
                               const u8** out_src, u32* out_pitch)
{
    vertex_texture_format_t format;
    if (!vertex_texture_format(vt, &format) || !vt->width || !vt->height ||
        vt->width > 4096 || vt->height > 4096)
        return 0;
    const u64 row_bytes64 = (u64)vt->width * format.bytes_per_texel;
    const u64 pitch64 = vt->pitch ? vt->pitch : row_bytes64;
    const u64 span64 = pitch64 * vt->height;
    if (row_bytes64 > UINT32_MAX || pitch64 < row_bytes64 ||
        pitch64 > UINT32_MAX || span64 > UINT32_MAX)
        return 0;
    const u32 pitch = (u32)pitch64;
    const u32 span = (u32)span64;
    const u8* src = span ? guest_ptr(vt->location, vt->offset, span) : NULL;
    if (out_src) *out_src = src;
    if (out_pitch) *out_pitch = pitch;
    return src ? fnv1a(src, span, 1469598103934665603ull) : 0;
}

static ID3D12Resource* decode_vertex_texture(
    const rsx_dsp_vertex_texture* vt, u64* out_hash)
{
    vertex_texture_format_t format;
    if (!vertex_texture_format(vt, &format) || !vt->width || !vt->height ||
        vt->width > 4096 || vt->height > 4096)
        return NULL;
    const u8* src = NULL;
    u32 pitch = 0;
    const u64 hash = vertex_texture_hash(vt, &src, &pitch);
    if (!src) return NULL;
    const u32 row_bytes = vt->width * format.bytes_per_texel;
    if (pitch < row_bytes)
        return NULL;
    u8* staging = (u8*)malloc((size_t)row_bytes * vt->height);
    if (!staging) return NULL;
    for (u32 y = 0; y < vt->height; y++) {
        const u8* srow = src + (size_t)y * pitch;
        u8* drow = staging + (size_t)y * row_bytes;
        const u32 components = row_bytes / format.component_bytes;
        for (u32 c = 0; c < components; c++) {
            const u8* source =
                srow + (size_t)c * format.component_bytes;
            u8* destination =
                drow + (size_t)c * format.component_bytes;
            for (u32 byte = 0; byte < format.component_bytes; byte++)
                destination[byte] =
                    source[format.component_bytes - 1u - byte];
        }
    }
    tex_level_t level = {
        vt->width, vt->height, staging, row_bytes, vt->height
    };
    ID3D12Resource* tex = create_texture_mipped(format.dxgi, &level, 1);
    free(staging);
    if (tex) {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        rsx_rec_ResourceBarrier(g.frame, 1, &b);
        if (out_hash) *out_hash = hash;
    }
    return tex;
}

static void write_vertex_texture_srv(u32 index, ID3D12Resource* tex)
{
    rsx_dsp_vertex_texture descriptor = {0};
    vertex_texture_format_t format;
    if (index >= g.n_vtex)
        return;
    descriptor.format = g.vtex[index].format;
    descriptor.dimension = 2;
    if (!vertex_texture_format(&descriptor, &format))
        return;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = format.dxgi;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g.dev->lpVtbl->CreateShaderResourceView(
        g.dev, tex, &sd, srv_cpu(SRV_VTEX_BASE + index));
}

static u32 vertex_texture_srv_slot(const rsx_dsp_vertex_texture* vt)
{
    for (u32 i = 0; i < g.n_vtex; i++) {
        vtexcache_t* e = &g.vtex[i];
        if (e->location != vt->location || e->offset != vt->offset ||
            e->format != vt->format || e->width != vt->width ||
            e->height != vt->height || e->pitch != vt->pitch)
            continue;
        if (e->tex && e->last_hash_frame != g_ld_frames) {
            const u64 hash = vertex_texture_hash(vt, NULL, NULL);
            e->last_hash_frame = g_ld_frames;
            if (hash && hash != e->content_hash) {
                u64 replacement_hash = 0;
                ID3D12Resource* replacement =
                    decode_vertex_texture(vt, &replacement_hash);
                if (replacement) {
                    ID3D12Resource* old = e->tex;
                    e->tex = replacement;
                    e->content_hash = replacement_hash;
                    write_vertex_texture_srv(i, e->tex);
                    retire_texture(old);
                    g_ld_vtex_refreshes++;
                }
            }
        }
        return e->tex ? SRV_VTEX_BASE + i : SRV_WHITE;
    }
    if (g.n_vtex >= MAX_VTEX) return SRV_WHITE;
    const u32 index = g.n_vtex++;
    vtexcache_t* e = &g.vtex[index];
    memset(e, 0, sizeof(*e));
    e->location = vt->location;
    e->offset = vt->offset;
    e->format = vt->format;
    e->width = vt->width;
    e->height = vt->height;
    e->pitch = vt->pitch;
    e->last_hash_frame = g_ld_frames;
    e->tex = decode_vertex_texture(vt, &e->content_hash);
    if (e->tex) {
        write_vertex_texture_srv(index, e->tex);
        g_ld_vtex_uploads++;
        if (vertex_texture_trace_enabled())
            fprintf(stderr,
                    "[vtex] upload unit-data %u:0x%08X fmt=0x%02X %ux%u pitch=%u\n",
                    vt->location, vt->offset, vt->format,
                    vt->width, vt->height, vt->pitch);
    }
    return e->tex ? SRV_VTEX_BASE + index : SRV_WHITE;
}

/* ---------------------------------------------------------------------------
 * surfaces (color RTs keyed by location/offset), rendered into then presented
 * -----------------------------------------------------------------------*/
/* fp constants are stored as raw bits; the shader reads them as floats. */
static float ld_c2f(u32 bits) { float f; memcpy(&f, &bits, sizeof f); return f; }
static void surface_views_invalidate(ID3D12Resource* source);

static u32 surface_get(u32 location, u32 offset, u32 want_w, u32 want_h,
                       DXGI_FORMAT want_fmt)
{
    if (!want_w) want_w = g.width;
    if (!want_h) want_h = g.height;
    /* Key on the DIMENSIONS as well as location/offset.
     *
     * Keying on location/offset alone meant a redeclaration of the same offset
     * at a different size STOLE the slot and recreated the D3D12 resource,
     * throwing the old contents away. ps1_netemu renders two passes at two
     * sizes -- the PS1 composite at 720x512 and the PS3 output at 1280x720 --
     * and it alternates between them, so that path fired 9,573 times in a
     * 90-second run:
     *
     *     [surfsz] live surface 0x0 redeclared 720x512 -> 1280x720 (content dropped)
     *     [surfsz] live surface 0x1401C00 redeclared 1280x720 -> 720x512 (content dropped)
     *
     * Every frame of both passes was being destroyed by the other. Giving each
     * (offset, size) its own surface lets both keep their contents.
     *
     * ponytail: two surfaces over the same guest memory do not alias, so a pass
     * that renders at one size and samples at the other will not see the
     * other's pixels. That is strictly better than the old behaviour, which saw
     * NOBODY's pixels, and it is the smaller change. If a title ever needs true
     * aliasing, the fix is one surface plus a copy on redeclare -- not going
     * back to destroying content. */
    u32 slot = MAX_SURFACES;
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].location == location && g.surfaces[i].offset == offset &&
            g.surfaces[i].w == want_w && g.surfaces[i].h == want_h &&
            g.surfaces[i].fmt == want_fmt) {
            /* A reduced target drawn with a logic op is a glyph scratch
             * (ld_logic_target_scaled): recreated once at the display scale. */
            if (!(g_ld_logic_target && g.surfaces[i].scale != ld_logic_target_scale() && ld_logic_target_scaled()))
                return i;
            slot = i;
            break;
        }
    /* Never destroy a usable render target because a malformed live command
     * briefly decoded a guest pointer as clip dimensions.  The known-good
     * orphanage stream never exceeds 1280x1024; D3D12 rejects the observed
     * 1280x16452 declaration and the old path then dereferenced a null
     * resource.  Preserve the prior surface so diagnostics can capture the
     * actual world pass while the bad command link is isolated. */
    if (want_w > 8192u || want_h > 8192u || !ld_physical(want_w) || !ld_physical(want_h)) {
        static u32 invalid_surface_logs = 0;
        if (PS3_LOG_FIRST(invalid_surface_logs, 32u))
            fprintf(stderr,
                    "[surface-guard] rejected implausible surface 0x%X "
                    "%ux%u; preserving slot=%s\n",
                    offset, want_w, want_h,
                    slot < MAX_SURFACES ? "existing" : "none");
        return slot < MAX_SURFACES ? slot : LD_INVALID_SURFACE;
    }
    if (slot == MAX_SURFACES && g.n_surfaces >= MAX_SURFACES) {
        /* Out of slots is now the only way a surface can be lost, so say so
         * once rather than silently returning an invalid handle. */
        static u32 full_logs = 0;
        if (PS3_LOG_FIRST(full_logs, 8))
            fprintf(stderr, "[surfsz] surface table full (%u); dropping "
                            "%u:0x%X %ux%u\n",
                    (unsigned)MAX_SURFACES, location, offset, want_w, want_h);
        return LD_INVALID_SURFACE;
    }
    const int rescaled = slot < MAX_SURFACES;
    if (!rescaled) slot = g.n_surfaces;
    { static u32 new_logs = 0;
      if (PS3_LOG_FIRST(new_logs, 24))
          fprintf(stderr, "[surfsz] new live surface %u:0x%X %ux%u (slot %u)\n",
                  location, offset, want_w, want_h, slot); }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    const float want_scale = g_ld_logic_target && ld_logic_target_scaled()
        ? ld_logic_target_scale() : ld_target_scale(want_w, want_h);
    const rsx_resolution_domain want_domain = { want_scale };
    rd.Width = rsx_resolution_extent(want_domain, want_w);
    rd.Height = rsx_resolution_extent(want_domain, want_h); rd.DepthOrArraySize = 1;
    /* UNORM for ordinary drawing/sampling, UINT for bitwise logic operations.
     * Both views address the same bytes; no format-conversion copy is needed. */
    rd.MipLevels = 1;
    rd.Format = want_fmt == DXGI_FORMAT_R8G8B8A8_UNORM
        ? DXGI_FORMAT_R8G8B8A8_TYPELESS : want_fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0}; cv.Format = want_fmt;
    surface_t* s = &g.surfaces[slot];
    ID3D12Resource* replacement = NULL;
    const HRESULT create_hr = g.dev->lpVtbl->CreateCommittedResource(
        g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
        &IID_ID3D12Resource, (void**)&replacement);
    if (FAILED(create_hr)) {
        static u32 surface_fail_logs = 0;
        if (PS3_LOG_FIRST(surface_fail_logs, 32)) {
            const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
            fprintf(stderr,
                    "[surface-fail] color %u:0x%X %ux%u hr=0x%08lX "
                    "removed=0x%08lX slot=%u count=%u\n",
                    location, offset, want_w, want_h,
                    (unsigned long)create_hr, (unsigned long)removed,
                    slot, g.n_surfaces);
        }
        /* A resize failure must not poison an existing usable slot. */
        return slot < g.n_surfaces && s->tex ? slot : LD_INVALID_SURFACE;
    }
    /* Draws already recorded in the open command list can still reference
     * the old render target.  Release it only after ld_flush fences that list,
     * just like dynamic texture and zeta replacements. */
    if (s->tex) {
        surface_views_invalidate(s->tex);
        retire_texture(s->tex);
    }
    if (s->fb_tex) {                 /* sized for the old resource */
        retire_texture(s->fb_tex);
        s->fb_tex = NULL;
    }
    for (u32 j = 0; j < LD_SWIN_PER; j++)   /* windows are sized and typed for the old resource */
        if (s->swin[j].tex) { retire_texture(s->swin[j].tex); s->swin[j].tex = NULL; }
    if (s->lview) { retire_texture(s->lview); s->lview = NULL; }
    s->tex = replacement;
    s->location = location; s->offset = offset; s->w = want_w; s->h = want_h;
    s->scale = want_scale;
    s->fmt = want_fmt;
    fprintf(stderr, "[resolution-image] color slot=%u logical=%ux%u physical=%llux%u scale=%.1f\n",
        slot, want_w, want_h, (unsigned long long)rd.Width, rd.Height, want_scale);
#if !defined(YZ_PERF_CLEAN)
    s->resource_serial = ++g_ld_surface_resource_serial;
#endif
    ld_surface_note_write(slot, LD_SURFACE_WRITE_CREATE);
    /* RTVs for surfaces live above the swap-chain backbuffer RTVs */
    D3D12_RENDER_TARGET_VIEW_DESC rv = {0};
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    rv.Format = want_fmt;
    rsx_rec_drain();
    if (g.frame) g.frame->bindings.valid &= ~8u;
    g.dev->lpVtbl->CreateRenderTargetView(g.dev, s->tex,
        &rv, rtv_handle(LD_SWAP_BUFFERS + slot));
    if (want_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
        rv.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        rsx_rec_drain();
        if (g.frame) g.frame->bindings.valid &= ~8u;
        g.dev->lpVtbl->CreateRenderTargetView(g.dev, s->tex,
            &rv, rtv_handle(LD_SWAP_BUFFERS + MAX_SURFACES + slot));
    }
    srv_write(SRV_SURFACE_BASE + slot, s->tex);
    if (rescaled && g.frame) {      /* the guest cleared the old resource; alpha must start at 0 */
        static const float transparent[4] = {0, 0, 0, 0};
        rsx_rec_ClearRenderTargetView(g.frame, rtv_handle(LD_SWAP_BUFFERS + slot), transparent, 0, NULL);
    }
    if (slot == g.n_surfaces) g.n_surfaces++;
    return slot;
}

/* A texture smaller than the colour surface it aliases reads the surface's memory texel for
 * sample, i.e. the top-left tex_w x tex_h of the surface, not the whole surface scaled into it. a title's scene
 * renders into 0x01FB0000 with SURFACE_FORMAT 0xA0B5148 (A8R8G8B8, antialias 5 = 2x2 samples), raw clip 2560x1440,
 * pitch 10240, viewport 1280x720. rsx_dsp_get_surface multiplies the clip by the sample grid (PR #281), so
 * the host surface is 5120x2880, but the game draws only its viewport, 2560x1440 samples, which is all the guest
 * memory holds (pitch 10240 = 2560 x 4 bytes; tile 7 is 0xE10000 = 10240 x 1440 bytes). The downsample then binds
 * 0x01FB0000 as a 2560x1440 A8R8G8B8 linear texture; bound to the whole 5120x2880 surface, its normalised
 * coordinates took the scene from the top-left quarter only, and every later pass (640x360 bloom chain, final
 * composite) showed the scene in a corner. On the RSX texel (x,y) of a linear texture is the byte at
 * y*pitch + x*bpp, which for an equal pitch and texel size is sample (x,y) of the surface. A 2D SRV cannot select a
 * sub-rectangle, so, the top-left w x h of the surface is copied into a w x h
 * texture of the surface's format (kept per surface and size, recopied only after the surface was written), and
 * clamp, filtering and texel size then see the texture the guest declared.
 * Returns the SRV slot, or -1 to bind the whole surface as before: equal size, a texture larger than the surface,
 * not a linear 2D texture, unknown or different texel size, or a texture pitch that differs from the surface's
 * guest pitch (the texture then does not walk the surface's rows; the old behaviour is kept, unproven either way).
 * LD_SWIN=0 = old behaviour. LD_ALIAS_DBG=1 logs each sub-rect mapping ([alias-subrect]). */
static int ld_swin_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_SWIN"); on = (e && e[0] == '0') ? 0 : 1;
                  fprintf(stderr, "[swin] %s (LD_SWIN)\n", on ? "on" : "off"); }
    return on;
}
/* Guest bytes per texel of a texture format (modifier bits ignored); 0 = compressed or not listed. */
static u32 ld_tex_texel_bytes(u32 format)
{
    switch (format & TEX_FMT_BASE_MASK) {
    case RSX_TEX_FMT_B8: return 1u;
    case RSX_TEX_FMT_A1R5G5B5: case RSX_TEX_FMT_A4R4G4B4: case RSX_TEX_FMT_R5G6B5: case RSX_TEX_FMT_G8B8: return 2u;
    case RSX_TEX_FMT_A8R8G8B8: case RSX_TEX_FMT_DEPTH24_D8: case RSX_TEX_FMT_X32_FLOAT: case RSX_TEX_FMT_Y16X16_FLOAT:
        return 4u;
    case RSX_TEX_FMT_W16Z16Y16X16_FLOAT: return 8u;
    case RSX_TEX_FMT_W32Z32Y32X32_FLOAT: return 16u;
    default: return 0u;
    }
}
static u64 g_ld_swin_copies;
static int ld_box_view_refresh(u32 si, ID3D12Resource* dst, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                               u32 w, u32 h, float ratio);
#include "rsx_surface_views.inl"
static int surface_window_legacy(u32 si, const rsx_dsp_texture* t, int dbg)
{
    surface_t* s = &g.surfaces[si];
    const u32 w = t->width, h = t->height;
    if (!ld_swin_on() || !s->tex || !w || !h) return -1;
    if (w == s->w && h == s->h) return -1;           /* exact-size alias: the whole surface, unchanged */
    if (w > s->w || h > s->h) return -1;
    const u32 sb = s->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;   /* the surface formats live-draw creates */
    const u32 tb = ld_tex_texel_bytes(t->format);
    if (t->dimension != 2u || t->cubemap || !(t->format & TEX_FMT_LINEAR) || !s->pitch ||
        t->pitch != s->pitch || tb != sb) {
        if (dbg) {
            static u32 n_keep;
            if (n_keep++ < 24)
                fprintf(stderr, "[alias-subrect] keep whole surface: tex %u:0x%08X fmt=0x%02X %ux%u pitch=%u "
                        "(%u B/texel) vs surface[%u] %ux%u pitch=%u (%u B/texel)\n", t->location, t->offset,
                        t->format, w, h, t->pitch, tb, si, s->w, s->h, s->pitch, sb);
        }
        return -1;
    }
    static u32 use_clock;
    u32 k = LD_SWIN_PER;
    for (u32 j = 0; j < LD_SWIN_PER; j++)
        if (s->swin[j].tex && s->swin[j].src == s->tex && s->swin[j].w == w && s->swin[j].h == h) { k = j; break; }
    if (k == LD_SWIN_PER) {
        k = 0;
        for (u32 j = 1; j < LD_SWIN_PER; j++)
            if (!s->swin[j].tex || (s->swin[k].tex && s->swin[j].used < s->swin[k].used)) k = j;
        if (s->swin[k].tex) retire_texture(s->swin[k].tex);   /* the open command list may still read it */
        s->swin[k].tex = NULL;
        D3D12_RESOURCE_DESC rd;
        s->tex->lpVtbl->GetDesc(s->tex, &rd);              /* the surface's format and sample count */
        rd.Width = ld_surface_physical(s, w); rd.Height = ld_surface_physical(s, h); rd.Alignment = 0;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource* wt = NULL;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&wt))) {
            static u32 n_fail;
            if (n_fail++ < 8)
                fprintf(stderr, "[swin] create failed %u:0x%X %ux%u: whole surface bound\n", s->location, s->offset,
                        w, h);
            return -1;
        }
        s->swin[k].tex = wt; s->swin[k].src = s->tex; s->swin[k].w = w; s->swin[k].h = h;
        g_surface_views.stats.legacy_allocations++;
        s->swin[k].wgen = s->swin_wgen - 1u;
        srv_write(SRV_SURF_WIN_BASE + si * LD_SWIN_PER + k, wt);
        static u32 n_log;
        if (n_log++ < 16)
            fprintf(stderr, "[swin] %u:0x%X surface %ux%u pitch=%u -> window %ux%u (slot %u.%u)\n",
                    s->location, s->offset, s->w, s->h, s->pitch, w, h, si, k);
    }
    if (dbg) {
        static u32 n_hit;
        if (n_hit++ < 24)
            fprintf(stderr, "[alias-subrect] tex %u:0x%08X fmt=0x%02X %ux%u pitch=%u -> surface[%u] %ux%u "
                    "texels [0,%u)x[0,%u) (window %u)\n", t->location, t->offset, t->format, w, h, t->pitch, si,
                    s->w, s->h, w, h, k);
    }
    s->swin[k].used = ++use_clock;
    if (s->swin[k].wgen != s->swin_wgen) {
        D3D12_RESOURCE_BARRIER b[2] = {0};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition.pResource = s->tex;
        b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[1].Transition.pResource = s->swin[k].tex;
        b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        rsx_rec_ResourceBarrier(g.frame, 2, b);
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s->swin[k].tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = s->tex;         src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box = { 0, 0, 0, ld_surface_physical(s, w), ld_surface_physical(s, h), 1 };
        rsx_rec_CopyTextureRegion(g.frame, &dst, 0, 0, 0, &src, &box);
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        rsx_rec_ResourceBarrier(g.frame, 2, b);
        s->swin[k].wgen = s->swin_wgen;
        g_surface_views.stats.legacy_copies++;
        if ((++g_ld_swin_copies & 0x3FFFu) == 1u)
            fprintf(stderr, "[swin] copies=%llu frame=%u\n", (unsigned long long)g_ld_swin_copies, g_ld_frames);
    }
    return (int)(SRV_SURF_WIN_BASE + si * LD_SWIN_PER + k);
}
static int surface_window(u32 si, const rsx_dsp_texture* t, int dbg)
{
    return surface_views_enabled() ? surface_view(si,t) : surface_window_legacy(si,t,dbg);
}

/* A draw that samples its own render target. RSX reads and writes the same
 * surface in one pass (GH3's post pass reads the scene at 0x200000 while
 * drawing into 0x200000); D3D12 cannot, and the lookup used to fall back to
 * guest VRAM -- empty, so the pass painted the scene black. Snapshot the
 * target into a per-surface copy and sample that. Returns the SRV slot, or
 * SRV_WHITE if the copy cannot be made. */
static u32 surface_feedback_srv(u32 slot)
{
    surface_t* s = &g.surfaces[slot];
    if (!s->tex) return SRV_WHITE;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    for (int k = 0; k < 2; k++) {
        b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    int fresh = 0;
    if (!s->fb_tex) {
        D3D12_RESOURCE_DESC rd;
        s->tex->lpVtbl->GetDesc(s->tex, &rd);
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
                g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void**)&s->fb_tex))) {
            s->fb_tex = NULL;
            return SRV_WHITE;
        }
        srv_write(SRV_FEEDBACK_BASE + slot, s->fb_tex);
        fresh = 1;
    }
    b[0].Transition.pResource = s->tex;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = s->fb_tex;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    rsx_rec_ResourceBarrier(g.frame, fresh ? 1 : 2, b);
    rsx_rec_CopyResource(g.frame, s->fb_tex, s->tex);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    rsx_rec_ResourceBarrier(g.frame, 2, b);
    return SRV_FEEDBACK_BASE + slot;
}

/* A reduced pass (ld_target_scale) reading a full-scale surface sees it at its
 * guest size: each logical texel is the box average of the physical texels it
 * covers, which is what RSX's taps read. Sampled directly, a bilinear tap
 * covers 2x2 of the 3x3 texels of a logical texel at scale 3, so thin bright
 * lines -- the Ignition rim, a blade's edge -- are kept or skipped by their
 * position and the bloom built from them is dotted and shimmers. Kept like a
 * surface window: rebuilt after the surface is written, sampled in
 * PIXEL_SHADER_RESOURCE. Returns the SRV slot, or -1 to sample the surface. */
static struct {
    ID3D12RootSignature* root;
    ID3D12PipelineState* pso[4];
    DXGI_FORMAT fmt[4];
} g_lview;

static ID3D12PipelineState* ld_logical_view_pso(DXGI_FORMAT fmt)
{
    u32 n = 0;
    for (; n < 4 && g_lview.pso[n]; n++)
        if (g_lview.fmt[n] == fmt) return g_lview.pso[n];
    if (n == 4) return NULL;
    /* Overlap-weighted box over [p*s, (p+1)*s) per axis; s may be fractional. */
    static const char src[] =
        "Texture2D<float4> t : register(t0);\n"
        "cbuffer c : register(b0) { float s; int2 size; };\n"
        "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
        "    float2 q = float2((id << 1) & 2, id & 2);\n"
        "    return float4(q * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n"
        "float4 ps(float4 p : SV_Position) : SV_Target {\n"
        "    float2 lo = floor(p.xy) * s, hi = lo + s;\n"
        "    int2 a = int2(floor(lo)), b = min(int2(ceil(hi)), size);\n"
        "    float4 sum = 0; float wsum = 0;\n"
        "    for (int y = a.y; y < b.y; y++) {\n"
        "        float wy = min(hi.y, y + 1) - max(lo.y, y);\n"
        "        for (int x = a.x; x < b.x; x++) {\n"
        "            float w = wy * (min(hi.x, x + 1) - max(lo.x, x));\n"
        "            sum += w * t.Load(int3(x, y, 0)); wsum += w;\n"
        "        }\n"
        "    }\n"
        "    return wsum > 0 ? sum / wsum : 0;\n"
        "}\n";
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, sizeof(src) - 1u, "ld_logical_view", NULL, NULL,
                            "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
    if (err) { err->lpVtbl->Release(err); err = NULL; }
    if (SUCCEEDED(hr))
        hr = D3DCompile(src, sizeof(src) - 1u, "ld_logical_view", NULL, NULL,
                        "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
    if (err) {
        fprintf(stderr, "[logical-view] %s\n", (const char*)err->lpVtbl->GetBufferPointer(err));
        err->lpVtbl->Release(err);
    }
    if (SUCCEEDED(hr) && !g_lview.root) {
        D3D12_DESCRIPTOR_RANGE range = {0};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER rp[2] = {0};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants.Num32BitValues = 3;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &range;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {0};
        rsd.NumParameters = 2; rsd.pParameters = rp;
        ID3DBlob* sig = NULL;
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (err) err->lpVtbl->Release(err);
        if (SUCCEEDED(hr)) {
            hr = g.dev->lpVtbl->CreateRootSignature(g.dev, 0, sig->lpVtbl->GetBufferPointer(sig),
                    sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&g_lview.root);
            sig->lpVtbl->Release(sig);
        }
    }
    ID3D12PipelineState* pso = NULL;
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = g_lview.root;
        pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb);
        pd.VS.BytecodeLength = vsb->lpVtbl->GetBufferSize(vsb);
        pd.PS.pShaderBytecode = psb->lpVtbl->GetBufferPointer(psb);
        pd.PS.BytecodeLength = psb->lpVtbl->GetBufferSize(psb);
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = fmt;
        pd.SampleDesc.Count = 1;
        hr = g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd,
                &IID_ID3D12PipelineState, (void**)&pso);
    }
    if (vsb) vsb->lpVtbl->Release(vsb);
    if (psb) psb->lpVtbl->Release(psb);
    if (FAILED(hr) || !pso) {
        static u32 fail_logs = 0;
        if (PS3_LOG_FIRST(fail_logs, 8))
            fprintf(stderr, "[logical-view] pipeline for format %u failed hr=0x%08lX\n",
                    (unsigned)fmt, (unsigned long)hr);
        return NULL;
    }
    g_lview.pso[n] = pso; g_lview.fmt[n] = fmt;
    return pso;
}

static int surface_logical_view(u32 si)
{
    surface_t* s = &g.surfaces[si];
    if (!s->tex || s->scale == 1.0f) return -1;
    ID3D12PipelineState* pso = ld_logical_view_pso(s->fmt);
    if (!pso) return -1;
    if (!s->lview || s->lview_src != s->tex) {
        if (s->lview) retire_texture(s->lview);
        s->lview = NULL;
        D3D12_RESOURCE_DESC rd;
        s->tex->lpVtbl->GetDesc(s->tex, &rd);    /* the surface's format */
        rd.Width = s->w; rd.Height = s->h; rd.Alignment = 0;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE cv = {0}; cv.Format = s->fmt;
        ID3D12Resource* lv = NULL;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, &IID_ID3D12Resource, (void**)&lv)))
            return -1;
        D3D12_RENDER_TARGET_VIEW_DESC rv = {0};
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rv.Format = s->fmt;
        rsx_rec_drain();
        g.dev->lpVtbl->CreateRenderTargetView(g.dev, lv, &rv, rtv_handle(RTV_LOGICAL_VIEW_BASE + si));
        srv_write(SRV_LOGICAL_VIEW_BASE + si, lv);
        s->lview = lv; s->lview_src = s->tex;
        s->lview_wgen = s->swin_wgen - 1u;
        fprintf(stderr, "[logical-view] surface %u 0x%X %ux%u (scale %.1f) for reduced passes\n",
                si, s->offset, s->w, s->h, s->scale);
    }
    if (s->lview_wgen != s->swin_wgen) {
        u32 slots[SRV_TABLE_SIZE];
        for (u32 i = 0; i < SRV_TABLE_SIZE; i++) slots[i] = SRV_WHITE;
        slots[0] = SRV_SURFACE_BASE + si;
        const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots);
        if (!table.ptr) return -1;
        D3D12_RESOURCE_BARRIER b[2] = {0};
        for (int k = 0; k < 2; k++) {
            b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        b[0].Transition.pResource = s->tex;
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b[1].Transition.pResource = s->lview;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        rsx_rec_ResourceBarrier(g.frame, 2, b);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(RTV_LOGICAL_VIEW_BASE + si);
        rsx_rec_targets(g.frame, 1, &rtv, NULL);
        ID3D12DescriptorHeap* heaps[] = {g.frame->srv_heap, g.frame->smp_heap};
        rsx_rec_heaps(g.frame, heaps);
        rsx_rec_pso(g.frame, pso);
        rsx_rec_root(g.frame, g_lview.root);
        D3D12_RESOURCE_DESC pd;
        s->tex->lpVtbl->GetDesc(s->tex, &pd);
        u32 c[3];
        memcpy(&c[0], &s->scale, 4);
        c[1] = (u32)pd.Width; c[2] = pd.Height;
        rsx_rec_SetGraphicsRoot32BitConstants(g.frame, 0, 3, c, 0);
        rsx_rec_SetGraphicsRootDescriptorTable(g.frame, 1, table);
        const D3D12_VIEWPORT vp = {0, 0, (float)s->w, (float)s->h, 0.0f, 1.0f};
        const D3D12_RECT sc = {0, 0, (LONG)s->w, (LONG)s->h};
        rsx_rec_viewport(g.frame, &vp);
        rsx_rec_scissor(g.frame, &sc);
        rsx_rec_topology(g.frame, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        rsx_rec_DrawInstanced(g.frame, 3, 1, 0, 0);
        for (int k = 0; k < 2; k++) {
            const D3D12_RESOURCE_STATES before = b[k].Transition.StateBefore;
            b[k].Transition.StateBefore = b[k].Transition.StateAfter;
            b[k].Transition.StateAfter = before;
        }
        rsx_rec_ResourceBarrier(g.frame, 2, b);
        s->lview_wgen = s->swin_wgen;
    }
    return (int)(SRV_LOGICAL_VIEW_BASE + si);
}

/* Fill `dst` (w x h physical, render target `rtv`) with the top-left of surface
 * `si` reduced by `ratio` per axis with the logical view's box filter: a glyph
 * scratch drawn above the display scale (ld_logic_target_scale), seen by the
 * composite at the display scale. Returns 0 when the pipeline is missing. */
static int ld_box_view_refresh(u32 si, ID3D12Resource* dst, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                               u32 w, u32 h, float ratio)
{
    surface_t* s = &g.surfaces[si];
    ID3D12PipelineState* pso = ld_logical_view_pso(s->fmt);
    if (!pso || !s->tex) return 0;
    u32 slots[SRV_TABLE_SIZE];
    for (u32 i = 0; i < SRV_TABLE_SIZE; i++) slots[i] = SRV_WHITE;
    slots[0] = SRV_SURFACE_BASE + si;
    const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots);
    if (!table.ptr) return 0;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    for (int k = 0; k < 2; k++) {
        b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    b[0].Transition.pResource = s->tex;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.pResource = dst;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rsx_rec_ResourceBarrier(g.frame, 2, b);
    rsx_rec_targets(g.frame, 1, &rtv, NULL);
    ID3D12DescriptorHeap* heaps[] = {g.frame->srv_heap, g.frame->smp_heap};
    rsx_rec_heaps(g.frame, heaps);
    rsx_rec_pso(g.frame, pso);
    rsx_rec_root(g.frame, g_lview.root);
    D3D12_RESOURCE_DESC sd;
    s->tex->lpVtbl->GetDesc(s->tex, &sd);
    u32 c[3];
    memcpy(&c[0], &ratio, 4);
    c[1] = (u32)sd.Width; c[2] = sd.Height;
    rsx_rec_SetGraphicsRoot32BitConstants(g.frame, 0, 3, c, 0);
    rsx_rec_SetGraphicsRootDescriptorTable(g.frame, 1, table);
    const D3D12_VIEWPORT vp = {0, 0, (float)w, (float)h, 0.0f, 1.0f};
    const D3D12_RECT sc = {0, 0, (LONG)w, (LONG)h};
    rsx_rec_viewport(g.frame, &vp);
    rsx_rec_scissor(g.frame, &sc);
    rsx_rec_topology(g.frame, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    rsx_rec_DrawInstanced(g.frame, 3, 1, 0, 0);
    for (int k = 0; k < 2; k++) {
        const D3D12_RESOURCE_STATES before = b[k].Transition.StateBefore;
        b[k].Transition.StateBefore = b[k].Transition.StateAfter;
        b[k].Transition.StateAfter = before;
    }
    rsx_rec_ResourceBarrier(g.frame, 2, b);
    return 1;
}

/* Find the live surface whose guest rows (at `pitch`) contain byte `abs`, with
 * room for a w x h rect there. Returns the slot and the rect's origin. */
static int surface_containing(u32 location, u32 abs, u32 pitch, u32 w, u32 h,
                              u32* ox, u32* oy)
{
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const surface_t* s = &g.surfaces[i];
        if (!s->tex || s->location != location || abs < s->offset) continue;
        const u32 d = abs - s->offset, y = d / pitch, x = (d % pitch) / 4u;
        if (x + w <= s->w && y + h <= s->h) { *ox = x; *oy = y; return (int)i; }
    }
    return -1;
}

/* NV3089 scaled-image blit whose SOURCE is a live surface: the pixels exist
 * only on the GPU, so the guest-memory copy in cellGcmSys moves black. GH3
 * copies its 1040x592 scene out to 0x1651500 every frame, runs its post pass
 * sampling that copy, and copies it back -- all black, so attract mode showed
 * only its overlay. Do the copy on the GPU, creating a live surface for the
 * destination so later texture lookups at that address find it. 1:1 only.
 * Returns 1 when handled, <= 0 when not (reason codes in ld_blit_gpu). */
static FILE* s_csv_file;       /* defined with live_draw_csv_emit */
static int   s_csv_gate_open;

static int ld_blit_gpu(u32 src_loc, u32 src_abs, u32 src_pitch,
                       u32 dst_loc, u32 dst_abs, u32 dst_pitch, u32 w, u32 h);
int rsx_live_draw_blit(u32 src_loc, u32 src_abs, u32 src_pitch,
                       u32 dst_loc, u32 dst_abs, u32 dst_pitch, u32 w, u32 h)
{
    const int r = ld_blit_gpu(src_loc, src_abs, src_pitch, dst_loc, dst_abs, dst_pitch, w, h);
    if (s_csv_file && s_csv_gate_open) {
        fprintf(s_csv_file, "#blit,%u,%u:0x%X/%u,%u:0x%X/%u,%ux%u,gpu=%d\n", g_ld_frames,
                src_loc, src_abs, src_pitch, dst_loc, dst_abs, dst_pitch, w, h, r);
        fflush(s_csv_file);
    }
    /* LD_BLIT_DUMP=<dir>: while the CSV gate is open, read back the first four
     * blits' destination surfaces right after the copy (one frame's worth). */
    { static const char* dir = (const char*)-1; static u32 n;
      if (dir == (const char*)-1) dir = getenv("LD_BLIT_DUMP");
      if (!(s_csv_file && s_csv_gate_open)) n = 0;
      else if (dir && r == 1 && n < 4) {
          u32 dx, dy;
          const int di = surface_containing(dst_loc, dst_abs, dst_pitch, w, h, &dx, &dy);
          if (di >= 0) {
              char path[MAX_PATH * 2];
              snprintf(path, sizeof(path), "%s\\blit_f%06u_%u_dst%08X.ppm", dir, g_ld_frames, n,
                       g.surfaces[di].offset);
              ld_dump_surface_ppm(path, &g.surfaces[di]);
          }
          n++;
      } }
    return r;
}

/* A SCALED NV3089 blit out of a live surface cannot be a CopyTextureRegion.
 * The one that matters is a supersampled scene resolved into a display buffer
 * (Tornado Outbreak renders 2x2 and downsamples into its scanout in 512x512
 * tiles): the guest-memory copy moves black and the flip presented whatever
 * surface happened to be current. When such a blit lands on a display buffer's
 * origin, remember the source surface; the flip presents it (at the host
 * surface's size) when the buffer has no live surface of its own.
 * ponytail: scanout alias, not a real scaled copy -- a title that samples the
 * resolved buffer as a texture still sees black; add a draw-based scaled blit
 * then. */
/* A k x k box-filter downsample from one live surface into another, drawn as
 * a full-viewport triangle over the destination rect: a resolve cannot be a
 * CopyTextureRegion once the source holds its samples (an antialiased target
 * is kept at its sample resolution, rsx_dispatch.h). Pipelines are made on
 * first use, one per render-target format. Returns 1 when drawn. */
static int ld_downsample(u32 si, u32 sx, u32 sy, u32 k,
                         u32 di, u32 dx, u32 dy, u32 w, u32 h)
{
    const DXGI_FORMAT fmt = g.surfaces[di].fmt;
    ID3D12PipelineState* pso = NULL;
    u32 n = 0;
    for (; n < 4 && g.downsample_pso[n]; n++)
        if (g.downsample_fmt[n] == fmt) { pso = g.downsample_pso[n]; break; }
    if (!pso) {
        if (n == 4) return 0;
        static const char src[] =
            "Texture2D<float4> s : register(t0);\n"
            "cbuffer c : register(b0) { int2 so; int2 dorg; int k; };\n"
            "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
            "    float2 t = float2((id << 1) & 2, id & 2);\n"
            "    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
            "}\n"
            "float4 ps(float4 p : SV_Position) : SV_Target {\n"
            "    int2 b = so + (int2(p.xy) - dorg) * k;\n"
            "    float4 a = 0;\n"
            "    for (int y = 0; y < k; y++)\n"
            "        for (int x = 0; x < k; x++) a += s.Load(int3(b + int2(x, y), 0));\n"
            "    return a / (k * k);\n"
            "}\n";
        ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
        HRESULT hr = D3DCompile(src, sizeof(src) - 1u, "ld_downsample", NULL, NULL,
                                "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
        if (err) { err->lpVtbl->Release(err); err = NULL; }
        if (SUCCEEDED(hr))
            hr = D3DCompile(src, sizeof(src) - 1u, "ld_downsample", NULL, NULL,
                            "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
        if (err) { err->lpVtbl->Release(err); err = NULL; }
        if (SUCCEEDED(hr) && !g.downsample_rootsig) {
            D3D12_DESCRIPTOR_RANGE range = {0};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;
            D3D12_ROOT_PARAMETER rp[2] = {0};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp[0].Constants.Num32BitValues = 5;
            rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[1].DescriptorTable.NumDescriptorRanges = 1;
            rp[1].DescriptorTable.pDescriptorRanges = &range;
            rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC rsd = {0};
            rsd.NumParameters = 2; rsd.pParameters = rp;
            ID3DBlob* sig = NULL;
            hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
            if (err) err->lpVtbl->Release(err);
            if (SUCCEEDED(hr)) {
                hr = g.dev->lpVtbl->CreateRootSignature(g.dev, 0, sig->lpVtbl->GetBufferPointer(sig),
                        sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature,
                        (void**)&g.downsample_rootsig);
                sig->lpVtbl->Release(sig);
            }
        }
        if (SUCCEEDED(hr)) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
            pd.pRootSignature = g.downsample_rootsig;
            pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb);
            pd.VS.BytecodeLength = vsb->lpVtbl->GetBufferSize(vsb);
            pd.PS.pShaderBytecode = psb->lpVtbl->GetBufferPointer(psb);
            pd.PS.BytecodeLength = psb->lpVtbl->GetBufferSize(psb);
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = 0xFFFFFFFFu;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = fmt;
            pd.SampleDesc.Count = 1;
            hr = g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd,
                    &IID_ID3D12PipelineState, (void**)&pso);
        }
        if (vsb) vsb->lpVtbl->Release(vsb);
        if (psb) psb->lpVtbl->Release(psb);
        if (FAILED(hr) || !pso) {
            static u32 fail_logs = 0;
            if (PS3_LOG_FIRST(fail_logs, 8))
                fprintf(stderr, "[live-draw] downsample pipeline for format %u failed hr=0x%08lX\n",
                        (unsigned)fmt, (unsigned long)hr);
            return 0;
        }
        g.downsample_pso[n] = pso; g.downsample_fmt[n] = fmt;
    }
    u32 slots[SRV_TABLE_SIZE];
    for (u32 i = 0; i < SRV_TABLE_SIZE; i++) slots[i] = SRV_WHITE;
    slots[0] = SRV_SURFACE_BASE + si;
    const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots);
    if (!table.ptr) return 0;

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g.surfaces[si].tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    rsx_rec_ResourceBarrier(g.frame, 1, &b);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(LD_SWAP_BUFFERS + di);
    rsx_rec_targets(g.frame, 1, &rtv, NULL);
    ID3D12DescriptorHeap* heaps[] = {g.frame->srv_heap, g.frame->smp_heap};
    rsx_rec_heaps(g.frame, heaps);
    rsx_rec_pso(g.frame, pso);
    rsx_rec_root(g.frame, g.downsample_rootsig);
    const surface_t* src = &g.surfaces[si];
    const surface_t* dst = &g.surfaces[di];
    const u32 c[5] = {ld_surface_edge(src, sx), ld_surface_edge(src, sy),
        ld_surface_edge(dst, dx), ld_surface_edge(dst, dy), k};
    rsx_rec_SetGraphicsRoot32BitConstants(g.frame, 0, 5, c, 0);
    rsx_rec_SetGraphicsRootDescriptorTable(g.frame, 1, table);
    const D3D12_VIEWPORT vp = {(float)ld_surface_edge(dst, dx), (float)ld_surface_edge(dst, dy),
        (float)ld_surface_physical(dst, w), (float)ld_surface_physical(dst, h), 0.0f, 1.0f};
    const D3D12_RECT sc = {ld_surface_edge(dst, dx), ld_surface_edge(dst, dy),
        ld_surface_edge(dst, dx + w), ld_surface_edge(dst, dy + h)};
    rsx_rec_viewport(g.frame, &vp);
    rsx_rec_scissor(g.frame, &sc);
    rsx_rec_topology(g.frame, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    rsx_rec_DrawInstanced(g.frame, 3, 1, 0, 0);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rsx_rec_ResourceBarrier(g.frame, 1, &b);
    return 1;
}

/* The copy half of a scaled resolve, done on the GPU. The host surface holds
 * the guest's grid 1:1 -- an antialiased target is kept at its sample
 * resolution (Tornado Outbreak: pitch 10240 = 2560 guest pixels, host surface
 * 2560x1440) -- so the guest position is the host position and a ds x dt
 * resolve is a k x k box downsample. Each tile lands in a live surface at the display buffer it writes
 * into, so the flip presents a snapshot taken at resolve time -- presenting
 * the source live (the alias below) showed whatever the title rendered into
 * it next, and the menu flickered to black on two frames in three.
 * Returns 1 when copied. */
/* Scaled resolve as a draw. NV3089 scales with independent 20.12 factors per
 * axis; Twisted Metal resolves its 1280x704 scene into a 642x432 rect at
 * (39,24) of a 720x480 display buffer (ds ~1.99, dt ~1.63), which no
 * CopyTextureRegion can express. Draw one triangle covering the destination
 * rect and sample the source surface with a linear clamp sampler.
 * One root signature, one PSO per destination format (cached). */
static ID3D12RootSignature* g_sblit_rs;
static struct { DXGI_FORMAT fmt; ID3D12PipelineState* pso; } g_sblit_pso[4];

static ID3D12PipelineState* ld_scaled_blit_pso(DXGI_FORMAT fmt)
{
    for (int i = 0; i < 4; i++)
        if (g_sblit_pso[i].pso && g_sblit_pso[i].fmt == fmt) return g_sblit_pso[i].pso;
    static const char src[] =
        "cbuffer C : register(b0) { float4 r; };\n"
        "Texture2D t0 : register(t0);\n"
        "SamplerState s0 : register(s0);\n"
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "V vs(uint id : SV_VertexID) {\n"
        "    V o; float2 q = float2((id << 1) & 2, id & 2);\n"
        "    o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "    o.uv = r.xy + (r.zw - r.xy) * q; return o; }\n"
        "float4 ps(V i) : SV_Target { return t0.SampleLevel(s0, i.uv, 0); }\n";
    if (!g_sblit_rs) {
        D3D12_DESCRIPTOR_RANGE range = {0};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER p[2] = {0};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.Num32BitValues = 4;
        p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges = &range;
        p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC smp = {0};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = D3D12_FLOAT32_MAX;
        smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {0};
        rsd.NumParameters = 2; rsd.pParameters = p;
        rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &smp;
        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            if (err) err->lpVtbl->Release(err);
            return NULL;
        }
        HRESULT hr = g.dev->lpVtbl->CreateRootSignature(g.dev, 0, sig->lpVtbl->GetBufferPointer(sig),
                         sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&g_sblit_rs);
        sig->lpVtbl->Release(sig);
        if (FAILED(hr)) { g_sblit_rs = NULL; return NULL; }
    }
    ID3DBlob* vs = NULL; ID3DBlob* ps = NULL; ID3DBlob* err = NULL;
    if (FAILED(D3DCompile(src, sizeof(src) - 1u, "scaled_blit", NULL, NULL, "vs", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(D3DCompile(src, sizeof(src) - 1u, "scaled_blit", NULL, NULL, "ps", "ps_5_0", 0, 0, &ps, &err))) {
        fprintf(stderr, "[scaled-blit] shader compile failed: %s\n",
                err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        if (vs) vs->lpVtbl->Release(vs);
        return NULL;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = g_sblit_rs;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs); pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps); pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    HRESULT hr = g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps);
    if (FAILED(hr)) { fprintf(stderr, "[scaled-blit] PSO failed hr=0x%08lX\n", (unsigned long)hr); return NULL; }
    for (int i = 0; i < 4; i++)
        if (!g_sblit_pso[i].pso) { g_sblit_pso[i].fmt = fmt; g_sblit_pso[i].pso = pso; break; }
    return pso;
}

#include "rsx_live_image_transfer.h"

/* Guest rectangles enter in logical texels; only the destination raster
 * rectangle is converted. Normalized source UVs are resolution independent. */
static int ld_scaled_blit(u32 si, float sx, float sy, float sw, float sh,
                          u32 di, u32 dx, u32 dy, u32 dw, u32 dh)
{
    surface_t* src = &g.surfaces[si];
    surface_t* dst = &g.surfaces[di];
    if (!src->w || !src->h) return 0;
    const float uv[4] = {sx / src->w, sy / src->h,
        (sx + sw) / src->w, (sy + sh) / src->h};
    int ok = ld_transfer_image(src->tex, SRV_SURFACE_BASE + si,
        D3D12_RESOURCE_STATE_RENDER_TARGET, dst->tex,
        rtv_handle(LD_SWAP_BUFFERS + di), D3D12_RESOURCE_STATE_RENDER_TARGET,
        dst->fmt, uv, ld_surface_edge(dst, dx), ld_surface_edge(dst, dy),
        rsx_resolution_span(ld_surface_domain(dst), dx, dx + dw),
        rsx_resolution_span(ld_surface_domain(dst), dy, dy + dh));
    if (ok) ld_surface_note_write(di, LD_SURFACE_WRITE_COPY);
    return ok;
}

int rsx_live_draw_resolve_blit(u32 src_loc, u32 src_abs, u32 src_pitch, u32 ds, u32 dt,
                               u32 dst_loc, u32 dst_abs, u32 dst_pitch,
                               u32 out_x, u32 out_y, u32 out_w, u32 out_h)
{
    if (!g.ready || !src_pitch || !dst_pitch || !ds || !dt) return 0;
    /* Uneven or fractional factors cannot be a copy; they are drawn. */
    const int scaled = ds != dt || ds < 0x100000u || (ds & 0xFFFFFu) != 0;
    /* Source surface and the rect's guest position inside it. */
    /* The CLOSEST base at or below the address: surfaces' guest ranges
     * overlap (a stale surface at 0x0 "contains" 0x12804 too), and taking the
     * first match copied every 0x10000 tile out of that black surface. */
    int si = -1; u32 gx = 0, gy = 0;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const surface_t* s = &g.surfaces[i];
        if (!s->tex || s->location != src_loc || src_abs < s->offset) continue;
        const u32 d = src_abs - s->offset;
        if (d / src_pitch >= 2u * s->h) continue;
        if (si >= 0 && g.surfaces[si].offset > s->offset) continue;
        si = (int)i; gx = (d % src_pitch) / 4u; gy = d / src_pitch;
    }
    if (si < 0) return 0;
    /* Destination: the display buffer this tile writes into. */
    int bi = -1;
    for (u32 b = 0; b < 8; b++) {
        const display_buffer_t* d = &g.display_buffers[b];
        if (d->valid && d->location == dst_loc && dst_abs >= d->offset &&
            dst_abs < d->offset + d->pitch * d->height) { bi = (int)b; break; }
    }
    if (bi < 0) return 0;
    const display_buffer_t* db = &g.display_buffers[bi];
    const u32 dx = (dst_abs - db->offset) % db->pitch / 4u + out_x;
    const u32 dy = (dst_abs - db->offset) / db->pitch + out_y;
    const u32 slot = surface_get(db->location, db->offset, db->width, db->height, g.surfaces[si].fmt);
    if (slot == LD_INVALID_SURFACE || (int)slot == si) return 0;
    surface_t* dst = &g.surfaces[slot];
    if (scaled) {
        /* Source extent in the blit's source units, which match host pixels
         * for a surface the host renders at its guest size. */
        u32 w = out_w, h = out_h;
        if (dx >= dst->w || dy >= dst->h) return 0;
        if (dx + w > dst->w) w = dst->w - dx;
        if (dy + h > dst->h) h = dst->h - dy;
        const float sw = (float)out_w * ((float)ds / 1048576.0f);
        const float sh = (float)out_h * ((float)dt / 1048576.0f);
        return ld_scaled_blit((u32)si, (float)gx, (float)gy, sw, sh, slot, dx, dy, w, h);
    }
    if (dst->fmt != g.surfaces[si].fmt) return 0;
    const u32 k = ds >> 20;                       /* 20.12 fixed -> integer factor */
    const u32 sx = gx, sy = gy;
    u32 w = out_w, h = out_h;
    if (dx >= dst->w || dy >= dst->h || sx >= g.surfaces[si].w || sy >= g.surfaces[si].h) return 0;
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (sx + w * k > g.surfaces[si].w) w = (g.surfaces[si].w - sx) / k;
    if (sy + h * k > g.surfaces[si].h) h = (g.surfaces[si].h - sy) / k;
    if (!w || !h) return 0;
    /* A copy keeps texels 1:1, so it needs both surfaces at one scale. */
    const rsx_resolution_domain copy_domain = ld_surface_domain(dst);
    if (g.surfaces[si].scale != dst->scale ||
        !rsx_resolution_copy_aligned(copy_domain, sx, dx) ||
        !rsx_resolution_copy_aligned(copy_domain, sy, dy) ||
        (k > 1 && copy_domain.scale != (u32)copy_domain.scale))
        return ld_scaled_blit((u32)si, (float)sx, (float)sy,
            (float)(w * k), (float)(h * k), slot, dx, dy, w, h);
    if (k > 1) {
        if (!ld_downsample((u32)si, sx, sy, k, slot, dx, dy, w, h)) return 0;
        ld_surface_note_write(slot, LD_SURFACE_WRITE_COPY);
        return 1;
    }
    ID3D12Resource* s = g.surfaces[si].tex;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    for (int q = 0; q < 2; q++) {
        b[q].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[q].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[q].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    b[0].Transition.pResource = s;        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = dst->tex; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    rsx_rec_ResourceBarrier(g.frame, 2, b);
    D3D12_TEXTURE_COPY_LOCATION sl = {0}, dl = {0};
    sl.pResource = s;        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.pResource = dst->tex; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { ld_surface_edge(dst, sx), ld_surface_edge(dst, sy), 0,
        ld_surface_edge(dst, sx + w), ld_surface_edge(dst, sy + h), 1 };
    rsx_rec_CopyTextureRegion(g.frame, &dl, ld_surface_edge(dst, dx), ld_surface_edge(dst, dy), 0, &sl, &box);
    for (int q = 0; q < 2; q++) {
        b[q].Transition.StateBefore = b[q].Transition.StateAfter;
        b[q].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    rsx_rec_ResourceBarrier(g.frame, 2, b);
    ld_surface_note_write(slot, LD_SURFACE_WRITE_COPY);
    if (s_csv_file && s_csv_gate_open) {
        fprintf(s_csv_file, "#resolve,%u,%u:0x%X,%u:0x%X,%u,%u,%ux%u\n", g_ld_frames,
                src_loc, src_abs, dst_loc, dst_abs, sx, sy, w, h);
        fflush(s_csv_file);
    }
    return 1;
}

void rsx_live_draw_note_resolve(u32 src_loc, u32 src_abs, u32 dst_loc, u32 dst_abs)
{
    if (!g.ready) return;
    for (u32 b = 0; b < 8; b++) {
        display_buffer_t* d = &g.display_buffers[b];
        if (!d->valid || d->location != dst_loc || d->offset != dst_abs) continue;
        for (u32 i = 0; i < g.n_surfaces; i++) {
            const surface_t* s = &g.surfaces[i];
            if (s->tex && s->location == src_loc && s->offset == src_abs) {
                if (d->resolve_src != (int)i + 1)
                    fprintf(stderr, "[live-draw] display buffer %u resolves from surface %u (0x%X)\n",
                            b, i, s->offset);
                d->resolve_src = (int)i + 1;
                return;
            }
        }
    }
}

static int ld_blit_gpu(u32 src_loc, u32 src_abs, u32 src_pitch,
                       u32 dst_loc, u32 dst_abs, u32 dst_pitch, u32 w, u32 h)
{
    if (!g.ready || !src_pitch || !dst_pitch || !w || !h) return 0;
    u32 sx, sy, dx = 0, dy = 0;
    const int si = surface_containing(src_loc, src_abs, src_pitch, w, h, &sx, &sy);
    if (si < 0) return -1;            /* source is not a live surface */
    int di = surface_containing(dst_loc, dst_abs, dst_pitch, w, h, &dx, &dy);
    if (di < 0) {
        const u32 slot = surface_get(dst_loc, dst_abs, dst_pitch / 4u, h,
                                     g.surfaces[si].fmt);
        if (slot == LD_INVALID_SURFACE) return -2;
        di = (int)slot;
        if (w > g.surfaces[di].w || h > g.surfaces[di].h) return -3;
    }
    if (di == si) return -4;
    if (g.surfaces[di].fmt != g.surfaces[si].fmt) return -5;
    const surface_t* dsurf = &g.surfaces[di];
    if (g.surfaces[si].scale != dsurf->scale ||
        !rsx_resolution_copy_aligned(ld_surface_domain(dsurf), sx, dx) ||
        !rsx_resolution_copy_aligned(ld_surface_domain(dsurf), sy, dy))
        return ld_scaled_blit((u32)si, (float)sx, (float)sy, (float)w, (float)h,
            (u32)di, dx, dy, w, h) ? 1 : -6;
    ID3D12Resource* src = g.surfaces[si].tex;
    ID3D12Resource* dst = g.surfaces[di].tex;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    for (int k = 0; k < 2; k++) {
        b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[k].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    b[0].Transition.pResource = src; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = dst; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    rsx_rec_ResourceBarrier(g.frame, 2, b);
    D3D12_TEXTURE_COPY_LOCATION s = {0}, d = {0};
    s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { ld_surface_edge(dsurf, sx), ld_surface_edge(dsurf, sy), 0,
        ld_surface_edge(dsurf, sx + w), ld_surface_edge(dsurf, sy + h), 1 };
    rsx_rec_CopyTextureRegion(g.frame, &d, ld_surface_edge(dsurf, dx), ld_surface_edge(dsurf, dy), 0, &s, &box);
    for (int k = 0; k < 2; k++) {
        b[k].Transition.StateBefore = b[k].Transition.StateAfter;
        b[k].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    rsx_rec_ResourceBarrier(g.frame, 2, b);
    ld_surface_note_write((u32)di, LD_SURFACE_WRITE_COPY);
    return 1;
}

static u32 current_surface(void)
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    g_ld_logic_target = 1;
    const u32 slot = surface_get(sf.color_location[0], sf.color_offset[0],
                                 sf.clip_w, sf.clip_h,
                                 sf.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16
                                     ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                     : DXGI_FORMAT_R8G8B8A8_UNORM);
    g_ld_logic_target = 0;
    if (slot != LD_INVALID_SURFACE) g.surfaces[slot].pitch = sf.color_pitch[0];
    return slot;
}

/* On RSX each zeta (depth) address is distinct memory. The offline renderer's
 * s31 fix proved that sharing one D3D depth resource across all of this game's
 * shadow, scene and post-processing passes cross-contaminates later depth
 * tests. Keep the legacy shared target only as a bounded allocation fallback. */
static int honor_zeta_track(void)
{
    static int initialized = 0;
    static int enabled = 1;
    if (!initialized) {
        initialized = 1;
        enabled = getenv("RSX_NO_ZETA_TRACK") ? 0 : 1;
        fprintf(stderr, "[zetatrack] live per-zeta depth %s\n",
                enabled ? "ON" : "OFF (legacy shared target)");
    }
    return enabled;
}

static D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.dsv_heap, &h);
    h.ptr += (size_t)slot * g.dsv_step;
    return h;
}

/* The R32F snapshot is sized from the clip of the first pass that bound the
 * zeta. A title that first binds its full-screen zeta with a smaller clip
 * (a 512x512 pass into a 1280x720 depth buffer) kept a 512x512 snapshot, and
 * every later full-size read of that depth saw 0 outside the top-left
 * corner. Grow the snapshot to the largest clip bound since, within the
 * depth resource. */
static void zdepth_grow_snapshot(u32 i, u32 rt_w, u32 rt_h)
{
    zdepth_t* z = &g.zdepths[i];
    if (!z->snapshot) return;
    u32 nw = rt_w < z->w ? rt_w : z->w, nh = rt_h < z->h ? rt_h : z->h;
    if (nw < z->snapshot_w) nw = z->snapshot_w;
    if (nh < z->snapshot_h) nh = z->snapshot_h;
    if (nw == z->snapshot_w && nh == z->snapshot_h) return;
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = ld_physical(nw);
    rd.Height = ld_physical(nh);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* snap = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
            &IID_ID3D12Resource, (void**)&snap))) {
        fprintf(stderr, "[zetatrack] snapshot grow failed %u:0x%X %ux%u\n",
                z->location, z->offset, nw, nh);
        return;
    }
    fprintf(stderr, "[zetatrack] snapshot %u:0x%X grown %ux%u -> %ux%u\n",
            z->location, z->offset, z->snapshot_w, z->snapshot_h, nw, nh);
    /* The open command list may still read the old one. */
    retire_texture(z->snapshot);
    z->snapshot = snap;
    z->snapshot_w = nw;
    z->snapshot_h = nh;
    z->snapshot_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    srv_write_zdepth(SRV_ZDEPTH_BASE + i, z->snapshot);
    uav_write_zdepth(UAV_ZDEPTH_BASE + i, z->snapshot);
    /* The depth resource still holds what the old snapshot held (a clear
     * would have invalidated both): resolve it again at the next read. */
    if (z->snapshot_valid) z->had_write = 1;
    z->snapshot_valid = 0;
}

/* Returns DSV slot 1+i. Slot 0 is the legacy shared depth fallback. */
static u32 zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h,
                      u32 pitch, u32 format, u32 raster)
{
    if (!honor_zeta_track()) return 0;
    /* The DSV must cover the complete live framebuffer/viewport it is bound
     * with.  Some early passes declare a smaller clip than the active canvas;
     * allocating only that clip causes the D3D device to reject later
     * surface/PSO creation.  Sampling a declared-size window from this padded
     * backing remains a separate SRV concern. */
    u32 want_w = rt_w > g.width ? rt_w : g.width;
    u32 want_h = rt_h > g.height ? rt_h : g.height;
    u32 slot = MAX_SURFACES;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        zdepth_t* z = &g.zdepths[i];
        if (z->location == location && z->offset == offset) {
            z->pitch = pitch; z->depth_format = format; z->raster_type = raster;
            if (z->w >= want_w && z->h >= want_h) {
                zdepth_grow_snapshot(i, rt_w, rt_h);
                return 1 + i;
            }
            slot = i;
            break;
        }
    }
    if (slot == MAX_SURFACES) {
        if (g.n_zdepths >= MAX_SURFACES) {
            fprintf(stderr, "[zetatrack] live cache full; shared fallback\n");
            return 0;
        }
        slot = g.n_zdepths;
    } else {
        zdepth_t* old = &g.zdepths[slot];
        if (want_w < old->w) want_w = old->w;
        if (want_h < old->h) want_h = old->h;
        fprintf(stderr,
                "[zetatrack] live zeta %u:0x%X outgrown %ux%u -> %ux%u\n",
                location, offset, old->w, old->h, want_w, want_h);
        if (old->tex) {
            /* Draws already recorded in the open command list may still
             * reference this DSV.  D3D12 command lists do not retain resource
             * lifetimes for the application, so release it only after the
             * next submit/fence completes. */
            retire_texture(old->tex);
            old->tex = NULL;
        }
        if (old->packed) {
            retire_texture(old->packed); old->packed = NULL;
            old->packed_valid = 0; old->packed_remap_mask = 0;
        }
        if (old->snapshot) {
            retire_texture(old->snapshot);
            old->snapshot = NULL;
        }
    }

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = ld_physical(want_w);
    rd.Height = ld_physical(want_h);
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {0};
    /* Optimized clear values must use the typed DSV format even though the
     * resource itself is typeless so it can also be exposed through an SRV. */
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 1.0f;
    zdepth_t* z = &g.zdepths[slot];
    HRESULT create_hr = g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
            &IID_ID3D12Resource, (void**)&z->tex);
    if (FAILED(create_hr)) {
        fprintf(stderr,
                "[zetatrack] live create failed %u:0x%X hr=0x%08lX; shared fallback\n",
                location, offset, (unsigned long)create_hr);
        z->tex = NULL;
        return 0;
    }
    z->location = location;
    z->offset = offset;
    z->pitch = pitch; z->depth_format = format; z->raster_type = raster;
    z->content_generation = 0; z->packed_valid = 0;
    z->w = want_w;
    z->h = want_h;
    z->had_write = 0;
    z->snapshot_valid = 0;
    z->snapshot_w = rt_w ? rt_w : want_w;
    z->snapshot_h = rt_h ? rt_h : want_h;
    if (z->snapshot_w > want_w) z->snapshot_w = want_w;
    if (z->snapshot_h > want_h) z->snapshot_h = want_h;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvd = {0};
    dsvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    rsx_rec_drain();
    if (g.frame) g.frame->bindings.valid &= ~8u;
    g.dev->lpVtbl->CreateDepthStencilView(
        g.dev, z->tex, &dsvd, dsv_handle(1 + slot));

    /* D3D12 forbids a partial CopyTextureRegion from a depth/stencil
     * resource.  Keep the replay-proven logical dimensions and resolve the
     * depth plane with a compute shader instead; that is a normal SRV read,
     * not a depth-resource copy operation. */
    D3D12_RESOURCE_DESC snapshot_rd = {0};
    snapshot_rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    snapshot_rd.Width = ld_physical(z->snapshot_w);
    snapshot_rd.Height = ld_physical(z->snapshot_h);
    snapshot_rd.DepthOrArraySize = 1;
    snapshot_rd.MipLevels = 1;
    snapshot_rd.Format = DXGI_FORMAT_R32_FLOAT;
    snapshot_rd.SampleDesc.Count = 1;
    snapshot_rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT snapshot_hr = g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &snapshot_rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
            &IID_ID3D12Resource, (void**)&z->snapshot);
    if (FAILED(snapshot_hr)) {
        fprintf(stderr,
                "[zetatrack] live snapshot create failed %u:0x%X "
                "%ux%u hr=0x%08lX; guest fallback\n",
                location, offset, z->snapshot_w, z->snapshot_h,
                (unsigned long)snapshot_hr);
        z->snapshot = NULL;
    } else {
        z->snapshot_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        srv_write_depth_source(SRV_DEPTH_SOURCE_BASE + slot, z->tex);
        D3D12_SHADER_RESOURCE_VIEW_DESC stencil = {0};
        stencil.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        stencil.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        /* Stencil occupies component G of this typed view. */
        stencil.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1,1,1,1);
        stencil.Texture2D.MipLevels = 1;
        stencil.Texture2D.PlaneSlice = 1;
        g.dev->lpVtbl->CreateShaderResourceView(g.dev, z->tex, &stencil,
            srv_cpu(SRV_STENCIL_SOURCE_BASE + slot));
        srv_write_zdepth(SRV_ZDEPTH_BASE + slot, z->snapshot);
        uav_write_zdepth(UAV_ZDEPTH_BASE + slot, z->snapshot);
    }
    rsx_rec_ClearDepthStencilView(g.frame, dsv_handle(1 + slot),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, NULL);
    z->cleared = 1;
    fprintf(stderr,
            "[zetatrack] live new #%u %u:0x%X backing=%ux%u "
            "clip=%ux%u canvas=%ux%u\n",
            slot, location, offset, want_w, want_h,
            rt_w, rt_h, g.width, g.height);
    if (slot == g.n_zdepths) g.n_zdepths++;
    return 1 + slot;
}

/* Publish the current GPU depth content on demand. Both a clear and geometry
 * writes make that content authoritative. CPU guest memory does not contain
 * GPU-produced depth, so a clear-only image must not fall back to that memory. */
static int zdepth_resolve(u32 slot, int packed)
{
    if (!slot) return 0;
    zdepth_t* z = &g.zdepths[slot - 1];
    ID3D12Resource* destination = packed ? z->packed : z->snapshot;
    ID3D12PipelineState* pipeline = packed ? g.packed_depth_pso : g.depth_snapshot_pso;
    D3D12_RESOURCE_STATES* state = packed ? &z->packed_state : &z->snapshot_state;
    int* valid = packed ? &z->packed_valid : &z->snapshot_valid;
    const u32 width = packed ? z->packed_w : z->snapshot_w;
    const u32 height = packed ? z->packed_h : z->snapshot_h;
    if (!destination || !pipeline || !g.depth_snapshot_rootsig) return 0;
    if (packed) {
        if (*valid) return 1;
        if (!z->content_generation) return 0;
    } else if (!z->had_write) return *valid;

    D3D12_GPU_DESCRIPTOR_HANDLE source_table, destination_table;
    if (!depth_snapshot_descriptors(
            slot - 1u, packed, &source_table, &destination_table)) {
        fprintf(stderr,
                "[zetatrack] depth snapshot descriptor ring exhausted "
                "at frame %u\n",
                g_ld_frames);
        return (*valid);
    }

    D3D12_RESOURCE_BARRIER bars[2] = {0};
    u32 nbar = 0;
    bars[nbar].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[nbar].Transition.pResource = z->tex;
    bars[nbar].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bars[nbar].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    bars[nbar].Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    nbar++;
    if ((*state) != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        bars[nbar].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[nbar].Transition.pResource = destination;
        bars[nbar].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bars[nbar].Transition.StateBefore = (*state);
        bars[nbar].Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        nbar++;
    }
    rsx_rec_ResourceBarrier(g.frame, nbar, bars);

    ID3D12DescriptorHeap* heaps[] = {g.frame->srv_heap, g.frame->smp_heap};
    rsx_rec_heaps(g.frame, heaps);
    rsx_rec_pso(g.frame, pipeline);
    rsx_rec_SetComputeRootSignature(g.frame, g.depth_snapshot_rootsig);
    rsx_rec_SetComputeRootDescriptorTable(g.frame, 0, source_table);
    rsx_rec_SetComputeRootDescriptorTable(g.frame, 1, destination_table);
    rsx_rec_Dispatch(g.frame, (ld_physical(width) + 7u) / 8u,
        (ld_physical(height) + 7u) / 8u, 1);

    D3D12_RESOURCE_BARRIER uav_done = {0};
    uav_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done.UAV.pResource = destination;
    rsx_rec_ResourceBarrier(g.frame, 1, &uav_done);

    D3D12_RESOURCE_BARRIER done[2] = {0};
    done[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    done[0].Transition.pResource = z->tex;
    done[0].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    done[0].Transition.StateBefore =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    done[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    done[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    done[1].Transition.pResource = destination;
    done[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    done[1].Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    done[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    rsx_rec_ResourceBarrier(g.frame, 2, done);
    (*state) = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    (*valid) = 1;
    if (packed) {
        z->packed_resolves++;
        if (z->packed_resolves == 1)
            fprintf(stderr, "[packed-depth] %u:0x%X %ux%u GPU view published\n",
                z->location, z->offset, width, height);
    } else z->had_write = 0;
    return 1;
}

static int zdepth_snapshot(u32 slot) { return zdepth_resolve(slot, 0); }

/* Only exact pitched Z24S8 -> linear ARGB views are supported here. A newer
 * colour writer owns the address; creating an empty colour attachment does not. */
static int zdepth_packed_match(const zdepth_t* z, const rsx_dsp_texture* t)
{
    if (z->location != t->location || z->offset != t->offset ||
        z->depth_format != 2 || z->raster_type != 1 || !z->content_generation ||
        (t->format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM) != TEX_FMT_A8R8G8B8 ||
        !(t->format & TEX_FMT_LINEAR) || t->dimension != 2 || t->cubemap ||
        t->mipmaps > 1 || t->pitch != z->pitch || t->width != z->snapshot_w ||
        t->height != z->snapshot_h) return 0;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const surface_t* c = &g.surfaces[i];
        if (c->location == z->location && c->offset == z->offset &&
            c->color_content_generation > z->content_generation) return 0;
    }
    return 1;
}
void rsx_live_draw_depth_view_stats(rsx_depth_view_stats* stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    for (u32 i=0; i<g.n_zdepths; i++) {
        const zdepth_t* z = &g.zdepths[i];
        stats->resolves += z->packed_resolves;
        stats->binds += z->packed_binds;
        if (z->packed) {
            stats->views++;
            stats->resident_bytes += (u64)ld_physical(z->packed_w)*ld_physical(z->packed_h)*4;
        }
    }
}
static int zdepth_packed_view(u32 slot, const rsx_dsp_texture* t, u32 unit)
{
    zdepth_t* z = &g.zdepths[slot - 1];
    if (!z->packed || z->packed_w != t->width || z->packed_h != t->height) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = ld_physical(t->width); rd.Height = ld_physical(t->height);
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* tex = NULL;
        if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE,
            &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource,
            (void**)&tex))) return -1;
        if (z->packed) retire_texture(z->packed);
        z->packed = tex; z->packed_w = t->width; z->packed_h = t->height;
        z->packed_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        z->packed_valid = 0; z->packed_remap_mask = 0;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {0};
        ud.Format = rd.Format; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g.dev->lpVtbl->CreateUnorderedAccessView(g.dev, tex, NULL, &ud,
            srv_cpu(UAV_PACKED_DEPTH_BASE + slot - 1));
    }
    if (!zdepth_resolve(slot, 1)) return -1;
    const u32 descriptor = SRV_PACKED_DEPTH_BASE + (slot-1)*SRV_TABLE_SIZE + unit;
    if (!(z->packed_remap_mask & (1u<<unit)) || z->packed_remap[unit] != t->remap) {
        static const u32 sel2d3d[4] = {3,0,1,2}, out2comp[4] = {1,2,3,0};
        u32 mapping = 1u<<12;
        for (u32 out=0; out<4; out++) {
            const u32 comp = out2comp[out], op = (t->remap>>(8+comp*2))&3;
            const u32 sel = (t->remap>>(comp*2))&3;
            mapping |= (op==0 ? 4 : op==1 ? 5 : sel2d3d[sel]) << (out*3);
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = mapping; sd.Texture2D.MipLevels = 1;
        g.dev->lpVtbl->CreateShaderResourceView(g.dev,z->packed,&sd,srv_cpu(descriptor));
        z->packed_remap[unit] = t->remap; z->packed_remap_mask |= 1u<<unit;
    }
    z->packed_binds++;
    return (int)descriptor;
}

/* ---------------------------------------------------------------------------
 * PSO cache (VP+FP+render-state keyed)
 * -----------------------------------------------------------------------*/
static u64 fnv1a(const void* data, u32 n, u64 h)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static int sampler_table_needs_flush(const u32 slots[SMP_TABLE_SIZE])
{
    if (g.frame->smp_ring_used < SMP_RING_TABLES) return 0;
    for (u32 i = 0; i < g.frame->smp_ring_used; i++)
        if (memcmp(g.frame->smp_ring_slots[i], slots,
                   sizeof(g.frame->smp_ring_slots[i])) == 0)
            return 0;
    return 1;
}

static u32 shader_disk_cache_stage_index(u32 stage)
{
    return stage == 'V' ? 0u : 1u;
}

static void shader_disk_cache_progress(u64 value)
{
    if (value > 4u && (value & (value - 1u)) != 0u)
        return;
    fprintf(stderr,
            "[shader-disk-cache] hits{vs=%llu ps=%llu} "
            "misses{vs=%llu ps=%llu} writes{vs=%llu ps=%llu} "
            "rejects=%llu\n",
            (unsigned long long)g_ld_shader_disk_hits[0],
            (unsigned long long)g_ld_shader_disk_hits[1],
            (unsigned long long)g_ld_shader_disk_misses[0],
            (unsigned long long)g_ld_shader_disk_misses[1],
            (unsigned long long)g_ld_shader_disk_writes[0],
            (unsigned long long)g_ld_shader_disk_writes[1],
            (unsigned long long)g_ld_shader_disk_rejects);
}

static int shader_disk_cache_prepare(void)
{
    if (g_ld_shader_disk_ready >= 0)
        return g_ld_shader_disk_ready;
    g_ld_shader_disk_ready = 0;
    if (getenv("RSX_NO_SHADER_DISK_CACHE")) {
        fprintf(stderr, "[shader-disk-cache] disabled\n");
        return 0;
    }

    const char* override_dir = getenv("RSX_SHADER_CACHE_DIR");
    if (override_dir && override_dir[0]) {
        if (snprintf(g_ld_shader_disk_dir, sizeof(g_ld_shader_disk_dir),
                     "%s", override_dir) < 0 ||
            strlen(g_ld_shader_disk_dir) >= sizeof(g_ld_shader_disk_dir) - 1u)
            return 0;
    } else {
        CreateDirectoryA("scratch", NULL);
        snprintf(g_ld_shader_disk_dir, sizeof(g_ld_shader_disk_dir),
                 "scratch\\rsx_shader_cache_v%u",
                 SHADER_DISK_CACHE_VERSION);
    }

    if (!CreateDirectoryA(g_ld_shader_disk_dir, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        fprintf(stderr,
                "[shader-disk-cache] cannot create '%s' error=%lu\n",
                g_ld_shader_disk_dir, (unsigned long)GetLastError());
        return 0;
    }
    const DWORD attributes = GetFileAttributesA(g_ld_shader_disk_dir);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;

    g_ld_shader_disk_ready = 1;
    fprintf(stderr, "[shader-disk-cache] enabled dir='%s' version=%u\n",
            g_ld_shader_disk_dir, SHADER_DISK_CACHE_VERSION);
    return 1;
}

static int shader_disk_cache_path(
    char path[MAX_PATH], u32 stage, u64 hash, u32 source_length)
{
    if (!shader_disk_cache_prepare())
        return 0;
    const int length = snprintf(
        path, MAX_PATH, "%s\\%c_%016llX_%08X.bin",
        g_ld_shader_disk_dir, (char)stage,
        (unsigned long long)hash, source_length);
    return length > 0 && length < MAX_PATH;
}

static ID3DBlob* shader_disk_cache_load(
    u32 stage, const char* source, u32 source_length, u64 hash)
{
    char path[MAX_PATH];
    const u32 stage_index = shader_disk_cache_stage_index(stage);
    if (!shader_disk_cache_path(path, stage, hash, source_length))
        return NULL;

    FILE* file = fopen(path, "rb");
    if (!file) {
        const u64 value = ++g_ld_shader_disk_misses[stage_index];
        shader_disk_cache_progress(value);
        return NULL;
    }

    shader_disk_cache_header header;
    memset(&header, 0, sizeof(header));
    int valid = fread(&header, sizeof(header), 1, file) == 1 &&
        header.magic == SHADER_DISK_CACHE_MAGIC &&
        header.version == SHADER_DISK_CACHE_VERSION &&
        header.stage == stage && header.source_hash == hash &&
        header.source_length == source_length && source_length <= 256u * 1024u &&
        header.blob_length > 0 &&
        header.blob_length <= SHADER_DISK_CACHE_MAX_BLOB;
    char* stored_source = NULL;
    ID3DBlob* blob = NULL;
    if (valid) {
        stored_source = (char*)malloc(source_length ? source_length : 1u);
        valid = stored_source != NULL &&
            fread(stored_source, 1, source_length, file) == source_length &&
            memcmp(stored_source, source, source_length) == 0;
    }
    if (valid) {
        valid = SUCCEEDED(D3DCreateBlob(header.blob_length, &blob)) && blob &&
            fread(blob->lpVtbl->GetBufferPointer(blob), 1,
                  header.blob_length, file) == header.blob_length;
    }
    free(stored_source);
    fclose(file);

    if (!valid) {
        if (blob)
            blob->lpVtbl->Release(blob);
        DeleteFileA(path);
        g_ld_shader_disk_rejects++;
        const u64 value = ++g_ld_shader_disk_misses[stage_index];
        shader_disk_cache_progress(value);
        return NULL;
    }

    const u64 value = ++g_ld_shader_disk_hits[stage_index];
    shader_disk_cache_progress(value);
    return blob;
}

static void shader_disk_cache_store(
    u32 stage, const char* source, u32 source_length, u64 hash,
    ID3DBlob* blob)
{
    if (!blob || source_length > 256u * 1024u)
        return;
    const size_t blob_length = blob->lpVtbl->GetBufferSize(blob);
    if (!blob_length || blob_length > SHADER_DISK_CACHE_MAX_BLOB)
        return;

    char path[MAX_PATH], temporary[MAX_PATH];
    if (!shader_disk_cache_path(path, stage, hash, source_length))
        return;
    const int temporary_length = snprintf(
        temporary, sizeof(temporary), "%s.tmp.%lu", path,
        (unsigned long)GetCurrentProcessId());
    if (temporary_length <= 0 || temporary_length >= sizeof(temporary))
        return;

    shader_disk_cache_header header;
    memset(&header, 0, sizeof(header));
    header.magic = SHADER_DISK_CACHE_MAGIC;
    header.version = SHADER_DISK_CACHE_VERSION;
    header.stage = stage;
    header.source_length = source_length;
    header.blob_length = (u32)blob_length;
    header.source_hash = hash;

    FILE* file = fopen(temporary, "wb");
    if (!file)
        return;
    const int wrote = fwrite(&header, sizeof(header), 1, file) == 1 &&
        fwrite(source, 1, source_length, file) == source_length &&
        fwrite(blob->lpVtbl->GetBufferPointer(blob), 1,
               blob_length, file) == blob_length &&
        fflush(file) == 0;
    fclose(file);
    if (!wrote || !MoveFileExA(
            temporary, path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temporary);
        return;
    }

    const u32 stage_index = shader_disk_cache_stage_index(stage);
    const u64 value = ++g_ld_shader_disk_writes[stage_index];
    shader_disk_cache_progress(value);
}

static u64 ld_hash_structural_render_state(
    const render_state_t* state, u64 hash)
{
#define LD_HASH_RENDER_FIELD(name) \
    hash = fnv1a(&state->name, sizeof(state->name), hash)
    /* Alpha payload is deliberately absent. Enable and compare mode still
     * select distinct shader/PSO behavior. Every other D3D12 render-state
     * field is listed explicitly so struct padding can never become identity. */
    LD_HASH_RENDER_FIELD(alpha_test_enable);
    LD_HASH_RENDER_FIELD(alpha_func);
    LD_HASH_RENDER_FIELD(blend_enable);
    LD_HASH_RENDER_FIELD(sf_rgb);
    LD_HASH_RENDER_FIELD(df_rgb);
    LD_HASH_RENDER_FIELD(sf_a);
    LD_HASH_RENDER_FIELD(df_a);
    LD_HASH_RENDER_FIELD(eq_rgb);
    LD_HASH_RENDER_FIELD(eq_a);
    LD_HASH_RENDER_FIELD(depth_test);
    LD_HASH_RENDER_FIELD(depth_write);
    LD_HASH_RENDER_FIELD(depth_func);
    LD_HASH_RENDER_FIELD(depth_clip);
    LD_HASH_RENDER_FIELD(cull_enable);
    LD_HASH_RENDER_FIELD(cull_face);
    LD_HASH_RENDER_FIELD(front_face);
    LD_HASH_RENDER_FIELD(color_mask);
    LD_HASH_RENDER_FIELD(logic_enable);
    LD_HASH_RENDER_FIELD(logic_op);
    LD_HASH_RENDER_FIELD(line_smooth);
    LD_HASH_RENDER_FIELD(rt_fp16);
    LD_HASH_RENDER_FIELD(rt_count);
    LD_HASH_RENDER_FIELD(stencil_enable);
    LD_HASH_RENDER_FIELD(stencil_two_sided);
    LD_HASH_RENDER_FIELD(s_func);
    LD_HASH_RENDER_FIELD(s_func_mask);
    LD_HASH_RENDER_FIELD(s_write_mask);
    LD_HASH_RENDER_FIELD(s_fail);
    LD_HASH_RENDER_FIELD(s_zfail);
    LD_HASH_RENDER_FIELD(s_zpass);
    LD_HASH_RENDER_FIELD(bs_func);
    LD_HASH_RENDER_FIELD(bs_fail);
    LD_HASH_RENDER_FIELD(bs_zfail);
    LD_HASH_RENDER_FIELD(bs_zpass);
#undef LD_HASH_RENDER_FIELD
    return hash;
}

/*
 * PSO identity includes render state, but D3DCompile only consumes the final
 * generated shader text.  Keep those cache boundaries separate: one exact
 * HLSL program is compiled once and its bytecode is reused by every PSO state
 * variant that references it.  The source bytes are retained for collision-
 * safe equality; the 64-bit hash is only an index accelerator.
 */
static ID3DBlob* shader_blob_cache_find(
    shader_blob_cache_t* cache, const char* source, u32 source_length,
    u64 hash, int* post_boundary_hit, int* hash_seen)
{
    if (post_boundary_hit)
        *post_boundary_hit = 0;
    if (hash_seen)
        *hash_seen = 0;
    for (u32 i = 0; i < cache->count; i++) {
        shader_blob_t* entry = &cache->entries[i];
        if (entry->hash != hash)
            continue;
        if (hash_seen)
            *hash_seen = 1;
        if (
            entry->source_length != source_length ||
            memcmp(entry->source, source, source_length) != 0)
            continue;
        if (post_boundary_hit && i >= FORMER_MAX_SHADER_BLOBS)
            *post_boundary_hit = 1;
        entry->blob->lpVtbl->AddRef(entry->blob);
        return entry->blob;
    }
    return NULL;
}

typedef enum {
    SHADER_BLOB_INSERT_SKIPPED = 0,
    SHADER_BLOB_INSERTED,
    SHADER_BLOB_INSERT_FULL
} shader_blob_insert_result;

static shader_blob_insert_result shader_blob_cache_insert(
    shader_blob_cache_t* cache, const char* source, u32 source_length,
    u64 hash, ID3DBlob* blob)
{
    if (!blob)
        return SHADER_BLOB_INSERT_SKIPPED;
    if (cache->count >= MAX_SHADER_BLOBS)
        return SHADER_BLOB_INSERT_FULL;
    char* source_copy = (char*)malloc((size_t)source_length + 1u);
    if (!source_copy)
        return SHADER_BLOB_INSERT_SKIPPED;
    memcpy(source_copy, source, source_length);
    source_copy[source_length] = '\0';
    shader_blob_t* entry = &cache->entries[cache->count++];
    entry->hash = hash;
    entry->source_length = source_length;
    entry->source = source_copy;
    entry->blob = blob;
    blob->lpVtbl->AddRef(blob);
    cache->retained_source_bytes += source_length;
    cache->retained_blob_bytes += blob->lpVtbl->GetBufferSize(blob);
    return SHADER_BLOB_INSERTED;
}

static void shader_blob_cache_insert_accounted(
    shader_blob_cache_t* cache, const char* source, u32 source_length,
    u64 hash, ID3DBlob* blob, u32 stage, int hash_seen)
{
    const u32 count_before = cache->count;
    const shader_blob_insert_result result = shader_blob_cache_insert(
        cache, source, source_length, hash, blob);
#if defined(YZ_PERF_PROFILE)
    if (stage == 'V') {
        if (result == SHADER_BLOB_INSERTED) {
            g_ld_profile.total.vs_blob_inserts++;
            if (count_before >= FORMER_MAX_SHADER_BLOBS && !hash_seen)
                g_ld_profile.total.vs_post_boundary_distinct++;
        } else if (result == SHADER_BLOB_INSERT_FULL) {
            g_ld_profile.total.vs_blob_full_rejects++;
        }
    } else {
        if (result == SHADER_BLOB_INSERTED) {
            g_ld_profile.total.ps_blob_inserts++;
            if (count_before >= FORMER_MAX_SHADER_BLOBS && !hash_seen)
                g_ld_profile.total.ps_post_boundary_distinct++;
        } else if (result == SHADER_BLOB_INSERT_FULL) {
            g_ld_profile.total.ps_blob_full_rejects++;
        }
    }
#else
    (void)count_before;
    (void)result;
    (void)stage;
    (void)hash_seen;
#endif
}

static void shader_blob_cache_release(shader_blob_cache_t* cache)
{
    for (u32 i = 0; i < cache->count; i++) {
        shader_blob_t* entry = &cache->entries[i];
        if (entry->blob)
            entry->blob->lpVtbl->Release(entry->blob);
        free(entry->source);
    }
    memset(cache, 0, sizeof(*cache));
}

#if defined(YZ_PERF_PROFILE)
static void ld_profile_note_hlsl(
    const char* hlsl, u64 hashes[MAX_SHADER_BLOBS], u32* count, u64* unique)
{
    const size_t length = strlen(hlsl);
    const u64 hash = fnv1a(
        hlsl, (u32)length, 1469598103934665603ull);
    for (u32 i = 0; i < *count; i++)
        if (hashes[i] == hash)
            return;
    if (*count < MAX_SHADER_BLOBS)
        hashes[(*count)++] = hash;
    (*unique)++;
}

static int ld_profile_note_hash(
    u64 hash, u64 hashes[LD_VARIANT_SET_CAPACITY], u32* count)
{
    if (!hash)
        hash = 1;
    u32 slot = (u32)hash & (LD_VARIANT_SET_CAPACITY - 1u);
    for (u32 probe = 0; probe < LD_VARIANT_SET_CAPACITY; ++probe) {
        if (hashes[slot] == hash)
            return 0;
        if (hashes[slot] == 0) {
            hashes[slot] = hash;
            (*count)++;
            return 1;
        }
        slot = (slot + 1u) & (LD_VARIANT_SET_CAPACITY - 1u);
    }
    g_ld_profile.ps_variant_tracking_overflow++;
    return 0;
}

static void ld_profile_note_fp_variants(
    const u8* fp_uc, u32 fp_size, u32 fp_ctrl_key, u32 cube_mask,
    const render_state_t* state, u64 known_canonical)
{
    const u64 seed = 1469598103934665603ull;
    u64 raw_constant = fnv1a(fp_uc, fp_size, seed);
    u64 raw_exact = raw_constant;
    u64 canonical = known_canonical ? known_canonical : rsx_fp_structural_hash(fp_uc, fp_size, seed);
    if (!canonical)
        return;

    const u32 alpha_enable = state->alpha_test_enable ? 1u : 0u;
    const u32 alpha_func = alpha_enable ? state->alpha_func : 0u;
#define LD_HASH_FP_SOURCE_FIELD(value) do { \
    const u32 _field = (value); \
    raw_exact = fnv1a(&_field, sizeof(_field), raw_exact); \
    raw_constant = fnv1a( \
        &_field, sizeof(_field), raw_constant); \
    canonical = fnv1a(&_field, sizeof(_field), canonical); \
} while (0)
    LD_HASH_FP_SOURCE_FIELD(fp_ctrl_key);
    LD_HASH_FP_SOURCE_FIELD(cube_mask);
    LD_HASH_FP_SOURCE_FIELD(alpha_enable);
    LD_HASH_FP_SOURCE_FIELD(alpha_func);
#undef LD_HASH_FP_SOURCE_FIELD

    float alpha_ref = 0.0f;
    if (alpha_enable && alpha_func != 0x0207u) {
        alpha_ref = rsx_fp_alpha_ref(
            state->alpha_ref_raw, state->alpha_ref_format);
        raw_exact = fnv1a(
            &alpha_ref, sizeof(alpha_ref), raw_exact);
    }

    const int new_raw_constant = ld_profile_note_hash(
        raw_constant, g_ld_profile.ps_raw_constant_hashes,
        &g_ld_profile.n_ps_raw_constant_hashes);
    const int new_raw_exact = ld_profile_note_hash(
        raw_exact, g_ld_profile.ps_raw_exact_hashes,
        &g_ld_profile.n_ps_raw_exact_hashes);
    if (new_raw_constant || new_raw_exact) {
        u64 literal = rsx_fp_literal_source_hash(
            fp_uc, fp_size, seed);
        literal = fnv1a(
            &fp_ctrl_key, sizeof(fp_ctrl_key), literal);
        literal = fnv1a(
            &cube_mask, sizeof(cube_mask), literal);
        literal = fnv1a(
            &alpha_enable, sizeof(alpha_enable), literal);
        literal = fnv1a(
            &alpha_func, sizeof(alpha_func), literal);
        if (new_raw_constant)
            ld_profile_note_hash(
                literal,
                g_ld_profile.ps_constant_specialized_hashes,
                &g_ld_profile.n_ps_constant_specialized_hashes);
        if (new_raw_exact) {
            u64 exact_source = literal;
            if (alpha_enable && alpha_func != 0x0207u) {
                char alpha_literal[64];
                const int length = snprintf(
                    alpha_literal, sizeof(alpha_literal), "%.9g",
                    (double)alpha_ref);
                if (length > 0 &&
                    (u32)length < sizeof(alpha_literal))
                    exact_source = fnv1a(
                        alpha_literal, (u32)length, exact_source);
            }
            ld_profile_note_hash(
                exact_source, g_ld_profile.ps_exact_source_hashes,
                &g_ld_profile.n_ps_exact_source_hashes);
        }
    }
    ld_profile_note_hash(
        canonical, g_ld_profile.ps_canonical_hashes,
        &g_ld_profile.n_ps_canonical_hashes);
}
#endif

static u32 vp_txl_unit_mask(const u8* ucode, u32 instrs)
{
    u32 mask = 0;
    for (u32 i = 0; i < instrs; i++) {
        const u8* p = ucode + i * 16;
        const u32 d1 = (u32)p[4] | ((u32)p[5] << 8) |
                       ((u32)p[6] << 16) | ((u32)p[7] << 24);
        if (((d1 >> 22) & 0x1Fu) != 0x19u) continue;
        const u32 d2 = (u32)p[8] | ((u32)p[9] << 8) |
                       ((u32)p[10] << 16) | ((u32)p[11] << 24);
        mask |= 1u << ((d2 >> 8) & 3u);
    }
    return mask;
}

/* D3D12 exposes logical output-merger operations on UINT targets. Preserve
 * the original floating shader (including discard, alpha test and depth),
 * then perform the UNORM8 export explicitly before the integer operation. */
/* Flat shading (ld_flat_shading): the colour inputs are not interpolated. */
static void ld_fp_flat_inputs(char* source, size_t capacity)
{
    static const char smooth[] = "float4 col0 : COLOR0; float4 col1 : COLOR1;";
    static const char flat[] = "nointerpolation float4 col0 : COLOR0; nointerpolation float4 col1 : COLOR1;";
    char* at = strstr(source, smooth);
    const size_t length = strlen(source), grow = sizeof(flat) - sizeof(smooth);
    if (!at || length + grow + 1 > capacity) return;
    memmove(at + sizeof(flat) - 1, at + sizeof(smooth) - 1, length - (size_t)(at - source) - (sizeof(smooth) - 1) + 1);
    memcpy(at, flat, sizeof(flat) - 1);
}

static int ld_fp_logic_export(char* source, size_t capacity)
{
    char* entry = strstr(source, " main(PSInput input)");
    const size_t length = strlen(source);
    if (!entry) return -1;
    const int mrt = strstr(source, "struct PSOutput") != NULL;
    char wrapper[2048]; size_t used = 0;
#define LD_APPEND_LOGIC(...) do { \
        int n = snprintf(wrapper + used, sizeof(wrapper) - used, __VA_ARGS__); \
        if (n < 0 || (size_t)n >= sizeof(wrapper) - used) return -1; \
        used += (size_t)n; \
    } while (0)
    if (!mrt) {
        LD_APPEND_LOGIC("\nuint4 main(PSInput input) : SV_TARGET {\n"
                        "    return (uint4)round(saturate(lgfp(input)) * 255.0);\n}\n");
    } else {
        u32 targets = 0;
        const int depth = strstr(source, "float depth : SV_Depth;") != NULL;
        LD_APPEND_LOGIC("\nstruct LogicOutput {\n");
        for (u32 t = 0; t < 4; ++t) {
            char field[64];
            snprintf(field, sizeof(field), "float4 t%u : SV_TARGET%u;", t, t);
            if (strstr(source, field)) {
                targets |= 1u << t;
                LD_APPEND_LOGIC("    uint4 t%u : SV_TARGET%u;\n", t, t);
            }
        }
        if (!targets) return -1;
        if (depth) LD_APPEND_LOGIC("    float depth : SV_Depth;\n");
        LD_APPEND_LOGIC("};\nLogicOutput main(PSInput input) {\n"
                        "    PSOutput v = lgfp(input); LogicOutput result;\n");
        for (u32 t = 0; t < 4; ++t)
            if (targets & (1u << t))
                LD_APPEND_LOGIC("    result.t%u = (uint4)round(saturate(v.t%u) * 255.0);\n", t, t);
        if (depth) LD_APPEND_LOGIC("    result.depth = v.depth;\n");
        LD_APPEND_LOGIC("    return result;\n}\n");
    }
#undef LD_APPEND_LOGIC
    if (length + used + 1 > capacity) return -1;
    memcpy(entry + 1, "lgfp", 4);
    memcpy(source + length, wrapper, used + 1);
    return 0;
}

static ID3D12PipelineState* build_pso(
    const char* vs_hlsl, const char* ps_hlsl, const render_state_t* rs,
    const rsx_vertex_layout_plan* masked_layout, int packed_offsets)
{
    ID3DBlob *vs = NULL, *ps = NULL, *err = NULL;
    static u32 compile_fail_logs = 0;
    static u32 create_fail_logs = 0;
    const u32 vs_length = (u32)strlen(vs_hlsl);
    const u32 ps_length = (u32)strlen(ps_hlsl);
    const u64 vs_hash = fnv1a(
        vs_hlsl, vs_length, 1469598103934665603ull);
    const u64 ps_hash = fnv1a(
        ps_hlsl, ps_length, 1469598103934665603ull);
#if defined(YZ_PERF_PROFILE)
    ld_profile_note_hlsl(
        vs_hlsl, g_ld_profile.vs_hashes, &g_ld_profile.n_vs_hashes,
        &g_ld_profile.total.vs_unique);
    ld_profile_note_hlsl(
        ps_hlsl, g_ld_profile.ps_hashes, &g_ld_profile.n_ps_hashes,
        &g_ld_profile.total.ps_unique);
    g_ld_profile.total.vs_blob_lookups++;
    const LONGLONG vs_lookup_begin = ld_profile_qpc();
#endif
    int vs_post_boundary_hit = 0;
    int vs_hash_seen = 0;
    vs = shader_blob_cache_find(
        &g.vs_blobs, vs_hlsl, vs_length, vs_hash,
        &vs_post_boundary_hit, &vs_hash_seen);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.vs_blob_lookup_qpc +=
        (u64)(ld_profile_qpc() - vs_lookup_begin);
#endif
    HRESULT vs_hr = S_OK;
    if (vs) {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.vs_blob_hits++;
        if (vs_post_boundary_hit)
            g_ld_profile.total.vs_post_boundary_repeats++;
#endif
    } else {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.vs_blob_misses++;
#endif
        vs = shader_disk_cache_load('V', vs_hlsl, vs_length, vs_hash);
        if (vs) {
            shader_blob_cache_insert_accounted(
                &g.vs_blobs, vs_hlsl, vs_length, vs_hash, vs,
                'V', vs_hash_seen);
        } else {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.vs_compile_calls++;
            const LONGLONG compile_begin = ld_profile_qpc();
#endif
            vs_hr = D3DCompile(
                vs_hlsl, vs_length, "xvs", NULL, NULL, "main",
                "vs_5_0", 0, 0, &vs, &err);
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.vs_compile_qpc +=
                (u64)(ld_profile_qpc() - compile_begin);
#endif
            if (SUCCEEDED(vs_hr)) {
                shader_disk_cache_store(
                    'V', vs_hlsl, vs_length, vs_hash, vs);
                shader_blob_cache_insert_accounted(
                    &g.vs_blobs, vs_hlsl, vs_length, vs_hash, vs,
                    'V', vs_hash_seen);
            }
        }
    }
    if (FAILED(vs_hr)) {
        if (PS3_LOG_FIRST(compile_fail_logs, 32)) {
            const char* msg = err ? (const char*)err->lpVtbl->GetBufferPointer(err)
                                  : "no compiler diagnostic";
            fprintf(stderr, "[pso-fail] vertex compile: %.768s\n", msg);
        }
        if (err) err->lpVtbl->Release(err); return NULL;
    }
    if (err) {
        err->lpVtbl->Release(err);
        err = NULL;
    }
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.ps_blob_lookups++;
    const LONGLONG ps_lookup_begin = ld_profile_qpc();
#endif
    int ps_post_boundary_hit = 0;
    int ps_hash_seen = 0;
    ps = shader_blob_cache_find(
        &g.ps_blobs, ps_hlsl, ps_length, ps_hash,
        &ps_post_boundary_hit, &ps_hash_seen);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.ps_blob_lookup_qpc +=
        (u64)(ld_profile_qpc() - ps_lookup_begin);
#endif
    HRESULT ps_hr = S_OK;
    if (ps) {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.ps_blob_hits++;
        if (ps_post_boundary_hit)
            g_ld_profile.total.ps_post_boundary_repeats++;
#endif
    } else {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.ps_blob_misses++;
#endif
        ps = shader_disk_cache_load('P', ps_hlsl, ps_length, ps_hash);
        if (ps) {
            shader_blob_cache_insert_accounted(
                &g.ps_blobs, ps_hlsl, ps_length, ps_hash, ps,
                'P', ps_hash_seen);
        } else {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.ps_compile_calls++;
            const LONGLONG compile_begin = ld_profile_qpc();
#endif
            ps_hr = D3DCompile(
                ps_hlsl, ps_length, "xps", NULL, NULL, "main",
                "ps_5_0", 0, 0, &ps, &err);
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.ps_compile_qpc +=
                (u64)(ld_profile_qpc() - compile_begin);
#endif
            if (SUCCEEDED(ps_hr)) {
                shader_disk_cache_store(
                    'P', ps_hlsl, ps_length, ps_hash, ps);
                shader_blob_cache_insert_accounted(
                    &g.ps_blobs, ps_hlsl, ps_length, ps_hash, ps,
                    'P', ps_hash_seen);
            }
        }
    }
    if (FAILED(ps_hr)) {
        if (PS3_LOG_FIRST(compile_fail_logs, 32)) {
            const char* msg = err ? (const char*)err->lpVtbl->GetBufferPointer(err)
                                  : "no compiler diagnostic";
            fprintf(stderr, "[pso-fail] pixel compile: %.768s\n", msg);
        }
        if (err) err->lpVtbl->Release(err);
        vs->lpVtbl->Release(vs); return NULL;
    }
    if (err) {
        err->lpVtbl->Release(err);
        err = NULL;
    }
    D3D12_INPUT_ELEMENT_DESC il[16];
    const u32 input_count = packed_offsets==2 ? 0 :
        (masked_layout ? masked_layout->count : RSX_DSP_NUM_VERTEX_ATTR);
    for (u32 slot = 0; slot < input_count; slot++) {
        const u32 attr =
            masked_layout ? masked_layout->attrs[slot] : slot;
        D3D12_INPUT_ELEMENT_DESC e = {
            "ATTR", attr, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
            masked_layout && !packed_offsets ? attr * 16u : slot * 16u,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        };
        il[slot] = e;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = g.rootsig_x;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
    pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pd.InputLayout.pInputElementDescs = input_count ? il : NULL;
    pd.InputLayout.NumElements = input_count;
    apply_render_state(&pd, rs);
    pd.SampleMask = 0xFFFFFFFFu;
    pd.PrimitiveTopologyType = g.rsx.current_primitive == RSX_PRIMITIVE_LINE_STRIP
        ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = rs->rt_count ? rs->rt_count : 1;
    for (u32 r = 0; r < pd.NumRenderTargets; r++)
        pd.RTVFormats[r] = rs->rt_fp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT
            : rs->logic_enable ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.create_pso_calls++;
    const LONGLONG create_begin = ld_profile_qpc();
#endif
    HRESULT hr = g.dev->lpVtbl->CreateGraphicsPipelineState(
        g.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.create_pso_qpc +=
        (u64)(ld_profile_qpc() - create_begin);
#endif
    if (FAILED(hr)) {
        const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
        if (PS3_LOG_FIRST(create_fail_logs, 32))
            fprintf(stderr,
                    "[pso-fail] create hr=0x%08lX removed=0x%08lX "
                    "depth{test=%u write=%u func=%u} blend=%u "
                    "cull{enable=%u face=%u}\n",
                    (unsigned long)hr, (unsigned long)removed,
                    rs->depth_test, rs->depth_write, rs->depth_func,
                    rs->blend_enable, rs->cull_enable, rs->cull_face);
        if (removed != S_OK)
            ld_dump_dred("CreateGraphicsPipelineState", hr);
    }
    vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps);
    return SUCCEEDED(hr) ? pso : NULL;
}

static rsx_np_cache g_native_vp, g_native_fp;
static rsx_np_record* g_native_current_vp;
static u64 g_native_program_verified;
static int ld_native_programs(void)
{
    static int enabled=-1;
    if (enabled<0) { const char* e=getenv("RSX_NATIVE_PROGRAMS"); enabled=e && e[0]=='1'; }
    return enabled;
}
static int ld_native_program_verify(void)
{
    static int enabled=-1;
    if (enabled<0) { const char* e=getenv("RSX_NATIVE_PROGRAM_VERIFY"); enabled=e && e[0]=='1'; }
    return enabled;
}
static void ld_native_graphics_report(void)
{
    rsx_rec_drain();
    rsx_rec_report();
    if (rsx_nb_enabled()) fprintf(stderr,"[native-bindings] emitted=%llu elided=%llu\n",
        g_native_binding_emitted,g_native_binding_elided);
    if (ld_native_programs()) fprintf(stderr,
        "[native-program] vp_hits=%llu vp_builds=%llu vp_shared=%llu vp_invalid=%llu "
        "fp_hits=%llu fp_builds=%llu fp_shared=%llu fp_invalid=%llu rejected=%llu verified=%llu\n",
        g_native_vp.hits,g_native_vp.builds,g_native_vp.shared,g_native_vp.invalidations,
        g_native_fp.hits,g_native_fp.builds,g_native_fp.shared,g_native_fp.invalidations,
        g_native_fp.rejected,g_native_program_verified);
}
typedef struct ld_native_fp_address { u32 location, offset; } ld_native_fp_address;
static const u8* ld_native_fp_resolve(void* context, u32 bytes)
{
    const ld_native_fp_address* a=(const ld_native_fp_address*)context;
    return guest_ptr(a->location,a->offset,bytes);
}

static int ld_current_vertex_layout(rsx_vertex_layout_plan* layout)
{
    g_native_current_vp=NULL;
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR) {
        ld_layout_plan_get(0xFFFFu, layout);
        return 0;
    }
    const u8* vp_uc = (const u8*)(g.rsx.vp + start * 4);
    if (ld_native_programs()) {
        g_native_current_vp=rsx_np_vp(&g_native_vp,start,vp_uc,(RSX_DSP_VP_INSTR-start)*16);
        if (g_native_current_vp) {
            ld_layout_plan_get(g_native_current_vp->input_mask,layout);
            return 1;
        }
    }
    const u32 vp_instrs = rsx_vp_program_size_instrs(
        vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
    rsx_vp_input_analysis analysis = {0xFFFFu, 0};
    if (!vp_instrs ||
        rsx_vp_analyze_inputs(
            vp_uc, vp_instrs * 16, &analysis) != (int)vp_instrs ||
        !analysis.exact)
        analysis.input_mask = 0xFFFFu;
    ld_layout_plan_get(analysis.input_mask, layout);
    return vp_instrs != 0;
}

/* Ultrawide (rsx_aspect.h): the constant block c[n..n+3] a vertex program
 * multiplies ATTR0 by for its position -- o[0].x..w = dp4(v[0], c[n+0..3]) as
 * the only writes of o[0] -- or -1. Nearly every Bleach program has this form
 * (c0..c3, the effect programs c221..c224), so a draw's position matrix can be
 * read on the CPU: ld_aspect_place tells 2D (affine) from 3D by its w row. */
static int ld_vp_position_block(const char* hlsl)
{
    static const char lead[] = "vp_dot4(((v[0]).xyzw),((vp_c[", lanes[] = "xyzw";
    int rows[4] = {-1, -1, -1, -1}, writes = 0;
    /* libfont's text program (the firmware's vector font): position =
     * c256..c259 x (c260..c263, translation offset by c467.xy) x
     * (v.x, v.y, c467.z, v.w) -- ld_aspect_place follows it as LD_VP_LIBFONT. */
    if (strstr(hlsl, "vp_c[467]") && strstr(hlsl, "vp_c[263]") && strstr(hlsl, "vp_c[256]") &&
        strstr(hlsl, "vp_c[259]") && strstr(hlsl, "vp_c[260]") && strstr(hlsl, "vp_c[261]"))
        return -2;
    /* The rows may go through a temporary first: r[K].x..w = dp4(...), then
     * o[0].xyzw = r[K] as the only write of o[0] (the menus' hexagon floor). */
    char target[8] = "o[0].";
    for (const char* p = strstr(hlsl, "o[0]."); p; p = strstr(p + 5, "o[0].")) ++writes;
    if (writes == 1) {
        const char* copy = strstr(hlsl, "o[0].xyzw = _v.xyzw;");
        const char* from = copy ? strstr(hlsl, "(float4)((r[") : NULL;
        const char* last = NULL;   /* the temporary copied right before the write */
        for (const char* p = from; p && p < copy; p = strstr(p + 1, "(float4)((r[")) last = p;
        int k = -1; char tail[16] = {0};
        if (!last || sscanf(last, "(float4)((r[%d]).xyzw%15s", &k, tail) != 2 || k < 0 || k > 31 ||
            strncmp(tail, ");", 2)) return -1;
        snprintf(target, sizeof target, "r[%d].", k);
        writes = 0;
        for (const char* p = strstr(hlsl, target); p; p = strstr(p + 1, target))
            if (p[strlen(target) + 1] == ' ' && p[strlen(target) + 2] == '=') ++writes;
    }
    if (writes != 4) return -1;
    const size_t tl = strlen(target);
    for (const char* p = strstr(hlsl, lead); p; p = strstr(p + 1, lead)) {
        char* end = NULL;
        const long n = strtol(p + sizeof(lead) - 1, &end, 10);
        if (!end || strncmp(end, "]).xyzw)));", 11)) continue;
        const char* q = end + 11;
        while (*q == '\r' || *q == '\n' || *q == ' ') ++q;
        const char* lane = strncmp(q, target, tl) || !q[tl] || q[tl + 1] != ' ' ? NULL : strchr(lanes, q[tl]);
        if (lane) rows[lane - lanes] = (int)n;
    }
    for (int i = 0; i < 4; ++i) if (rows[i] != rows[0] + i) return -1;
    return rows[0] >= 0 && rows[0] + 3 < 512 ? rows[0] : -1;
}

/* Ultrawide (rsx_aspect.h): k = display aspect / (16/9), 1 when off. */
static float g_ld_aspect_k = 1.0f;
/* With k != 1 every vertex program gets one line after its viewport epilogue:
 * a draw whose position the CPU cannot follow (ld_aspect_place, per_vertex:
 * libfont's text) sets vp_aspect1.x = 1/k and its zones in vp_aspect0, and
 * each of its vertices with w = 1 -- 2D -- goes to the edge or the centre its
 * zone says; perspective vertices (w = view depth, at least the near plane)
 * are left alone, or, when vp_aspect1.y is set, get x * vp_aspect1.y +
 * vp_aspect1.w and y * vp_aspect1.z (NDC): squeezed on a screen whose 3D is
 * not widened (flat_3d), placed with a screen's figure. vp_aspect1 is 0 for
 * every other draw. At 16:9 the HLSL,
 * and so the shader cache, is untouched. */
static int ld_hlsl_insert_after(char* hlsl, size_t capacity, const char* anchor, const char* text)
{
    char* at = strstr(hlsl, anchor);
    const size_t length = strlen(hlsl), extra = strlen(text);
    if (!at || length + extra + 1 > capacity) return 0;
    at += strlen(anchor);
    memmove(at + extra, at, length - (size_t)(at - hlsl) + 1);
    memcpy(at, text, extra);
    return 1;
}
static void ld_vp_aspect_epilogue(char* hlsl, size_t capacity)
{
    if (g_ld_aspect_k == 1.0f) return;
    /* vp_aspect0 = zones (NDC), vp_aspect1.x = 1/k for a text draw, else 0:
     * the 2 float4s after vp_posoffset, in the constant block's padding. */
    char* fields = strstr(hlsl, "float4 vp_posoffset;");
    if (!fields || strstr(hlsl, "vp_aspect1")) return;
    if (!ld_hlsl_insert_after(hlsl, capacity, "float4 vp_posoffset;",
                              "\n    float4 vp_aspect0;\n    float4 vp_aspect1;")) return;
    ld_hlsl_insert_after(hlsl, capacity,
        "Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);",
        "\n    if (vp_aspect1.x != 0.0 && _p.w == 1.0) {"
        "\n        float _ax = Out.pos.x;"
        "\n        float _sh = _ax < vp_aspect0.x ? vp_aspect1.x - 1.0"
        " : ((_ax > vp_aspect0.y || (_ax > vp_aspect0.w && Out.pos.y < vp_aspect0.z)) ? 1.0 - vp_aspect1.x : 0.0);"
        "\n        Out.pos.x = _ax * vp_aspect1.x + _sh;"
        "\n    }"
        "\n    if (vp_aspect1.y != 0.0 && _p.w != 1.0) {"
        "\n        Out.pos.x = Out.pos.x * vp_aspect1.y + vp_aspect1.w * Out.pos.w;"
        "\n        Out.pos.y *= vp_aspect1.z;"
        "\n    }");
}

/* LD_PSO_DBG=1: name the first bail-out of each kind in get_pso(). Every exit
 * there returns NULL silently and the caller only counts drop{pso=N}, so a
 * title whose draws ALL die in this function has no way to say which gate
 * rejected them. One line per distinct reason, so a stuck title prints a
 * handful of lines rather than one per draw. */
static void ld_pso_bail(int* said, const char* reason)
{
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("LD_PSO_DBG") ? 1 : 0;
    if (!dbg || *said) return;
    *said = 1;
    fprintf(stderr, "[pso-bail] %s\n", reason);
    fflush(stderr);
}
#define LD_PSO_BAIL(reason) \
    do { static int _said_ = 0; ld_pso_bail(&_said_, (reason)); } while (0)

/* The fragment texture units as get_pso() decoded them for the current draw;
 * the rest of the draw's preparation reads them from here. */
static rsx_dsp_texture g_ld_draw_textures[RSX_DSP_NUM_TEXTURES];

static ID3D12PipelineState* get_pso(
    const rsx_vertex_layout_plan* masked_layout, int packed_payload)
{
    memset(&g_ld_current_pso, 0, sizeof(g_ld_current_pso));
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR) { LD_PSO_BAIL("no vertex program uploaded (vp start out of range)"); return NULL; }
    const u8* vp_uc = (const u8*)(g.rsx.vp + start * 4);
    const rsx_np_record* native_vp=ld_native_programs() ? g_native_current_vp : NULL;
    const u32 vp_instrs = native_vp ? native_vp->size/16 :
        rsx_vp_program_size_instrs(vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
    if (!vp_instrs) { LD_PSO_BAIL("vertex program has zero instructions"); return NULL; }

    u32 fp_loc = 0;
    const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
    const u8* fp_uc = guest_ptr(fp_loc, fp_off, 16);
    if (!fp_uc) { LD_PSO_BAIL("fragment program address does not resolve to guest memory"); return NULL; }
    const ld_native_fp_address fp_address={fp_loc,fp_off};
    rsx_np_record* native_fp=ld_native_programs() ? rsx_np_fp(&g_native_fp,
        ((u64)fp_loc<<32)|fp_off,ld_native_fp_resolve,(void*)&fp_address) : NULL;
    const u32 fp_size = ld_native_programs() ? (native_fp ? native_fp->size : 0) :
        rsx_fp_program_size(fp_uc, 0x10000);
    if (!fp_size) {
        /* No END-bit instruction in 64 KB: the bytes at (location, offset) are
         * not a fragment program. Almost always the LOCATION is wrong, not the
         * offset -- the same class of bug caner hit in Yakuza with textures --
         * so report the register the guest actually wrote and the head of what
         * we read, which is what says local-vs-main. */
        static int said = 0;
        if (!said && getenv("LD_PSO_DBG")) {
            said = 1;
            fprintf(stderr,
                    "[pso-bail] fragment program size reads as zero: "
                    "reg=0x%08X -> loc=%u off=0x%08X, first words "
                    "%08X %08X %08X %08X\n",
                    rsx_dsp_reg(&g.rsx, 0x08E4 /* M_FP_ACTIVE_PROGRAM */), fp_loc, fp_off,
                    rsx_fp_read_word(fp_uc + 0), rsx_fp_read_word(fp_uc + 4),
                    rsx_fp_read_word(fp_uc + 8), rsx_fp_read_word(fp_uc + 12));
            /* All-zero here has two very different causes: the offset resolved
             * into the wrong memory, or nothing ever wrote the buffer. Scan
             * forward for the first non-zero byte -- a hit a few hundred bytes
             * on means the region IS populated and the offset is wrong; no hit
             * across 64 KB means the producer never ran. */
            {
                const u8* q = guest_ptr(fp_loc, fp_off, 0x10000);
                u32 i = 0;
                if (q) { while (i < 0x10000u && !q[i]) i++; }
                if (!q)
                    fprintf(stderr, "[pso-bail]   (64 KB window not mapped)\n");
                else if (i == 0x10000u)
                    fprintf(stderr, "[pso-bail]   64 KB from here is entirely "
                                    "zero -- nothing ever wrote this buffer\n");
                else
                    fprintf(stderr, "[pso-bail]   first non-zero byte at "
                                    "+0x%X -- the region is populated, the "
                                    "offset is wrong\n", i);
            }
            fflush(stderr);
        }
        return NULL;
    }
    /* re-resolve with the true size to validate the whole program is mapped */
    fp_uc = guest_ptr(fp_loc, fp_off, fp_size);
    if (!fp_uc) { LD_PSO_BAIL("fragment program is not fully mapped in guest memory"); return NULL; }

    /* Fragment output register mode (fp16 h0 vs fp32 r0) is driven by the
     * SHADER_CONTROL word bit 0x40 (same fix as the replay harness â€” the
     * AUTO heuristic returned stale fp32 scratch for h0-writing materials);
     * fold the deciding bit into the cache key so a program reused under a
     * different export mode gets its own PSO. Kill-switch RSX_FP_CTRL_AUTO=1
     * restores the old heuristic for the A/B. */
    static int ctrl_auto = -1;
    if (ctrl_auto < 0) ctrl_auto = getenv("RSX_FP_CTRL_AUTO") ? 1 : 0;
    const u32 fp_ctrl = ctrl_auto ? RSX_FP_CTRL_AUTO : rsx_dsp_shader_control(&g.rsx);
    u32 cube_mask = 0;
    for (u32 unit = 0; unit < RSX_DSP_NUM_TEXTURES; unit++) {
        rsx_dsp_get_texture(&g.rsx, unit, &g_ld_draw_textures[unit]);
        if (g_ld_draw_textures[unit].enabled && g_ld_draw_textures[unit].cubemap)
            cube_mask |= 1u << unit;
    }
    const u32 vtex_mask = vertex_texture_mask();
    const u32 txl_mask = native_vp ? native_vp->txl_mask : vp_txl_unit_mask(vp_uc, vp_instrs);
    if (txl_mask && !(txl_mask & vtex_mask)) {
        g_ld_vtex_missing_for_txl++;
        static u32 warned = 0;
        if (PS3_LOG_FIRST(warned, 32) && vertex_texture_trace_enabled()) {
            fprintf(stderr,
                    "[vtex] TXL shader has no supported binding txl=0x%X "
                    "bound=0x%X start=%u instrs=%u\n",
                    txl_mask, vtex_mask, start, vp_instrs);
            for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
                if (!((txl_mask >> u) & 1u)) continue;
                const u32 base = 0x0900 + u * 0x20;
                fprintf(stderr,
                        "[vtex] raw u%u off=%08X fmt=%08X wrap=%08X "
                        "ctl0=%08X ctl3=%08X filter=%08X rect=%08X\n",
                        u, rsx_dsp_reg(&g.rsx, base),
                        rsx_dsp_reg(&g.rsx, base + 4),
                        rsx_dsp_reg(&g.rsx, base + 8),
                        rsx_dsp_reg(&g.rsx, base + 12),
                        rsx_dsp_reg(&g.rsx, base + 16),
                        rsx_dsp_reg(&g.rsx, base + 20),
                        rsx_dsp_reg(&g.rsx, base + 24));
            }
        }
    }

    render_state_t rs;
    decode_render_state(&rs);
    if (native_fp) rsx_np_constants(native_fp,fp_uc,&g.fp_constants);
    else if (rsx_fp_collect_constants(
            fp_uc, fp_size, &g.fp_constants) < 0) {
        LD_PSO_BAIL("fragment program constant collection failed");
        return NULL;
    }
    g.fp_alpha_ref = rsx_fp_alpha_ref(
        rs.alpha_ref_raw, rs.alpha_ref_format);

    const u64 vp_hash = native_vp ? native_vp->hash : fnv1a(
        vp_uc, vp_instrs * 16, 1469598103934665603ull);
    u64 key=vp_hash;
    if (ld_fp_constants_buffered())
        key = native_fp ? rsx_np_fp_hash(native_fp,key) : rsx_fp_structural_hash(fp_uc, fp_size, key);
    else
        key = fnv1a(fp_uc, fp_size, key);
    if (!key) {
        LD_PSO_BAIL("shader hash collapsed to zero");
        return NULL;
    }
    /* Depth comparison is shader state, not merely a sampler filter. The
     * native depth snapshot holds raw R32 depth; TEX/TXP must compare its
     * reference before PCF, using the same shared translation as replay. */
    rsx_fp_shadow_desc shadow[16] = {{0}};
    u32 shadow_mask = 0;
    const u32 sampled_units = native_fp ? native_fp->sampled_units : rsx_fp_texture_units(fp_uc, fp_size);
    if (native_fp && native_vp && ld_native_program_verify()) {
        rsx_fp_constant_block oracle;
        rsx_vp_input_analysis inputs={0xFFFFu,0};
        if (rsx_vp_analyze_inputs(vp_uc,vp_instrs*16,&inputs)!=(int)vp_instrs || !inputs.exact)
            inputs.input_mask=0xFFFFu;
        const int constants=rsx_fp_collect_constants(fp_uc,fp_size,&oracle);
        if (constants<0 || oracle.count!=g.fp_constants.count ||
            memcmp(oracle.values,g.fp_constants.values,oracle.count*16) ||
            rsx_vp_program_size_instrs(vp_uc,(RSX_DSP_VP_INSTR-start)*16)!=vp_instrs ||
            rsx_fp_program_size(fp_uc,fp_size)!=fp_size ||
            inputs.input_mask!=native_vp->input_mask ||
            vp_txl_unit_mask(vp_uc,vp_instrs)!=txl_mask ||
            fnv1a(vp_uc,vp_instrs*16,1469598103934665603ull)!=vp_hash ||
            rsx_fp_texture_units(fp_uc,fp_size)!=sampled_units ||
            rsx_fp_structural_hash(fp_uc,fp_size,vp_hash)!=rsx_np_fp_hash(native_fp,vp_hash)) {
            fprintf(stderr,"[native-program] MISMATCH loc=%u offset=%08X\n",fp_loc,fp_off);
            abort();
        }
        ++g_native_program_verified;
    }
    static int shadow_on = -1;
    if (shadow_on < 0) { const char* e = getenv("BLEACH_SHADOW_COMPARE"); shadow_on = !(e && e[0]=='0'); }
    if (shadow_on) for (u32 u=0; u<16; ++u) {
        if (!(sampled_units & (1u << u))) continue;
        const rsx_dsp_texture t = g_ld_draw_textures[u];
        rsx_fp_shadow_desc_from_texture(&shadow[u],t.format,t.dimension,t.cubemap,
            t.mipmaps,t.filter,t.wrap,t.control0);
        if (shadow[u].enabled) shadow_mask |= 1u << u;
    }
    if (shadow_mask) {
        key = fnv1a(&shadow_mask,sizeof(shadow_mask),key);
        key = fnv1a(shadow,sizeof(shadow),key);
    }
    const u32 fp_ctrl_key = fp_ctrl & 0x4Eu;   /* 32-bit exports, depth export */
    key = fnv1a(&fp_ctrl_key, sizeof(fp_ctrl_key), key);
    key = fnv1a(&cube_mask, sizeof(cube_mask), key);
    key = fnv1a(&vtex_mask, sizeof(vtex_mask), key);
    /* Texel-addressed (RSX_TEX_FMT_UNNORM) units need their coordinates divided
     * by the texture size in the shader -- D3D12 has no unnormalised addressing
     * mode. The divisor is baked into the HLSL, so the sizes are part of the
     * PSO identity and must be in the key. */
    u32 unnorm_mask = 0;
    u32 unnorm_dim[16][2] = {{0}};
    for (u32 uu = 0; uu < 16; uu++) {
        const rsx_dsp_texture ut = g_ld_draw_textures[uu];
        if (!ut.enabled || !(ut.format & TEX_FMT_UNNORM) || (shadow_mask & (1u<<uu))) continue;
        if (!ut.width || !ut.height) continue;
        unnorm_mask |= 1u << uu;
        unnorm_dim[uu][0] = ut.width;
        unnorm_dim[uu][1] = ut.height;
        /* A depth texture bound to a zeta's snapshot reads that host
         * resource, which is as large as the largest pass that used the zeta
         * (bounded by the zeta), not as large as the texture. Texel
         * coordinates address the resource, so divide by its size: a texture
         * smaller than the zeta then reads its top-left corner, as it does in
         * VRAM, instead of a stretched copy of the whole depth buffer. */
        if ((ut.format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM) ==
                TEX_FMT_DEPTH24_D8) {
            for (u32 zi = 0; zi < g.n_zdepths; zi++) {
                const zdepth_t* z = &g.zdepths[zi];
                if (z->location != ut.location || z->offset != ut.offset)
                    continue;
                /* Match the authoritative GPU snapshot, whether produced by
                 * geometry or an explicit depth clear. */
                if (z->snapshot && (z->had_write || z->snapshot_valid) &&
                    z->snapshot_w >= ut.width &&
                    z->snapshot_h >= ut.height) {
                    unnorm_dim[uu][0] = z->snapshot_w;
                    unnorm_dim[uu][1] = z->snapshot_h;
                }
                break;
            }
        }
    }
    key = fnv1a(&unnorm_mask, sizeof(unnorm_mask), key);
    if (unnorm_mask) key = fnv1a(unnorm_dim, sizeof(unnorm_dim), key);
    if (masked_layout) {
        static const u32 masked_layout_tag = 0x314B534Du; /* "MSK1" */
        const u32 payload_stride =
            packed_payload ? masked_layout->stride : VERT_STRIDE;
        key = fnv1a(
            &masked_layout_tag, sizeof(masked_layout_tag), key);
        key = fnv1a(
            &masked_layout->mask, sizeof(masked_layout->mask), key);
        key = fnv1a(
            &payload_stride, sizeof(payload_stride), key);
        key = fnv1a(
            &packed_payload, sizeof(packed_payload), key);
    }
    if (ld_fp_constants_buffered())
        key = ld_hash_structural_render_state(&rs, key);
    else
        key = fnv1a(&rs, sizeof(rs), key);
    /* Line and triangle rasterization require different D3D12 PSOs even
     * when every shader and render-state register is otherwise identical. */
    const u32 line_topology = g.rsx.current_primitive == RSX_PRIMITIVE_LINE_STRIP;
    key = fnv1a(&line_topology, sizeof(line_topology), key);
    const u32 flat_shading = ld_flat_shading();
    if (flat_shading) key = fnv1a(&flat_shading, sizeof(flat_shading), key ^ 0x666c6174ull);
    /* SV_Position -> WPOS divides by the colour target's scale (ld_target_scale). */
    rsx_resolution_domain target_domain = g.resolution;
    {
        rsx_dsp_surface target_sf;
        rsx_dsp_get_surface(&g.rsx, &target_sf);
        target_domain.scale = ld_draw_target_scale(&target_sf);
        if (target_domain.scale != g.resolution.scale)
            key = fnv1a(&target_domain.scale, sizeof(target_domain.scale), key);
    }
#if defined(YZ_PERF_PROFILE)
    ld_profile_note_fp_variants(
        fp_uc, fp_size, fp_ctrl_key, cube_mask, &rs, native_fp ? native_fp->hash : 0);
#endif
    g_ld_current_pso.valid = 1;
    g_ld_current_pso.key = key;
    g_ld_current_pso.vp_hash = vp_hash;
    g_ld_current_pso.vp_start = start;
    g_ld_current_pso.vp_instrs = vp_instrs;
    g_ld_current_pso.fp_location = fp_loc;
    g_ld_current_pso.fp_offset = fp_off;
    g_ld_current_pso.fp_size = fp_size;
    g_ld_current_pso.fp_control = fp_ctrl;
    g_ld_current_pso.cube_mask = cube_mask;
    g_ld_current_pso.vtex_mask = vtex_mask;
    g_ld_current_pso.txl_mask = txl_mask;
    g_ld_current_pso.input_mask =
        masked_layout ? masked_layout->mask : 0xFFFFu;
    g_ld_current_pso.input_stride =
        masked_layout && packed_payload
            ? masked_layout->stride : VERT_STRIDE;
    g_ld_current_pso.packed_offsets =
        masked_layout && packed_payload;

#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.pso_lookups++;
#endif
    /* The PSO key already includes the complete structural state. Retain the
     * same equality rule, but index it instead of scanning every prior PSO. */
    const u32 index_mask = MAX_PSOS * 2u - 1u;
    u32 bucket = (u32)(key ^ (key >> 32)) & index_mask;
    while (g.pso_index[bucket]) {
        const u32 i = g.pso_index[bucket] - 1u;
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.pso_probes++;
#endif
        if (g.psos[i].key == key) {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.pso_hits++;
#endif
            g_ld_current_pso.resources = g.psos[i].resources;
            return g.psos[i].pso;
        }
        bucket = (bucket + 1u) & index_mask;
    }
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.pso_misses++;
#endif
    if (g.n_psos >= MAX_PSOS) {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.pso_full++;
#endif
        LD_PSO_BAIL("PSO cache full (MAX_PSOS)");
        return NULL;
    }

    static char vs_hlsl[256 * 1024];
    static char ps_hlsl[256 * 1024];
    ID3D12PipelineState* pso = NULL;
#if defined(YZ_PERF_PROFILE)
    const LONGLONG decompile_begin = ld_profile_qpc();
#endif
    rsx_vp_set_branch_base(rsx_dsp_vp_start(&g.rsx));
    const int vi = masked_layout
        ? rsx_vp_decompile_compact_ex(
            vp_uc, vp_instrs * 16, vtex_mask, masked_layout->mask,
            vs_hlsl, sizeof(vs_hlsl))
        : rsx_vp_decompile_ex(
            vp_uc, vp_instrs * 16, vtex_mask, vs_hlsl, sizeof(vs_hlsl));
    if (vi > 0) ld_vp_aspect_epilogue(vs_hlsl, sizeof(vs_hlsl));
    u32 decompiled_constant_count = 0;
    int fi;
    if (ld_fp_constants_buffered()) {
        fi = rsx_fp_decompile_buffered_shadow_ex(
            fp_uc, fp_size, fp_ctrl, cube_mask, shadow, ps_hlsl,
            sizeof(ps_hlsl), &decompiled_constant_count);
        if (fi > 0 &&
            decompiled_constant_count != g.fp_constants.count)
            fi = -1;
        if (fi > 0 && rs.alpha_test_enable &&
            rsx_fp_apply_alpha_test_buffered(
                ps_hlsl, sizeof(ps_hlsl), rs.alpha_func) < 0)
            fi = -1;
        if (fi > 0) {
            const int np = rsx_fp_apply_unnorm_scale(
                ps_hlsl, sizeof(ps_hlsl), unnorm_mask, unnorm_dim, cube_mask);
            { static int n = 0;
              if (PS3_LOG_FIRST(n, 8))
                  fprintf(stderr, "[unnorm] mask=0x%X cube=0x%X dim0=%ux%u"
                                  " patched=%d\n",
                          unnorm_mask, cube_mask, unnorm_dim[0][0],
                          unnorm_dim[0][1], np); }
            if (np < 0) fi = -1;
        }
    } else {
        fi = rsx_fp_decompile_shadow_ex(
            fp_uc, fp_size, fp_ctrl, cube_mask, shadow,
            ps_hlsl, sizeof(ps_hlsl));
        if (fi > 0 && rs.alpha_test_enable &&
            rsx_fp_apply_alpha_test(
                ps_hlsl, sizeof(ps_hlsl), rs.alpha_func,
                g.fp_alpha_ref) < 0)
            fi = -1;
        if (fi > 0 && rsx_fp_apply_unnorm_scale(
                ps_hlsl, sizeof(ps_hlsl), unnorm_mask,
                unnorm_dim, cube_mask) < 0)
            fi = -1;
    }
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.decompile_qpc +=
        (u64)(ld_profile_qpc() - decompile_begin);
#endif
    /* LD_FORCE_GREEN=1: make every fragment program return solid green.
     *
     * The same trick as LD_CLEAR_TEST, applied one stage later. Clears are
     * visible on these surfaces and draws are not, while the engine reports
     * 18,452 groups executed with zero drops and the GPU state cannot discard
     * anything (viewport 0,0 720x512, scissor full, depth test off, colour mask
     * all channels, blend off, cull off). Two possibilities remain and they need
     * opposite fixes: the draws reach the GPU and the SAMPLED DATA is black, or
     * they never reach it and the accounting is wrong. Green separates them. */
    { static int fg = -1;
      /* LD_FORCE_GREEN=<pso key hex> limits it to one PSO. */
      static u64 gk = 0;
      if (fg < 0) { const char* e = getenv("LD_FORCE_GREEN"); fg = e ? 1 : 0;
                    gk = e ? _strtoui64(e, 0, 16) : 0; if (gk == 1) gk = 0; }
      if (fg && fi > 0 && (!gk || gk == live_legacy_pso_key())) {
          /* The emulated alpha test discards before the return, so replacing
           * only the return still showed nothing for a failing alpha. */
          for (char* d = strstr(ps_hlsl, "discard;"); d; d = strstr(d, "discard;"))
              memcpy(d, "        ", 8);
          char* r = strstr(ps_hlsl, "    return ");
          if (r) {
              char* semi = strchr(r, ';');
              if (semi) {
                  const char rep[] = "    return float4(0,1,0,1);";
                  const size_t rl = sizeof(rep) - 1;
                  const size_t old = (size_t)(semi + 1 - r);
                  const size_t used = strlen(ps_hlsl);
                  if (used - old + rl + 1 <= sizeof(ps_hlsl)) {
                      memmove(r + rl, semi + 1, used - (size_t)(semi + 1 - ps_hlsl) + 1);
                      memcpy(r, rep, rl);
                  }
              }
          }
      } }
    /* LD_FORCE_COL0=1: every vertex program outputs white COLOR0 -- separates
     * "the VS computes a wrong colour" from everything downstream of it. */
    /* LD_FORCE_COL0=<pso key hex> limits it to one PSO. */
    { static int fc = -1; static u64 fk = 0;
      if (fc < 0) { const char* e = getenv("LD_FORCE_COL0"); fc = e ? 1 : 0;
                    fk = e ? _strtoui64(e, 0, 16) : 0; if (fk == 1) fk = 0; }
      char* c0 = fc && vi > 0 && (!fk || fk == live_legacy_pso_key())
                 ? strstr(vs_hlsl, "Out.col0 = o[1];") : NULL;
      if (c0) memcpy(c0, "Out.col0=1+0*_p;", 16); }   /* same length, in place */
    if (fi > 0 && shadow_mask && g.resolution.scale != 1) {
        const char* needle = "coordinate.xy / size";
        char replacement[64];
        snprintf(replacement,sizeof(replacement),"coordinate.xy * %.1f / size",g.resolution.scale);
        char* at=ps_hlsl;
        while ((at=strstr(at,needle))) {
            size_t old=strlen(needle), len=strlen(replacement), total=strlen(ps_hlsl);
            if (total+len-old+1 > sizeof(ps_hlsl)) { fi=-1; break; }
            memmove(at+len,at+old,total-(size_t)(at-ps_hlsl)-old+1);
            memcpy(at,replacement,len); at+=len;
        }
    }
    if (fi > 0 && rsx_resolution_fragment(ps_hlsl, sizeof(ps_hlsl), target_domain) < 0)
        fi = -1;
    if (fi > 0 && flat_shading) ld_fp_flat_inputs(ps_hlsl, sizeof(ps_hlsl));
    if (fi > 0 && rs.logic_enable && !rs.rt_fp16 &&
        ld_fp_logic_export(ps_hlsl, sizeof(ps_hlsl)) < 0)
        fi = -1;
    /* LD_HLSL_DUMP=<dir>: files named by the PSO key the draw CSV prints. */
    { static const char* hd = (const char*)-1;
      if (hd == (const char*)-1) hd = getenv("LD_HLSL_DUMP");
      if (hd) {
        char fn[MAX_PATH]; FILE* f;
        snprintf(fn, sizeof fn, "%s/hlsl_%016llx.vs.txt", hd, (unsigned long long)live_legacy_pso_key());
        f = fopen(fn, "wb"); if (f) { fputs(vi > 0 ? vs_hlsl : "<vp fail>", f); fclose(f); }
        snprintf(fn, sizeof fn, "%s/hlsl_%016llx.ps.txt", hd, (unsigned long long)live_legacy_pso_key());
        f = fopen(fn, "wb"); if (f) { fputs(fi > 0 ? ps_hlsl : "<fp fail>", f); fclose(f); }
        snprintf(fn, sizeof fn, "%s/pipeline_%016llx.vs.txt", hd, (unsigned long long)key);
        f = fopen(fn, "wb"); if (f) { fputs(vi > 0 ? vs_hlsl : "<vp fail>", f); fclose(f); }
        snprintf(fn, sizeof fn, "%s/pipeline_%016llx.ps.txt", hd, (unsigned long long)key);
        f = fopen(fn, "wb"); if (f) { fputs(fi > 0 ? ps_hlsl : "<fp fail>", f); fclose(f); }
        snprintf(fn, sizeof fn, "%s/pipeline_%016llx.fp.bin", hd, (unsigned long long)key);
        f = fopen(fn, "wb"); if (f) { fwrite(fp_uc, 1, fp_size, f); fclose(f); }
      } }
    if (vi > 0 && fi > 0 && (packed_payload!=2 ||
        rsx_vertex_pull_wrap(vs_hlsl,sizeof(vs_hlsl),masked_layout?masked_layout->mask:0xffffu)))
        pso = build_pso(
            vs_hlsl, ps_hlsl, &rs, masked_layout, packed_payload);

    g.psos[g.n_psos].key = key;
    g.psos[g.n_psos].pso = pso;
    /* Decode once per pipeline. Inline constant payloads are skipped by the
     * decoder, and texture instructions in conditional paths remain live. */
    g_ld_current_pso.resources.fragment_textures = rsx_fp_texture_units(fp_uc, fp_size);
    g_ld_current_pso.resources.vertex_textures = txl_mask & vtex_mask;
    g_ld_current_pso.resources.program = rsx_fp_structural_hash(fp_uc,fp_size,1469598103934665603ull);
    g_ld_current_pso.resources.position_block = vi > 0 ? ld_vp_position_block(vs_hlsl) : -1;
    g.psos[g.n_psos].resources = g_ld_current_pso.resources;
    g.pso_index[bucket] = g.n_psos + 1u;
    g.n_psos++;
    return pso;
}

static int ld_upload_pixel_constants(u32* out_offset)
{
    if (out_offset)
        *out_offset = 0;
    if (!ld_fp_constants_buffered())
        return 1;
    if (!g.frame->ps_cb || !g.frame->ps_cb_mapped)
        return 0;

    /* HLSL uses a one-element placeholder array for programs with no inline
     * constants so fp_alpha retains a valid 16-byte slot. */
    const u32 value_slots =
        g.fp_constants.count ? g.fp_constants.count : 1u;
    const u32 data_bytes = (value_slots + 1u) * 16u;
    u32 offset = 0;
    u32 allocation_bytes = 0;
    int plan = rsx_fp_constant_ring_plan(
        g.frame->ps_cb_used, PS_CB_RING_BYTES, data_bytes,
        &offset, &allocation_bytes);
    if (plan < 0) {
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.ps_constant_capacity_failures++;
#endif
        return 0;
    }
    if (plan == 0) {
        ld_flush(LD_FLUSH_PIXEL_CONSTANT_RING);
        if (!g.ready)
            return 0;
        g.frame->ps_cb_used = 0;
        plan = rsx_fp_constant_ring_plan(
            g.frame->ps_cb_used, PS_CB_RING_BYTES, data_bytes,
            &offset, &allocation_bytes);
        if (plan != 1) {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.ps_constant_capacity_failures++;
#endif
            return 0;
        }
    }

#if defined(YZ_PERF_PROFILE)
    const LONGLONG upload_begin = ld_profile_qpc();
#endif
    u8* destination = g.frame->ps_cb_mapped + offset;
    memset(destination, 0, data_bytes);
    if (g.fp_constants.count)
        memcpy(
            destination, g.fp_constants.values,
            (size_t)g.fp_constants.count * 16u);
    if (g_pixel_constants_filter && g.fp_constants.count)
        g_pixel_constants_filter(g_ld_current_pso.resources.program,
            g.resolution.scale, (u32*)destination, g.fp_constants.count);
    memcpy(
        destination + value_slots * 16u,
        &g.fp_alpha_ref, sizeof(g.fp_alpha_ref));
    g.frame->ps_cb_used += allocation_bytes;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.ps_constant_allocations++;
    g_ld_profile.total.ps_constant_upload_bytes += data_bytes;
    g_ld_profile.total.ps_constant_upload_qpc +=
        (u64)(ld_profile_qpc() - upload_begin);
#endif
    if (out_offset)
        *out_offset = offset;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Draw accumulation sink (mirrors the harness sink)
 * -----------------------------------------------------------------------*/
typedef struct { float a[16][4]; } vtx_t;
typedef struct { u32 first, count; } batch_t;

typedef struct {
    /* INLINE_ARRAY source for this primitive, or NULL. The stream is the
     * vertices themselves, in guest byte order, so the fetch path reads it
     * with the same big-endian decoders it uses for a vertex array -- only
     * the base pointer differs. inl_off[i] is attribute i's byte offset
     * within one inline vertex; the attributes are packed in ascending
     * index order, which is what makes the offsets derivable at all (an
     * inline stream carries no per-array offset registers). */
    const u8* inl; u32 inl_bytes, inl_stride; u32 inl_off[16];
    batch_t arr[256]; u32 n_arr;
    batch_t idx[256]; u32 n_idx;
    u32     n_packets;
    vtx_t*  verts; u32 n_verts, cap_verts;
    rsx_vertex_ref* refs; u32 n_refs, cap_refs;
    u32     n_source_refs;
    int     refs_remapped;
    rsx_vertex_remap ref_remap;
    u8* compact_verts; u64 compact_capacity;
    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch_plan;
    rsx_vertex_pull_plan pull_plan;
    int native_pull;
    int     fetch_ok;
    /* Primitive-restart cut points (s25 port of the replay-harness fix):
     * a guest index equal to the RSX restart sentinel is a cut marker, not
     * a vertex reference (RPCS3 RSXThread.cpp:398); fetch_batches records
     * the n_verts position at each one and the STRIP/FAN expansion must
     * not bridge across a cut. */
    u32*    cuts; u32 n_cuts, cap_cuts;
} draw_ctx;

static draw_ctx dc;

static void dc_reset(void)
{
    dc.n_arr = dc.n_idx = dc.n_verts = dc.n_refs = 0;
    dc.n_source_refs = 0;
    dc.refs_remapped = 0;
    dc.native_pull = 0;
    dc.n_packets = 0;
    dc.n_cuts = 0;
    dc.fetch_ok = 1;
    dc.inl = NULL; dc.inl_bytes = dc.inl_stride = 0;
}
static void push_vert(const vtx_t* v)
{
    if (dc.n_verts >= dc.cap_verts) {
        u32 nc = dc.cap_verts ? dc.cap_verts * 2 : 4096;
        vtx_t* nv = (vtx_t*)realloc(dc.verts, (size_t)nc * sizeof(vtx_t));
        if (!nv) { dc.fetch_ok = 0; return; }
        dc.verts = nv; dc.cap_verts = nc;
    }
    dc.verts[dc.n_verts++] = *v;
}

static void push_compact_ref(u32 vertex_id, u32 base_index)
{
    if (dc.n_refs >= dc.cap_refs) {
        const u32 next_capacity = dc.cap_refs ? dc.cap_refs * 2u : 4096u;
        rsx_vertex_ref* refs = (rsx_vertex_ref*)realloc(
            dc.refs, (size_t)next_capacity * sizeof(*refs));
        if (!refs) {
            dc.fetch_ok = 0;
            return;
        }
        dc.refs = refs;
        dc.cap_refs = next_capacity;
    }
    dc.refs[dc.n_refs].vertex_id = vertex_id;
    dc.refs[dc.n_refs].base_index = base_index;
    dc.n_refs++;
}

static int reserve_compact_vertices(u64 bytes)
{
    if (bytes <= dc.compact_capacity)
        return 1;
    if (bytes > SIZE_MAX)
        return 0;
    u64 next_capacity = dc.compact_capacity ? dc.compact_capacity : 1u << 20;
    while (next_capacity < bytes) {
        if (next_capacity > SIZE_MAX / 2u)
            return 0;
        next_capacity *= 2u;
    }
    u8* vertices = (u8*)realloc(
        dc.compact_verts, (size_t)next_capacity);
    if (!vertices)
        return 0;
    dc.compact_verts = vertices;
    dc.compact_capacity = next_capacity;
    return 1;
}

static int reserve_legacy_vertices(u32 count)
{
    if (count <= dc.cap_verts)
        return 1;
    u32 next_capacity = dc.cap_verts ? dc.cap_verts : 4096u;
    while (next_capacity < count) {
        if (next_capacity > UINT32_MAX / 2u)
            return 0;
        next_capacity *= 2u;
    }
    vtx_t* vertices = (vtx_t*)realloc(
        dc.verts, (size_t)next_capacity * sizeof(*vertices));
    if (!vertices)
        return 0;
    dc.verts = vertices;
    dc.cap_verts = next_capacity;
    return 1;
}

static float ld_be_f32(const u8* p)
{
    const u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static float ld_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, 4);
    return f;
}

static int fetch_attr(u32 i, u32 base, u32 vertex_id, u32 base_index,
                      float out[4])
{
    rsx_dsp_vertex_attr a;
    rsx_dsp_get_vertex_attr(&g.rsx, i, &a);
    if (!a.type || !a.size) return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    const u32 elem_size = rsx_vertex_attrib_size(a.type, a.size);
    const u32 stride = a.stride ? a.stride : elem_size;
    const u32 divider_mask = rsx_dsp_reg(&g.rsx, 0x1FC0);
    if (a.frequency >= 2)
        g_ld_divider_fetches++;
    const u32 source_element = rsx_vertex_element_index(
        vertex_id, base_index, a.frequency,
        (divider_mask >> i) & 1u);
    const u8* p;
    if (dc.inl) {
        /* Inline vertices ignore the array offset and the data base: the
         * stream IS the array, packed at the declared stride. */
        const u64 at = (u64)source_element * dc.inl_stride + dc.inl_off[i];
        if (at + elem_size > dc.inl_bytes) return 0;
        p = dc.inl + at;
    } else {
        p = guest_ptr(
            a.location, base + a.offset + source_element * stride, elem_size);
    }
    if (!p) return 0;
    for (u32 c = 0; c < a.size && c < 4; c++) {
        switch (a.type) {
        case RSX_VTX_TYPE_FLOAT:
            out[c] = ld_be_f32(p + c * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[c] = ld_be_f16(p + c * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[c] = p[c] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[c] = (float)p[c];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 v = (s16)((p[c * 2] << 8) | p[c * 2 + 1]);
            out[c] = v / 32767.0f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 v = (s16)((p[c * 2] << 8) | p[c * 2 + 1]);
            out[c] = (float)v;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                          ((u32)p[2] << 8) | (u32)p[3];
            s32 x = (s32)(w & 0x7FF);          if (x & 0x400) x -= 0x800;
            s32 y = (s32)((w >> 11) & 0x7FF); if (y & 0x400) y -= 0x800;
            s32 z = (s32)((w >> 22) & 0x3FF); if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

static void fetch_one(u32 base, u32 vertex_id, u32 base_index)
{
    vtx_t v;
    for (u32 i = 0; i < 16; i++) {
        rsx_dsp_vertex_attr a;
        rsx_dsp_get_vertex_attr(&g.rsx, i, &a);
        if (a.type && a.size &&
            fetch_attr(i, base, vertex_id, base_index, v.a[i])) continue;
        if (i == 0) { dc.fetch_ok = 0; return; }
        rsx_dsp_vertex_default(&g.rsx, i, v.a[i]);
        if (i == 3 && v.a[3][0] == 0 && v.a[3][1] == 0 && v.a[3][2] == 0 && v.a[3][3] == 1)
            v.a[3][0] = v.a[3][1] = v.a[3][2] = 1.0f;
    }
    push_vert(&v);
}

static void fetch_batches(void)
{
    const u32 base = rsx_dsp_vertex_data_base_offset(&g.rsx);
    for (u32 r = 0; r < dc.n_arr && dc.fetch_ok; r++)
        for (u32 i = 0; i < dc.arr[r].count && dc.fetch_ok; i++)
            fetch_one(base, dc.arr[r].first + i, 0);
    if (!dc.n_idx) return;
    const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
    rsx_dsp_index_array ia; rsx_dsp_get_index_array(&g.rsx, &ia);
    /* Restart sentinel handling, same rule as the replay harness (RPCS3
     * RSXThread.cpp:398 "if (value == restart) continue" + rsx_methods.h
     * restart_index_enabled()/restart_index()): record a cut, never fetch
     * the phantom vertex. Kill-switch RSX_NO_RESTART shared with the
     * harness for byte-exact A/B. */
    static int s_no_restart = -1;
    if (s_no_restart < 0) s_no_restart = getenv("RSX_NO_RESTART") ? 1 : 0;
    const int restart_en = !s_no_restart && rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
    const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
    for (u32 r = 0; r < dc.n_idx && dc.fetch_ok; r++)
        for (u32 i = 0; i < dc.idx[r].count && dc.fetch_ok; i++) {
            const u32 esz = ia.is_u32 ? 4 : 2;
            const u8* ip = guest_ptr(ia.location, ia.offset + (dc.idx[r].first + i) * esz, esz);
            if (!ip) { dc.fetch_ok = 0; return; }
            const u32 index = ia.is_u32
                ? (((u32)ip[0] << 24) | ((u32)ip[1] << 16) | ((u32)ip[2] << 8) | ip[3])
                : (u32)((ip[0] << 8) | ip[1]);
            if (restart_en && index == restart_val) {
                if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                                          dc.n_verts)) {
                    dc.fetch_ok = 0;
                    return;
                }
                continue;
            }
            fetch_one(base, index, base_index);
        }
}

static void fetch_batches_hoisted(
    const rsx_vertex_layout_plan* layout, int packed_payload)
{
    for (u32 batch = 0; batch < dc.n_arr && dc.fetch_ok; batch++)
        for (u32 i = 0; i < dc.arr[batch].count && dc.fetch_ok; i++)
            push_compact_ref(dc.arr[batch].first + i, 0);

    if (dc.n_idx && dc.fetch_ok) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array index_array;
        rsx_dsp_get_index_array(&g.rsx, &index_array);
        const u32 element_size = index_array.is_u32 ? 4u : 2u;
        static int no_restart = -1;
        if (no_restart < 0)
            no_restart = getenv("RSX_NO_RESTART") ? 1 : 0;
        const int restart_enabled =
            !no_restart &&
            rsx_dsp_restart_index_enabled(&g.rsx, index_array.is_u32);
        const u32 restart_value = rsx_dsp_restart_index(&g.rsx);

        for (u32 batch = 0; batch < dc.n_idx && dc.fetch_ok; batch++) {
            const u32 first = dc.idx[batch].first;
            const u32 count = dc.idx[batch].count;
            const u64 start64 =
                (u64)index_array.offset + (u64)first * element_size;
            const u64 bytes64 = (u64)count * element_size;
            const u8* contiguous = NULL;
            if (count && start64 <= UINT32_MAX &&
                bytes64 <= UINT32_MAX &&
                start64 + bytes64 <= 0x100000000ull) {
                contiguous = guest_ptr(
                    index_array.location, (u32)start64, (u32)bytes64);
            }
            for (u32 i = 0; i < count && dc.fetch_ok; i++) {
                const u32 source_offset =
                    index_array.offset + (first + i) * element_size;
                const u8* source = contiguous
                    ? contiguous + (size_t)i * element_size
                    : guest_ptr(
                        index_array.location, source_offset, element_size);
                if (!source) {
                    dc.fetch_ok = 0;
                    break;
                }
                const u32 index = index_array.is_u32
                    ? (((u32)source[0] << 24) |
                       ((u32)source[1] << 16) |
                       ((u32)source[2] << 8) | source[3])
                    : (u32)((source[0] << 8) | source[1]);
                if (restart_enabled && index == restart_value) {
                    if (!rsx_restart_cut_push(
                            &dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                            dc.n_refs)) {
                        dc.fetch_ok = 0;
                        break;
                    }
                    continue;
                }
                push_compact_ref(index, base_index);
            }
        }
    }

    if (!dc.fetch_ok)
        return;
    dc.n_source_refs = dc.n_refs;
    if (packed_payload && dc.n_refs > 1) {
        u32 unique_refs = dc.n_refs;
        if (rsx_vertex_remap_build(
                &dc.ref_remap, dc.refs, dc.n_refs, &unique_refs)) {
            dc.refs_remapped = unique_refs < dc.n_refs;
            dc.n_refs = unique_refs;
        }
    }
    dc.layout = *layout;
    rsx_vertex_fetch_plan_init(
        &dc.fetch_plan, &g.rsx, layout,
        (rsx_vertex_guest_ptr_fn)g.guest_ptr, g.guest_user);
    rsx_vertex_fetch_plan_set_inline(&dc.fetch_plan, &g.rsx, dc.inl, dc.inl_bytes);
    rsx_vertex_fetch_plan_prepare(&dc.fetch_plan, dc.refs, dc.n_refs);
    for (u32 slot = 0; slot < layout->count; slot++) {
        const u32 attr = layout->attrs[slot];
        if (dc.fetch_plan.attr[attr].desc.frequency >= 2)
            g_ld_divider_fetches += dc.n_refs;
    }
    if(packed_payload && ld_vertex_mask_signature() && ld_vertex_pull()) {
        if(rsx_vertex_pull_prepare(&dc.pull_plan,&dc.fetch_plan,dc.refs,dc.n_refs,VERT_BUFFER_SIZE)) {
            dc.native_pull=1;dc.n_verts=dc.n_refs;return;
        }
        ++g_pull_declined;
    }
    if (packed_payload) {
        const u64 required = (u64)dc.n_refs * layout->stride;
        if (!reserve_compact_vertices(required)) {
            dc.fetch_ok = 0;
            return;
        }
        for (u32 i = 0; i < dc.n_refs; i++) {
            u8* destination = layout->stride
                ? dc.compact_verts + (u64)i * layout->stride : NULL;
            if (!rsx_vertex_fetch_one(
                    &dc.fetch_plan, &dc.refs[i], destination)) {
                dc.fetch_ok = 0;
                return;
            }
        }
    } else {
        /*
         * H and M deliberately retain the complete legacy payload.  With an
         * all-attribute layout, slot order is register order, so the helper
         * writes exactly the original attribute*16 byte offsets.
         */
        if (!reserve_legacy_vertices(dc.n_refs)) {
            dc.fetch_ok = 0;
            return;
        }
        /* Legacy treats an absent ATTR0 as a failed group, even though other
         * absent attributes use their RSX current/default value. */
        if (dc.n_refs &&
            (!dc.fetch_plan.attr[0].desc.type ||
             !dc.fetch_plan.attr[0].desc.size)) {
            dc.fetch_ok = 0;
            return;
        }
        for (u32 i = 0; i < dc.n_refs; i++) {
            if (!rsx_vertex_fetch_one(
                    &dc.fetch_plan, &dc.refs[i],
                    (u8*)&dc.verts[i])) {
                dc.fetch_ok = 0;
                return;
            }
        }
    }
    dc.n_verts = dc.n_refs;
}

static const float* dc_vertex_attr(
    u32 vertex, u32 attr, float fallback[4])
{
    if(dc.native_pull && vertex<dc.n_verts) {
        rsx_vertex_fetch_plan one=dc.fetch_plan;
        rsx_vertex_layout_plan_init(&one.layout,1u<<attr);
        if(rsx_vertex_fetch_one(&one,&dc.refs[vertex],(u8*)fallback))return fallback;
    }
    if (!ld_vertex_compact_payload())
        return dc.verts[vertex].a[attr];
    const s8 slot = dc.layout.slot_by_attr[attr];
    if (slot >= 0 && vertex < dc.n_verts)
        return (const float*)(
            dc.compact_verts + (u64)vertex * dc.layout.stride +
            (u32)slot * 16u);
    memcpy(fallback, dc.fetch_plan.attr[attr].default_value, 16);
    return fallback;
}

/* Diagnostic-only counterpart to dc_vertex_attr().  Compact rendering omits
 * attributes the active vertex program does not read, but the legacy replay
 * fingerprint contains all sixteen RSX attributes.  Re-fetch an omitted,
 * enabled attribute from the original guest reference so a CSV hash compares
 * the producer bytes rather than the two renderers' different upload layouts. */
static const float* dc_vertex_attr_canonical(
    u32 vertex, u32 attr, float fallback[4])
{
    if(dc.native_pull)return dc_vertex_attr(vertex,attr,fallback);
    if (!ld_vertex_compact_payload())
        return dc.verts[vertex].a[attr];
    const s8 slot = dc.layout.slot_by_attr[attr];
    if (slot >= 0 && vertex < dc.n_verts)
        return (const float*)(
            dc.compact_verts + (u64)vertex * dc.layout.stride +
            (u32)slot * 16u);

    const rsx_vertex_fetch_attr* fetch = &dc.fetch_plan.attr[attr];
    if (vertex < dc.n_verts && fetch->desc.type && fetch->desc.size &&
        fetch->elem_size) {
        const rsx_vertex_ref* ref = &dc.refs[vertex];
        const u32 element = rsx_vertex_element_index(
            ref->vertex_id, ref->base_index, fetch->desc.frequency,
            (dc.fetch_plan.divider_mask >> attr) & 1u);
        const u64 offset =
            (u64)dc.fetch_plan.base_offset + fetch->desc.offset +
            (u64)element * fetch->stride;
        if (offset <= UINT32_MAX) {
            const u8* source = guest_ptr(
                fetch->desc.location, (u32)offset, fetch->elem_size);
            if (source && rsx_vertex_decode_element(
                    fetch->desc.type, fetch->desc.size,
                    source, fallback))
                return fallback;
        }
    }
    memcpy(fallback, fetch->default_value, 16);
    return fallback;
}

/* primitive ids = raw NV4097 VERTEX_BEGIN_END arg (rsx_dispatch stores it raw;
 * matches the replay harness). These were off by one (4/5/6/7), which dropped
 * EVERY quad/triangle draw through the switch's default: return -> black. */
#define PRIM_TRIANGLES       5
#define PRIM_TRIANGLE_STRIP  6
#define PRIM_TRIANGLE_FAN    7
#define PRIM_QUADS           8

/* Live-draw activity counters (verification: is real geometry flowing, or only
 * clears?). Reported per presented frame in rsx_live_draw_present. */
/* DRAW_ARRAYS/DRAW_INDEX_ARRAY writes are packets. Multiple packets between a
 * single BEGIN/END are deliberately coalesced into one D3D12 DrawInstanced,
 * so comparing packet count directly with executed D3D draws was invalid.
 * Keep two independently balanced ledgers instead:
 *
 *   packets_seen = packets_queued + packets_movie + packets_queue_full
 *   groups_seen  = groups_executed + every group_drop_* outcome
 *
 * An END with no accepted packet is counted separately as groups_empty and is
 * not a render attempt. This makes every early return explicit. */
typedef struct ld_stats {
    unsigned long long packets_seen;
    unsigned long long packets_queued;
    unsigned long long packets_movie;
    unsigned long long packets_queue_full;
    unsigned long long groups_seen;
    unsigned long long groups_empty;
    unsigned long long groups_executed;
    unsigned long long group_drop_fetch;
    unsigned long long group_drop_degenerate;
    unsigned long long group_drop_primitive;
    unsigned long long group_drop_alloc;
    unsigned long long group_drop_pso;
    unsigned long long group_drop_ring;
    unsigned long long group_drop_surface;
    unsigned long long clears;
    unsigned long long clear_drop_surface;
    unsigned long long implicit_depth_clears;
} ld_stats;

static ld_stats g_ld_stats;

void rsx_live_draw_a010_probe_begin(void)
{
    if ((!getenv("RSX_A010_PROBE") &&
         !getenv("RSX_A010_SURFACE_DUMP")) || !g.ready)
        return;
    /*
     * Targeted surface readbacks submit and fence the open D3D12 list. When
     * surface capture alone is requested, defer that cost until AUTH confirms
     * that both the character palette and verified camera are synchronized.
     * Explicit RSX_A010_PROBE retains its scene-open diagnostic behavior.
     */
    if (!getenv("RSX_A010_PROBE") &&
        InterlockedCompareExchange(
            &g_yz_a010_reference_camera_active, 0, 0) == 0)
        return;
    if (g_yz_runtime_config.a010_start_reference) {
        /* Deterministic visual-verification camera derived from the shipped
         * cam_Came_000_002.cmt frame 612. Install it on the RSX thread as soon
         * as the a010 probe opens so a later blocked PPU scene update cannot
         * prevent the reference matrix from reaching the first world draws. */
        static const float reference_matrix_612[16] = {
             3.18890834f,   -0.000270011135f,  0.08452246f,   5.81511511f,
            -0.0102274104f,  5.65674844f,      0.403935942f, -18.5085361f,
             0.0264371686f,  0.0712562195f,   -0.997207931f,  9.99292802f,
             0.0264345249f,  0.0712490938f,   -0.99710821f,  10.0919287f,
        };
        static const float reference_matrix_874[16] = {
             6.80647601f,   -0.00327668415f,  0.611324803f, -36.9135075f,
             0.00523179535f, 12.1491076f,     0.00686819648f, -38.8146474f,
             0.0894642733f,  0.000524588748f,-0.996090306f, -11.7467763f,
             0.0894553268f,  0.000524536289f,-0.995990697f, -11.6456016f,
        };
        const float* reference_matrix =
            g_yz_runtime_config.a010_reference_874
                ? reference_matrix_874 : reference_matrix_612;
        rsx_live_draw_set_a010_camera_matrix(reference_matrix);
    }
    CreateDirectoryA("scratch\\a010_probe", NULL);
    g_ld_a010_probe_start_frame = g_ld_frames;
    g_ld_a010_probe_sample = 0;
    g_ld_a010_probe_touched = 0;
    InterlockedExchange(&g_ld_a010_world_ready, 0);
    InterlockedExchange(&g_ld_a010_probe_active, 1);
    fprintf(stderr,
            "[a010-probe] BEGIN live_frame=%u surfaces=%u packets=%llu groups=%llu\n",
            g_ld_frames, g.n_surfaces, g_ld_stats.packets_seen,
            g_ld_stats.groups_seen);
    fflush(stderr);
}

int rsx_live_draw_a010_probe_active(void)
{
    return InterlockedCompareExchange(&g_ld_a010_probe_active, 0, 0) != 0;
}

int rsx_live_draw_a010_world_ready(void)
{
    return InterlockedCompareExchange(&g_ld_a010_world_ready, 0, 0) != 0;
}

static unsigned long long ld_groups_accounted(void)
{
    return g_ld_stats.groups_executed + g_ld_stats.group_drop_fetch +
           g_ld_stats.group_drop_degenerate + g_ld_stats.group_drop_primitive +
           g_ld_stats.group_drop_alloc + g_ld_stats.group_drop_pso +
           g_ld_stats.group_drop_ring + g_ld_stats.group_drop_surface;
}

#if defined(YZ_PERF_PROFILE)
static double ld_profile_ticks_ms(u64 ticks)
{
    return g_ld_profile.qpc_frequency
        ? (double)ticks * 1000.0 / (double)g_ld_profile.qpc_frequency
        : 0.0;
}

static void ld_profile_present(u32 frame)
{
    const LONGLONG now = ld_profile_qpc();
    const double frame_ms = ld_profile_ticks_ms(
        (u64)(now - g_ld_profile.previous_present_qpc));
    const ld_profile_counts* total = &g_ld_profile.total;
    const ld_profile_counts* previous = &g_ld_profile.previous;
#define LD_PROFILE_DELTA(field) (total->field - previous->field)
    const u64 packets =
        g_ld_stats.packets_seen - g_ld_profile.previous_packets;
    const u64 groups =
        g_ld_stats.groups_seen - g_ld_profile.previous_groups;
    const u64 executed =
        g_ld_stats.groups_executed - g_ld_profile.previous_executed;
    const u64 evictions =
        g_ld_texture_cache_evictions - g_ld_profile.previous_evictions;
    const u64 refreshes =
        g_ld_vtex_refreshes - g_ld_profile.previous_refreshes;
    const u64 used_attribute_draws =
        LD_PROFILE_DELTA(used_attribute_draws);
    const double used_attribute_average = used_attribute_draws
        ? (double)LD_PROFILE_DELTA(used_attribute_sum) /
              (double)used_attribute_draws
        : 0.0;
    const int compile_free =
        LD_PROFILE_DELTA(vs_compile_calls) == 0 &&
        LD_PROFILE_DELTA(ps_compile_calls) == 0;
    g_ld_profile.measured_frames++;
    g_ld_profile.total_frame_ms += frame_ms;
    if (compile_free) {
        g_ld_profile.compile_free_frames++;
        g_ld_profile.compile_free_frame_ms += frame_ms;
    }
    /*
     * The two matched visual-validation windows are deliberately small.
     * Emit one aggregate line per presented frame there so an improvement
     * below the generic 250 ms slow-frame threshold remains measurable.
     * Scene identity is still established by the run driver's asset marker
     * and screenshots; these values are only run-local capture locators.
     */
    const int matched_window =
        (frame >= 2240u && frame <= 2300u) ||
        (frame >= 4800u && frame <= 4900u);
    const int emit =
        frame <= 16u || matched_window || (frame & 63u) == 0u ||
        frame_ms >= 250.0 ||
        LD_PROFILE_DELTA(pso_misses) != 0 ||
        LD_PROFILE_DELTA(pso_full) != 0 ||
        LD_PROFILE_DELTA(flush_reason[LD_FLUSH_VERTEX_RING]) != 0 ||
        LD_PROFILE_DELTA(flush_reason[LD_FLUSH_RETIRE_QUEUE]) != 0 ||
        LD_PROFILE_DELTA(
            flush_reason[LD_FLUSH_PIXEL_CONSTANT_RING]) != 0 ||
        LD_PROFILE_DELTA(ps_constant_capacity_failures) != 0;

    if (emit) {
        const rsx_surface_view_stats* views=&g_surface_views.stats;
        fprintf(stderr,"[surface-views] frame=%u hits=%llu misses=%llu allocations=%llu copies=%llu evictions=%llu entries=%llu resident_mb=%.3f peak_mb=%.3f legacy_allocations=%llu legacy_copies=%llu\n",
            frame,(unsigned long long)views->hits,(unsigned long long)views->misses,
            (unsigned long long)views->allocations,(unsigned long long)views->copies,
            (unsigned long long)views->evictions,(unsigned long long)views->resident_entries,
            views->resident_bytes/(1024.0*1024.0),views->peak_resident_bytes/(1024.0*1024.0),
            (unsigned long long)views->legacy_allocations,(unsigned long long)views->legacy_copies);
        fprintf(stderr,"[shader-resources] frame=%u dependencies=%llu unused_enabled=%llu mode=%s\n",
                g_ld_frames,
                (unsigned long long)LD_PROFILE_DELTA(texture_dependencies),
                (unsigned long long)LD_PROFILE_DELTA(texture_unused_bindings),
                ld_shader_resources_enabled() ? "shader" : "legacy");
        fprintf(stderr, "[bleach-render-cost] frame=%u pso_ms=%.3f texture_hash_ms=%.3f texture_mb=%.3f source_cache_mb=%.3f compares=%llu fallbacks=%llu clean_pages=%llu texture_resolve_ms=%.3f command_encode_ms=%.3f gpu_ms=%.3f\n",
                g_ld_frames, ld_profile_ticks_ms(LD_PROFILE_DELTA(pso_total_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(texture_hash_qpc)),
                (double)LD_PROFILE_DELTA(texture_hash_bytes) / (1024.0 * 1024.0),
                (double)g_ld_source_bytes / (1024.0 * 1024.0),
                (unsigned long long)g_ld_source_compares,
                (unsigned long long)g_ld_source_fallbacks,
                (unsigned long long)g_ld_source_clean,
                ld_profile_ticks_ms(LD_PROFILE_DELTA(texture_resolve_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(command_encode_qpc)),
                g.timing_frequency ? 1000.0 * (double)LD_PROFILE_DELTA(gpu_ticks) / g.timing_frequency : -1.0);
        DXGI_QUERY_VIDEO_MEMORY_INFO memory = {0};
        if (g_ld_profile.adapter)
            g_ld_profile.adapter->lpVtbl->QueryVideoMemoryInfo(
                g_ld_profile.adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                &memory);
        fprintf(stderr,
                "[rsx-perf] frame=%u dt_ms=%.3f "
                "pso{look=%llu hit=%llu miss=%llu miss_total=%llu "
                "probes=%llu full=%llu cached=%u capacity=%u mem_kb=%.1f} "
                "reject{requests=%llu unique=%u "
                "world=%llu/%u character=%llu/%u "
                "ui=%llu/%u other=%llu/%u} "
                "hlsl{vs_lookup=%llu vs_hit=%llu vs_compile=%llu "
                "vs_new=%llu vs_unique=%u ps_lookup=%llu ps_hit=%llu "
                "ps_compile=%llu ps_new=%llu ps_unique=%u} "
                "time_ms{decompile=%.3f vs_compile=%.3f ps_compile=%.3f "
                "create_pso=%.3f flush=%.3f fence=%.3f draw_cpu=%.3f} "
                "vertex{path=%s used_avg=%.3f used_draws=%llu used_max=%u "
                "legacy_mb=%.3f actual_mb=%.3f fetch_pack_ms=%.3f} "
                "vb_sync{flushes=%llu flush_ms=%.3f wait_ms=%.3f} "
                "frame{compile_free=%u} "
                "draw{packets=%llu groups=%llu exec=%llu "
                "input_v=%llu expanded_v=%llu vb_mb=%.3f} "
                "flush{present=%llu ref=%llu vb=%llu retire=%llu "
                "movie=%llu movie_present=%llu readback=%llu} "
                "tex{cached=%u evict=%llu refresh=%llu upload_mb=%.3f "
                "decode=%llu decode_ms=%.3f upload_hi_mb=%.3f "
                "retired_hi=%u} "
                "rings{vb_hi_mb=%.3f upload_total_hi_mb=%.3f "
                "vb_total_hi_mb=%.3f retired_total_hi=%u} "
                "vram{usage_mb=%.1f budget_mb=%.1f reservation_mb=%.1f}\n",
                frame, frame_ms,
                (unsigned long long)LD_PROFILE_DELTA(pso_lookups),
                (unsigned long long)LD_PROFILE_DELTA(pso_hits),
                (unsigned long long)LD_PROFILE_DELTA(pso_misses),
                (unsigned long long)total->pso_misses,
                (unsigned long long)LD_PROFILE_DELTA(pso_probes),
                (unsigned long long)LD_PROFILE_DELTA(pso_full),
                g.n_psos,
                MAX_PSOS,
                (double)sizeof(g.psos) / 1024.0,
                (unsigned long long)total->pso_full,
                g_ld_profile.n_rejected_pso_keys,
                (unsigned long long)
                    g_ld_profile.rejected_pso_requests[LD_REJECT_WORLD],
                g_ld_profile.rejected_pso_unique[LD_REJECT_WORLD],
                (unsigned long long)
                    g_ld_profile.rejected_pso_requests[LD_REJECT_CHARACTER],
                g_ld_profile.rejected_pso_unique[LD_REJECT_CHARACTER],
                (unsigned long long)
                    g_ld_profile.rejected_pso_requests[LD_REJECT_UI],
                g_ld_profile.rejected_pso_unique[LD_REJECT_UI],
                (unsigned long long)
                    g_ld_profile.rejected_pso_requests[LD_REJECT_OTHER],
                g_ld_profile.rejected_pso_unique[LD_REJECT_OTHER],
                (unsigned long long)LD_PROFILE_DELTA(vs_blob_lookups),
                (unsigned long long)LD_PROFILE_DELTA(vs_blob_hits),
                (unsigned long long)LD_PROFILE_DELTA(vs_compile_calls),
                (unsigned long long)LD_PROFILE_DELTA(vs_unique),
                g_ld_profile.n_vs_hashes,
                (unsigned long long)LD_PROFILE_DELTA(ps_blob_lookups),
                (unsigned long long)LD_PROFILE_DELTA(ps_blob_hits),
                (unsigned long long)LD_PROFILE_DELTA(ps_compile_calls),
                (unsigned long long)LD_PROFILE_DELTA(ps_unique),
                g_ld_profile.n_ps_hashes,
                ld_profile_ticks_ms(LD_PROFILE_DELTA(decompile_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(vs_compile_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(ps_compile_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(create_pso_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(flush_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(fence_wait_qpc)),
                ld_profile_ticks_ms(LD_PROFILE_DELTA(sink_end_qpc)),
                ld_vertex_mode_name(),
                used_attribute_average,
                (unsigned long long)used_attribute_draws,
                g_ld_profile.frame_used_attribute_max,
                (double)LD_PROFILE_DELTA(legacy_vertex_upload_bytes) /
                    (1024.0 * 1024.0),
                (double)LD_PROFILE_DELTA(vertex_upload_bytes) /
                    (1024.0 * 1024.0),
                ld_profile_ticks_ms(
                    LD_PROFILE_DELTA(vertex_fetch_pack_qpc)),
                (unsigned long long)
                    LD_PROFILE_DELTA(
                        flush_reason[LD_FLUSH_VERTEX_RING]),
                ld_profile_ticks_ms(
                    LD_PROFILE_DELTA(
                        flush_reason_qpc[LD_FLUSH_VERTEX_RING])),
                ld_profile_ticks_ms(
                    LD_PROFILE_DELTA(
                        fence_reason_qpc[LD_FLUSH_VERTEX_RING])),
                compile_free ? 1u : 0u,
                (unsigned long long)packets,
                (unsigned long long)groups,
                (unsigned long long)executed,
                (unsigned long long)LD_PROFILE_DELTA(input_vertices),
                (unsigned long long)LD_PROFILE_DELTA(expanded_vertices),
                (double)LD_PROFILE_DELTA(vertex_upload_bytes) /
                    (1024.0 * 1024.0),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_PRESENT]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_GUEST_REFERENCE]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_VERTEX_RING]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_RETIRE_QUEUE]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_MOVIE]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_MOVIE_PRESENT]),
                (unsigned long long)
                    LD_PROFILE_DELTA(flush_reason[LD_FLUSH_READBACK]),
                g.n_textures,
                (unsigned long long)evictions,
                (unsigned long long)refreshes,
                (double)LD_PROFILE_DELTA(texture_upload_bytes) /
                    (1024.0 * 1024.0),
                (unsigned long long)
                    LD_PROFILE_DELTA(texture_decode_calls),
                ld_profile_ticks_ms(
                    LD_PROFILE_DELTA(texture_decode_qpc)),
                (double)g_ld_profile.frame_upload_high /
                    (1024.0 * 1024.0),
                g_ld_profile.frame_retired_high,
                (double)g_ld_profile.frame_vb_high /
                    (1024.0 * 1024.0),
                (double)g_ld_profile.total_upload_high /
                    (1024.0 * 1024.0),
                (double)g_ld_profile.total_vb_high /
                    (1024.0 * 1024.0),
                g_ld_profile.total_retired_high,
                (double)memory.CurrentUsage / (1024.0 * 1024.0),
                (double)memory.Budget / (1024.0 * 1024.0),
                (double)memory.CurrentReservation / (1024.0 * 1024.0));
        fprintf(stderr,
                "[shader-cache-perf] frame=%u "
                "vs{count=%u capacity=%u lookups=%llu hits=%llu "
                "misses=%llu inserts=%llu full_rejects=%llu "
                "compile_calls=%llu lookup_ms=%.3f compile_ms=%.3f "
                "post2048_distinct=%llu post2048_repeats=%llu "
                "source_bytes=%llu blob_bytes=%llu} "
                "ps{count=%u capacity=%u lookups=%llu hits=%llu "
                "misses=%llu inserts=%llu full_rejects=%llu "
                "compile_calls=%llu lookup_ms=%.3f compile_ms=%.3f "
                "post2048_distinct=%llu post2048_repeats=%llu "
                "source_bytes=%llu blob_bytes=%llu}\n",
                frame,
                g.vs_blobs.count, MAX_SHADER_BLOBS,
                (unsigned long long)total->vs_blob_lookups,
                (unsigned long long)total->vs_blob_hits,
                (unsigned long long)total->vs_blob_misses,
                (unsigned long long)total->vs_blob_inserts,
                (unsigned long long)total->vs_blob_full_rejects,
                (unsigned long long)total->vs_compile_calls,
                ld_profile_ticks_ms(total->vs_blob_lookup_qpc),
                ld_profile_ticks_ms(total->vs_compile_qpc),
                (unsigned long long)total->vs_post_boundary_distinct,
                (unsigned long long)total->vs_post_boundary_repeats,
                (unsigned long long)g.vs_blobs.retained_source_bytes,
                (unsigned long long)g.vs_blobs.retained_blob_bytes,
                g.ps_blobs.count, MAX_SHADER_BLOBS,
                (unsigned long long)total->ps_blob_lookups,
                (unsigned long long)total->ps_blob_hits,
                (unsigned long long)total->ps_blob_misses,
                (unsigned long long)total->ps_blob_inserts,
                (unsigned long long)total->ps_blob_full_rejects,
                (unsigned long long)total->ps_compile_calls,
                ld_profile_ticks_ms(total->ps_blob_lookup_qpc),
                ld_profile_ticks_ms(total->ps_compile_qpc),
                (unsigned long long)total->ps_post_boundary_distinct,
                (unsigned long long)total->ps_post_boundary_repeats,
                 (unsigned long long)g.ps_blobs.retained_source_bytes,
                 (unsigned long long)g.ps_blobs.retained_blob_bytes);
        const u32 collapsed_by_constants =
            g_ld_profile.n_ps_constant_specialized_hashes >=
                    g_ld_profile.n_ps_canonical_hashes
                ? g_ld_profile.n_ps_constant_specialized_hashes -
                      g_ld_profile.n_ps_canonical_hashes
                : 0;
        const u32 collapsed_by_alpha =
            g_ld_profile.n_ps_exact_source_hashes >=
                    g_ld_profile.n_ps_constant_specialized_hashes
                ? g_ld_profile.n_ps_exact_source_hashes -
                      g_ld_profile.n_ps_constant_specialized_hashes
                : 0;
        fprintf(
            stderr,
            "[fp-constant-perf] frame=%u mode=%s "
            "variants{exact_source=%u constant_specialized=%u "
            "canonical=%u collapsed_constants=%u collapsed_alpha=%u "
            "tracking_overflow=%llu} "
            "cb{allocations=%llu upload_bytes=%llu upload_ms=%.3f "
            "ring_used=%u ring_capacity=%u capacity_failures=%llu "
            "flushes=%llu flush_ms=%.3f wait_ms=%.3f}\n",
            frame, ld_fp_constant_mode_name(),
            g_ld_profile.n_ps_exact_source_hashes,
            g_ld_profile.n_ps_constant_specialized_hashes,
            g_ld_profile.n_ps_canonical_hashes,
            collapsed_by_constants, collapsed_by_alpha,
            (unsigned long long)
                g_ld_profile.ps_variant_tracking_overflow,
            (unsigned long long)
                LD_PROFILE_DELTA(ps_constant_allocations),
            (unsigned long long)
                LD_PROFILE_DELTA(ps_constant_upload_bytes),
            ld_profile_ticks_ms(
                LD_PROFILE_DELTA(ps_constant_upload_qpc)),
            g.frame->ps_cb_used, PS_CB_RING_BYTES,
            (unsigned long long)
                LD_PROFILE_DELTA(ps_constant_capacity_failures),
            (unsigned long long)LD_PROFILE_DELTA(
                flush_reason[LD_FLUSH_PIXEL_CONSTANT_RING]),
            ld_profile_ticks_ms(LD_PROFILE_DELTA(
                flush_reason_qpc[LD_FLUSH_PIXEL_CONSTANT_RING])),
            ld_profile_ticks_ms(LD_PROFILE_DELTA(
                fence_reason_qpc[LD_FLUSH_PIXEL_CONSTANT_RING])));
        fflush(stderr);
    }

    g_ld_profile.previous = g_ld_profile.total;
    g_ld_profile.previous_packets = g_ld_stats.packets_seen;
    g_ld_profile.previous_groups = g_ld_stats.groups_seen;
    g_ld_profile.previous_executed = g_ld_stats.groups_executed;
    g_ld_profile.previous_evictions = g_ld_texture_cache_evictions;
    g_ld_profile.previous_refreshes = g_ld_vtex_refreshes;
    g_ld_profile.previous_present_qpc = now;
    g_ld_profile.frame_upload_high = 0;
    g_ld_profile.frame_vb_high = 0;
    g_ld_profile.frame_retired_high = 0;
    g_ld_profile.frame_used_attribute_max = 0;
#undef LD_PROFILE_DELTA
}
#endif

static int ld_target_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("RSX_TARGET_TRACE") ? 1 : 0;
    return enabled;
}

static void ld_trace_target(const char* event, u32 target, u32 mask)
{
    if (!ld_target_trace_enabled()) return;
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    fprintf(stderr,
            "[rsx-target] frame=%u event=%s target=%u loc=%u off=0x%08X "
            "fmt=0x%X pitch=%u clip=%u,%u+%ux%u mask=0x%02X\n",
            g_ld_frames, event, target, sf.color_location[0], sf.color_offset[0],
            sf.color_format, sf.color_pitch[0], sf.clip_x, sf.clip_y,
            sf.clip_w, sf.clip_h, mask);
}

/* Movie mode: while a host-decoded movie owns the window
 * (rsx_live_draw_present_rgba), do not make the default boot process the
 * guest's otherwise invisible RSX stream.  That experiment materially changes
 * CRI/movie handoff scheduling and regressed the proven title/menu path.
 * RSX_MOVIE_TRACK keeps the state-tracking experiment available for a
 * focused post-movie A/B once the transition itself is deterministic. */
static volatile int g_ld_movie_mode = 0;
static int g_ld_movie_track_rsx = -1;
static int g_ld_movie_composite_ui = -1;

#if defined(YZ_PERF_PROFILE)
static int ld_vp_has_indexed_constants(const u8* ucode, u32 instrs)
{
    for (u32 i = 0; i < instrs; i++) {
        const u8* p = ucode + i * 16u + 12u;
        const u32 d3 = (u32)p[0] | ((u32)p[1] << 8) |
                       ((u32)p[2] << 16) | ((u32)p[3] << 24);
        if ((d3 >> 1) & 1u)
            return 1;
    }
    return 0;
}

static ld_reject_class ld_classify_rejected_pso(u32 target)
{
    const u32 start = g_ld_current_pso.vp_start;
    const u8* vp_uc = start < RSX_DSP_VP_INSTR
        ? (const u8*)(g.rsx.vp + start * 4)
        : NULL;
    if (vp_uc && ld_vp_has_indexed_constants(
            vp_uc, g_ld_current_pso.vp_instrs))
        return LD_REJECT_CHARACTER;

    render_state_t rs;
    decode_render_state(&rs);
    if (target < g.n_surfaces) {
        const surface_t* surface = &g.surfaces[target];
        int scanout = 0;
        for (u32 i = 0; i < 8; i++) {
            const display_buffer_t* display = &g.display_buffers[i];
            if (display->valid &&
                display->location == surface->location &&
                display->offset == surface->offset) {
                scanout = 1;
                break;
            }
        }
        if (scanout && !rs.depth_test && rs.blend_enable)
            return LD_REJECT_UI;
        if (rs.depth_test ||
            surface->offset == 0x00E40000u ||
            surface->offset == 0x01800000u)
            return LD_REJECT_WORLD;
    }
    return LD_REJECT_OTHER;
}

static void ld_profile_note_rejected_pso(void)
{
    if (!g_ld_current_pso.valid || g.n_psos < MAX_PSOS)
        return;

    rsx_dsp_surface rsx_surface;
    rsx_dsp_get_surface(&g.rsx, &rsx_surface);
    u32 target = LD_INVALID_SURFACE;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        if (g.surfaces[i].location == rsx_surface.color_location[0] &&
            g.surfaces[i].offset == rsx_surface.color_offset[0]) {
            target = i;
            break;
        }
    }
    const ld_reject_class draw_class = ld_classify_rejected_pso(target);
    const u8 class_bit = (u8)(1u << (u32)draw_class);
    g_ld_profile.rejected_pso_requests[draw_class]++;

    u32 key_index = g_ld_profile.n_rejected_pso_keys;
    for (u32 i = 0; i < g_ld_profile.n_rejected_pso_keys; i++) {
        if (g_ld_profile.rejected_pso_keys[i] ==
            g_ld_current_pso.key) {
            key_index = i;
            break;
        }
    }
    if (key_index == g_ld_profile.n_rejected_pso_keys) {
        if (key_index >= MAX_REJECTED_PSO_KEYS)
            return;
        g_ld_profile.rejected_pso_keys[key_index] =
            g_ld_current_pso.key;
        g_ld_profile.rejected_pso_class_masks[key_index] = 0;
        g_ld_profile.n_rejected_pso_keys++;
    }
    if (!(g_ld_profile.rejected_pso_class_masks[key_index] & class_bit)) {
        g_ld_profile.rejected_pso_class_masks[key_index] |= class_bit;
        g_ld_profile.rejected_pso_unique[draw_class]++;
    }

    if (!g_ld_profile.first_pso_full_frame) {
        g_ld_profile.first_pso_full_frame = g_ld_frames;
        const u32 surface_offset = target < g.n_surfaces
            ? g.surfaces[target].offset
            : 0;
        fprintf(stderr,
                "[pso-capacity] reached count=%u capacity=%u "
                "movie=%d orphanage_world_ready=%d "
                "first_rejected_key=%016llx class=%u "
                "surface=0x%08X frame_locator=%u\n",
                g.n_psos, MAX_PSOS, g_ld_movie_mode,
                InterlockedCompareExchange(
                    &g_ld_a010_world_ready, 0, 0) != 0,
                (unsigned long long)g_ld_current_pso.key,
                (u32)draw_class, surface_offset, g_ld_frames);
        fflush(stderr);
    }
}
#endif

static int ld_movie_composite_ui_enabled(void)
{
    if (g_ld_movie_composite_ui < 0)
        g_ld_movie_composite_ui = getenv("RSX_MOVIE_COMPOSITE_UI") ? 1 : 0;
    return g_ld_movie_composite_ui;
}

static void ld_movie_reset_rings(void)
{
    g.frame->vb_used = 0;
    g.frame->ib_used = 0;
    g.frame->cb_used = 0;
    g.frame->ps_cb_used = 0;
    g.frame->srv_ring_used = 0;
    g.frame->smp_ring_used = 0;
    g.depth_cleared = 0;
}

static int ld_movie_overlay_ensure(void)
{
    if (g.movie_upload && g.movie_upload_mapped &&
        g.movie_overlay_readback && g.movie_overlay_rgba &&
        g.movie_overlay_mask)
        return 1;

    g.movie_overlay_pitch = (g.width * 4 + 255) & ~255u;
    const UINT64 rb_size = (UINT64)g.movie_overlay_pitch * g.height;
    D3D12_HEAP_PROPERTIES hp = {0};
    D3D12_RESOURCE_DESC bd = {0};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = rb_size;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&g.movie_overlay_readback))) {
        fprintf(stderr, "[movie-ui] readback allocation failed\n");
        return 0;
    }
    g.movie_overlay_rgba = (u8*)malloc((size_t)g.width * g.height * 4);
    g.movie_overlay_mask = (u8*)malloc((size_t)g.width * g.height);
    if (!g.movie_overlay_rgba || !g.movie_overlay_mask) {
        free(g.movie_overlay_rgba);
        free(g.movie_overlay_mask);
        g.movie_overlay_rgba = NULL;
        g.movie_overlay_mask = NULL;
        g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
        g.movie_overlay_readback = NULL;
        fprintf(stderr, "[movie-ui] CPU overlay allocation failed\n");
        return 0;
    }

    /* Do not reuse the general guest upload ring for host frames. Guest draw
     * commands recorded between flips can still reference that memory; a
     * dedicated upload buffer lets host presentation append to the same
     * command list without a second fence/wait on every movie frame. */
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&g.movie_upload))) {
        free(g.movie_overlay_rgba);
        free(g.movie_overlay_mask);
        g.movie_overlay_rgba = NULL;
        g.movie_overlay_mask = NULL;
        g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
        g.movie_overlay_readback = NULL;
        fprintf(stderr, "[movie-ui] host upload allocation failed\n");
        return 0;
    }
    D3D12_RANGE no_read = {0, 0};
    if (FAILED(g.movie_upload->lpVtbl->Map(
            g.movie_upload, 0, &no_read, (void**)&g.movie_upload_mapped))) {
        g.movie_upload->lpVtbl->Release(g.movie_upload);
        g.movie_upload = NULL;
        free(g.movie_overlay_rgba);
        free(g.movie_overlay_mask);
        g.movie_overlay_rgba = NULL;
        g.movie_overlay_mask = NULL;
        g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
        g.movie_overlay_readback = NULL;
        fprintf(stderr, "[movie-ui] host upload map failed\n");
        return 0;
    }
    return 1;
}

/* Start movies from a known transparent guest surface. This removes the stale
 * pre-movie frame before the game begins drawing captions, while leaving the
 * swap chain entirely owned by the 30 Hz host presenter. */
static void ld_movie_overlay_begin(void)
{
    g.movie_overlay_valid = 0;
    g.movie_overlay_frames = 0;
    if (!ld_movie_overlay_ensure()) return;

    ld_flush(LD_FLUSH_MOVIE);
    const float transparent[4] = {0, 0, 0, 0};
    for (u32 i = 0; i < g.n_surfaces; i++) {
        rsx_rec_ClearRenderTargetView(g.frame, rtv_handle(LD_SWAP_BUFFERS + i), transparent, 0, NULL);
        ld_surface_note_write(i, LD_SURFACE_WRITE_OTHER);
    }
    if (g.n_surfaces) ld_flush(LD_FLUSH_MOVIE);
    ld_movie_reset_rings();
    fprintf(stderr, "[movie-ui] compositor armed (%ux%u, %u guest surfaces)\n",
            g.width, g.height, g.n_surfaces);
    fflush(stderr);
}

/* Capture the guest's latest offscreen result without presenting it. The
 * guest target is not a true transparent overlay: depending on the auth
 * sequence it can contain black, a fade, or a complete rendered scene.
 * Extract only low-saturation bright glyphs from the subtitle-safe lower band
 * and synthesize a small dark outline. The decoded movie therefore remains the
 * background even when the guest rendered an opaque full-screen image. */
static void ld_movie_capture_overlay(void)
{
    if (!ld_movie_overlay_ensure()) return;
    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) {
        g.movie_overlay_valid = 0;
        return;
    }
    ID3D12Resource* rt = g.surfaces[target].tex;
    if (g.resolution.scale != 1) {
        ID3D12Resource* canvas = ld_host_image(1, g.width, g.height);
        const float uv[4] = {0, 0, 1, 1};
        if (!canvas || !ld_transfer_image(rt, SRV_SURFACE_BASE + target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, canvas,
            rtv_handle(RTV_HOST_IMAGE_BASE + 1), D3D12_RESOURCE_STATE_RENDER_TARGET,
            DXGI_FORMAT_R8G8B8A8_UNORM, uv, 0, 0, g.width, g.height)) {
            g.movie_overlay_valid = 0;
            return;
        }
        rt = canvas;
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rsx_rec_ResourceBarrier(g.frame, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = g.movie_overlay_readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = g.width;
    dst.PlacedFootprint.Footprint.Height = g.height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = g.movie_overlay_pitch;
    rsx_rec_CopyTextureRegion(g.frame, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rsx_rec_ResourceBarrier(g.frame, 1, &b);
    ld_flush(LD_FLUSH_MOVIE);

    const SIZE_T rb_size = (SIZE_T)g.movie_overlay_pitch * g.height;
    u8* mapped = NULL;
    D3D12_RANGE rr = {0, rb_size};
    if (FAILED(g.movie_overlay_readback->lpVtbl->Map(
            g.movie_overlay_readback, 0, &rr, (void**)&mapped))) {
        g.movie_overlay_valid = 0;
        return;
    }
    for (u32 y = 0; y < g.height; y++)
        memcpy(g.movie_overlay_rgba + (size_t)y * g.width * 4,
               mapped + (size_t)y * g.movie_overlay_pitch,
               (size_t)g.width * 4);
    D3D12_RANGE wr = {0, 0};
    g.movie_overlay_readback->lpVtbl->Unmap(
        g.movie_overlay_readback, 0, &wr);

    const u64 total = (u64)g.width * g.height;
    const u32 band_y0 = g.height * 52 / 100;
    const u32 band_y1 = g.height * 96 / 100;
    const u32 band_x0 = g.width * 5 / 100;
    const u32 band_x1 = g.width * 95 / 100;
    const u64 band_pixels =
        (u64)(band_y1 - band_y0) * (band_x1 - band_x0);
    u64 glyph_pixels = 0;
    memset(g.movie_overlay_mask, 0, (size_t)total);

    for (u32 y = band_y0; y < band_y1; y++) {
        for (u32 x = band_x0; x < band_x1; x++) {
            const u64 i = (u64)y * g.width + x;
            const u8* p = g.movie_overlay_rgba + i * 4;
            const int hi = p[0] > p[1]
                ? (p[0] > p[2] ? p[0] : p[2])
                : (p[1] > p[2] ? p[1] : p[2]);
            const int lo = p[0] < p[1]
                ? (p[0] < p[2] ? p[0] : p[2])
                : (p[1] < p[2] ? p[1] : p[2]);
            if (hi < 145 || hi - lo > 52)
                continue;
            int coverage = (hi - 96) * 255 / 159;
            if (coverage < 64) coverage = 64;
            if (coverage > 255) coverage = 255;
            g.movie_overlay_mask[i] = (u8)coverage;
            glyph_pixels++;
        }
    }

    /* A value of 1 denotes the synthetic outline; 64..255 are glyph
     * coverage. Work from a copy condition (>=64) so dilation does not grow
     * recursively. */
    if (glyph_pixels >= 8 && glyph_pixels < band_pixels / 6) {
        for (u32 y = band_y0; y < band_y1; y++) {
            for (u32 x = band_x0; x < band_x1; x++) {
                const u64 i = (u64)y * g.width + x;
                if (g.movie_overlay_mask[i] < 64)
                    continue;
                const u32 ya = y > 1 ? y - 2 : 0;
                const u32 yb = y + 2 < g.height ? y + 2 : g.height - 1;
                const u32 xa = x > 1 ? x - 2 : 0;
                const u32 xb = x + 2 < g.width ? x + 2 : g.width - 1;
                for (u32 oy = ya; oy <= yb; oy++)
                    for (u32 ox = xa; ox <= xb; ox++) {
                        u8* m = &g.movie_overlay_mask[(u64)oy * g.width + ox];
                        if (!*m) *m = 1;
                    }
            }
        }
    }
    g.movie_overlay_valid =
        glyph_pixels >= 8 && glyph_pixels < band_pixels / 6;
    g.movie_overlay_frames++;
    if (g.movie_overlay_frames <= 16 ||
        (g.movie_overlay_frames & 63) == 0 ||
        (!g.movie_overlay_valid && glyph_pixels)) {
        fprintf(stderr,
                "[movie-ui] overlay=%llu glyphs=%llu/%llu %s\n",
                (unsigned long long)g.movie_overlay_frames,
                (unsigned long long)glyph_pixels,
                (unsigned long long)band_pixels,
                g.movie_overlay_valid ? "accepted" : "rejected");
        fflush(stderr);
    }

    /* A guest flip still marks a new texture-generation boundary even though
     * the guest surface is not sent to the swap chain. */
    g_ld_frames++;
    ld_movie_reset_rings();
}

static void sink_begin(void* u, const rsx_dispatch* r, u32 prim) { (void)u; (void)r; (void)prim; dc_reset(); }
static void sink_draw_arrays(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r; g_ld_stats.packets_seen++;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) {
        g_ld_stats.packets_movie++;
        return;
    }
    dc.n_packets++;
    if (dc.n_arr >= 256) { g_ld_stats.packets_queue_full++; return; }
    dc.arr[dc.n_arr].first = first; dc.arr[dc.n_arr].count = count; dc.n_arr++;
    g_ld_stats.packets_queued++;
}
/* INLINE_ARRAY arrives as one finished stream instead of a (first, count)
 * range, so plan the layout here -- offsets and stride come from the VTXFMT
 * declaration -- and queue it as an ordinary consecutive batch. Everything
 * downstream (topology, textures, PSO, upload) is then identical to a draw
 * out of a vertex array, because by then it IS one. */
static void sink_inline_array(void* u, const rsx_dispatch* r,
                              const u8* data, u32 bytes)
{
    (void)u; (void)r;
    g_ld_stats.packets_seen++;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) {
        g_ld_stats.packets_movie++;
        return;
    }

    const u32 stride = rsx_vertex_inline_layout(&g.rsx, dc.inl_off);
    if (!stride || bytes < stride) { g_ld_stats.packets_queue_full++; return; }

    dc.inl = data; dc.inl_bytes = bytes; dc.inl_stride = stride;
    dc.n_packets++;
    if (dc.n_arr >= 256) { g_ld_stats.packets_queue_full++; return; }
    dc.arr[dc.n_arr].first = 0; dc.arr[dc.n_arr].count = bytes / stride;
    dc.n_arr++;
    g_ld_stats.packets_queued++;
}

static void sink_draw_index(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r; g_ld_stats.packets_seen++;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) {
        g_ld_stats.packets_movie++;
        return;
    }
    dc.n_packets++;
    if (dc.n_idx >= 256) { g_ld_stats.packets_queue_full++; return; }
    dc.idx[dc.n_idx].first = first; dc.idx[dc.n_idx].count = count; dc.n_idx++;
    g_ld_stats.packets_queued++;
}

void rsx_live_draw_set_a010_camera_matrix(const float* matrix16)
{
    if (!matrix16) {
        InterlockedExchange(&g_ld_a010_camera_ready, 0);
        return;
    }
    memcpy(g_ld_a010_camera_bits, matrix16, sizeof(g_ld_a010_camera_bits));
    MemoryBarrier();
    InterlockedExchange(&g_ld_a010_camera_ready, 1);
}

/* Hash the same pre-cubemap/pre-VTF PSO inputs used by the reference replay's
 * RSX_DRAW_CSV.  Keeping this legacy key lets a live draw be matched directly
 * to the known-good a010 capture even though the current live PSO key contains
 * additional feature masks. */
static u64 live_legacy_pso_key(void)
{
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start >= RSX_DSP_VP_INSTR)
        return 0;
    const u8* vp_uc = (const u8*)(g.rsx.vp + start * 4);
    const u32 vp_instrs = rsx_vp_program_size_instrs(
        vp_uc, (RSX_DSP_VP_INSTR - start) * 16);
    if (!vp_instrs)
        return 0;

    u32 fp_loc = 0;
    const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
    const u8* fp_uc = guest_ptr(fp_loc, fp_off, 16);
    if (!fp_uc)
        return 0;
    const u32 fp_size = rsx_fp_program_size(fp_uc, 0x10000);
    if (!fp_size)
        return 0;
    fp_uc = guest_ptr(fp_loc, fp_off, fp_size);
    if (!fp_uc)
        return 0;

    u64 key = fnv1a(vp_uc, vp_instrs * 16, 1469598103934665603ull);
    key = fnv1a(fp_uc, fp_size, key);
    const u32 fp_ctrl_key = rsx_dsp_shader_control(&g.rsx) & 0x4Eu;
    key = fnv1a(&fp_ctrl_key, sizeof(fp_ctrl_key), key);
    render_state_t rs;
    decode_render_state(&rs);
    return fnv1a(&rs, sizeof(rs), key);
}

/* Hash the canonical sixteen-attribute vertex stream in source occurrence
 * order.  Compact mode may deduplicate the uploaded payload, so hashing its
 * packed bytes directly is not comparable with the legacy replay renderer.
 * Re-expanding through the remap and applying the RSX defaults produces the
 * same vtx_t byte stream on both paths and places the producer/consumer
 * boundary immediately after guest-memory vertex decoding. */
static u64 live_decoded_vertex_hash(u64 attr_hash[16])
{
    u64 hash = 1469598103934665603ull;
    for (u32 attr = 0; attr < 16; attr++)
        attr_hash[attr] = 1469598103934665603ull;
    const u32 count = dc.n_source_refs ? dc.n_source_refs : dc.n_verts;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const u32 vertex = dc.refs_remapped
            ? rsx_vertex_remap_index(&dc.ref_remap, occurrence)
            : occurrence;
        for (u32 attr = 0; attr < 16; attr++) {
            float fallback[4];
            const float* value =
                dc_vertex_attr_canonical(vertex, attr, fallback);
            hash = fnv1a(value, 16u, hash);
            attr_hash[attr] = fnv1a(value, 16u, attr_hash[attr]);
        }
    }
    return hash;
}

/* RSX_DRAW_CSV=path: uncapped per-draw fingerprints for direct comparison
 * with the working RPCS3 .rxs replay.  Default-off and renderer-neutral. */
/* The open RSX_DRAW_CSV and its gate, for marker rows from outside a draw
 * (rsx_live_draw_blit notes each 2D copy where it falls in the draw stream). */
static FILE* s_csv_file;
static int   s_csv_gate_open = 1;
static u64 s_csv_last_draw;

static void live_draw_csv_emit(u32 prim, u32 n_tri, const char* outcome)
{
#if defined(YZ_PERF_CLEAN) && !defined(YZ_PERF_PROFILE)
    (void)prim;
    (void)n_tri;
    (void)outcome;
#else
    static int inited = 0;
    static FILE* file = NULL;
    static u64 draw = 0;
    s_csv_gate_open = 0;
    /* Optional wall-time gate keeps a short state audit out of startup. */
    { static ULONGLONG origin, after;
      if (!origin) { const char* e = getenv("RSX_DRAW_CSV_AFTER");
          origin = GetTickCount64(); after = e ? 1000ull * strtoull(e, NULL, 10) : 0; }
      if (after && GetTickCount64() - origin < after) return; }
    /* RSX_DRAW_CSV_FROM=<frame>: skip rows before that frame. Logging every
     * draw from boot slows a 10-minute run to an hour. */
    { static long from = -1;
      if (from < 0) { const char* e = getenv("RSX_DRAW_CSV_FROM"); from = e ? atol(e) : 0; }
      if ((long)g_ld_frames < from) return; }
    /* Bound diagnostic captures to complete frames. Unbounded per-draw logs
     * distort pacing and make a single-pass investigation unnecessarily large. */
    { static int limit = -1; static u32 first = UINT32_MAX;
      if (limit < 0) { const char* e = getenv("RSX_DRAW_CSV_FRAMES"); limit = e ? atoi(e) : 0; }
      if (limit > 0) {
          if (first == UINT32_MAX) first = g_ld_frames;
          if (g_ld_frames - first >= (u32)limit) return;
      } }
    /* RSX_DRAW_CSV_GATE=<file>: rows only while <file> exists (polled every
     * 512 draws), so a capture can be switched on when the scene of interest
     * is on screen instead of guessing its frame number. */
    { static const char* gate = (const char*)-1; static int open_gate = 0; static unsigned poll;
      if (gate == (const char*)-1) gate = getenv("RSX_DRAW_CSV_GATE");
      if (gate) {
          if ((poll++ & 511u) == 0) { struct stat st; open_gate = stat(gate, &st) == 0; }
          s_csv_gate_open = open_gate;
          if (!open_gate) return;
      } }
    s_csv_gate_open = 1;
    static int a010_only = -1;
    if (a010_only < 0) a010_only = getenv("RSX_DRAW_CSV_A010_ONLY") != NULL;
    if (a010_only) {
        if (InterlockedCompareExchange(
                &g_yz_a010_root_active, 0, 0) == 0)
            return;
    }
    if (!inited) {
        inited = 1;
        const char* path = getenv("RSX_DRAW_CSV");
        if (path && path[0]) {
            file = fopen(path, "w");
            s_csv_file = file;
            if (file) {
                fprintf(file,
                    "draw,frame,outcome,surf,prim,verts,source_verts,decoded_hash,"
                    "pso_key,regs_hash,vp_hash,"
                    "const_hash,blend,dtest,dwrite,dfunc,cull,cullface,"
                    "frontface,cmask,vpx,vpy,vpw,vph,sclx,scly,sclz,"
                    "trnx,trny,trnz,clipw,cliph,zeta_off,zeta_pitch,zeta_loc,"
                    "seen_vtex,seen_vtxfmt,seen_freqdiv,"
                    "attr0_hash,attr1_hash,attr2_hash,attr3_hash,"
                    "attr4_hash,attr5_hash,attr6_hash,attr7_hash,"
                    "attr8_hash,attr9_hash,attr10_hash,attr11_hash,"
                    "attr12_hash,attr13_hash,attr14_hash,attr15_hash,"
                    "active_attr_mask,used_attr_mask,tex\n");
                fprintf(stderr, "[live-diff] RSX_DRAW_CSV armed: %s\n", path);
            } else {
                fprintf(stderr, "[live-diff] cannot open RSX_DRAW_CSV: %s\n",
                        path);
            }
            fflush(stderr);
        }
    }
    if (!file)
        return;

    s_csv_last_draw = draw;
    const u32 target = current_surface();
    const u32 surf = target < g.n_surfaces ? g.surfaces[target].offset : 0;
    /* LD_DRAW_DUMP=<dir> [LD_DRAW_DUMP_EVERY=k, default 10]: in the first
     * gated frame, read back each k-th draw's target BEFORE it is recorded,
     * named by frame-relative index -- bisects which draw changes a surface. */
    { static const char* dir = (const char*)-1; static u32 every, frame = ~0u, idx, dumped_frame = ~0u;
      if (dir == (const char*)-1) { dir = getenv("LD_DRAW_DUMP");
          const char* e = getenv("LD_DRAW_DUMP_EVERY"); every = e ? (u32)atoi(e) : 10; if (!every) every = 10; }
      if (frame != g_ld_frames) { frame = g_ld_frames; idx = 0; }
      if (dir && target < g.n_surfaces && (dumped_frame == ~0u || dumped_frame == frame) &&
          (idx % every) == 0) {
          dumped_frame = frame;
          char path[MAX_PATH * 2];
          snprintf(path, sizeof(path), "%s\\draw_f%06u_i%04u_%08X.ppm", dir, frame, idx, surf);
          ld_dump_surface_ppm(path, &g.surfaces[target]);
      }
      idx++; }
    const u64 regs_hash = fnv1a(
        g.rsx.regs, RSX_DSP_NUM_REGS * sizeof(g.rsx.regs[0]),
        1469598103934665603ull);
    const u64 vp_hash = fnv1a(
        g.rsx.vp, sizeof(g.rsx.vp), 1469598103934665603ull);
    const u64 const_hash = fnv1a(
        g.rsx.constants, sizeof(g.rsx.constants), 1469598103934665603ull);
    render_state_t rs;
    decode_render_state(&rs);
    rsx_dsp_viewport vp;
    rsx_dsp_get_viewport(&g.rsx, &vp);
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);

    u32 seen_vtex = 0, seen_vtxfmt = 0;
    for (u32 i = 0; i < 32; i++)
        seen_vtex += g.rsx.seen[(0x0900u >> 2) + i];
    for (u32 i = 0; i < 16; i++)
        seen_vtxfmt += g.rsx.seen[(0x1740u >> 2) + i];
    const u32 seen_freqdiv = g.rsx.seen[0x1FC0u >> 2];
    u64 attr_hash[16];
    const u64 decoded_hash = live_decoded_vertex_hash(attr_hash);
    u32 active_attr_mask = 0;
    for (u32 attr = 0; attr < 16; attr++)
        if (dc.fetch_plan.attr[attr].desc.type &&
            dc.fetch_plan.attr[attr].desc.size)
            active_attr_mask |= 1u << attr;

    fprintf(file,
        "%llu,%u,%s,0x%X,%u,%u,%u,%016llx,"
        "%016llx,%016llx,%016llx,%016llx,"
        "%u,%u,%u,0x%X,%u,0x%X,0x%X,0x%08X,"
        "%u,%u,%u,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
        "%u,%u,0x%X,%u,%u,%u,%u,%u",
        (unsigned long long)draw++, g_ld_frames, outcome, surf, prim, n_tri,
        dc.n_source_refs ? dc.n_source_refs : dc.n_verts,
        (unsigned long long)decoded_hash,
        (unsigned long long)live_legacy_pso_key(),
        (unsigned long long)regs_hash, (unsigned long long)vp_hash,
        (unsigned long long)const_hash,
        rs.blend_enable, rs.depth_test, rs.depth_write, rs.depth_func,
        rs.cull_enable, rs.cull_face, rs.front_face, rs.color_mask,
        vp.x, vp.y, vp.w, vp.h,
        vp.scale[0], vp.scale[1], vp.scale[2],
        vp.translate[0], vp.translate[1], vp.translate[2],
        sf.clip_w, sf.clip_h, sf.zeta_offset, sf.zeta_pitch, sf.zeta_location,
        seen_vtex, seen_vtxfmt, seen_freqdiv);
    for (u32 attr = 0; attr < 16; attr++)
        fprintf(file, ",%016llx", (unsigned long long)attr_hash[attr]);
    fprintf(file, ",0x%04X,0x%04X,",
            active_attr_mask, dc.layout.mask & 0xFFFFu);
    /* Enabled fragment texture units, so a pass that samples a surface (or an
     * empty copy of one) is visible in the row: unit:loc:offset:fmt:WxH. */
    for (u32 u = 0; u < 16; u++) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled) {
            fprintf(file, "%u:%u:%X:%02X:%ux%u ", u, t.location, t.offset,
                    t.format & 0xFF, t.width, t.height);
            fprintf(file, "s%u=%08X/%08X/%08X/%08X/%08X ", u,
                t.wrap, t.filter, t.control0, t.border_color, t.remap);
        }
    }
    if (rs.blend_enable)
        fprintf(file, "bf=%X/%X/%X/%X eq=%X/%X bc=%08X ", rs.sf_rgb, rs.df_rgb, rs.sf_a, rs.df_a,
                rs.eq_rgb, rs.eq_a, rsx_dsp_reg(&g.rsx, NV4097_SET_BLEND_COLOR));
    /* ANTI_ALIASING_CONTROL (enable, alpha-to-coverage, alpha-to-one, sample mask) and MRT colour mask. */
    fprintf(file, "aa=%08X mrtm=%08X ", rsx_dsp_reg(&g.rsx, 0x1D7C), rsx_dsp_reg(&g.rsx, 0x0370));
    fprintf(file, "at=%u/0x%X/0x%X ", rs.alpha_test_enable, rs.alpha_func, rs.alpha_ref_raw);
    if (rs.logic_enable) fprintf(file, "lo=%X ", rs.logic_op);
    /* SHADE_MODE, LINE_WIDTH, LINE_SMOOTH_ENABLE, RESTART_INDEX_ENABLE (libfont's outline passes). */
    fprintf(file, "sh=%X lw=%X ls=%u ri=%u ", rsx_dsp_reg(&g.rsx, 0x0368), rsx_dsp_reg(&g.rsx, 0x03B8),
            rsx_dsp_reg(&g.rsx, 0x03BC) & 1u, rsx_dsp_reg(&g.rsx, 0x1DAC) & 1u);
    fprintf(file, "zc=%08X uclip=%08X pso=%016llx ",
        rsx_dsp_reg(&g.rsx, M_ZMIN_MAX_CONTROL), rsx_dsp_reg(&g.rsx, 0x1478),
        (unsigned long long)g_ld_current_pso.key);
    fprintf(file, "sc=%08X/%08X ", rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL), rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL));
    if (rs.stencil_enable)
        fprintf(file, "st=%X/%X/%X/%X/%X/%X ref=%X ", rs.s_func, rs.s_func_mask, rs.s_write_mask, rs.s_fail, rs.s_zfail, rs.s_zpass, rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC + 4));
    /* Inline draws (HUD sprites): the largest vertex-colour alpha byte, attr 3
     * as UB4 -- an element faded to 0 is drawn but invisible. */
    if (dc.inl && dc.inl_stride) {
        rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, 3, &va);
        if (va.type == RSX_VTX_TYPE_UNORM8 && va.size == 4) {
            u32 amax = 0;
            for (u32 o = dc.inl_off[3] + 3; o < dc.inl_bytes; o += dc.inl_stride)
                if (dc.inl[o] > amax) amax = dc.inl[o];
            fprintf(file, "va=%u ", amax);
        }
        /* ...and the xy bounding box of attr 0 when it is float (screen-space HUD). */
        rsx_dsp_vertex_attr p0; rsx_dsp_get_vertex_attr(&g.rsx, 0, &p0);
        if (p0.type == RSX_VTX_TYPE_FLOAT && p0.size >= 2) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (u32 o = dc.inl_off[0]; o + 8 <= dc.inl_bytes; o += dc.inl_stride) {
                u32 xb = (dc.inl[o] << 24) | (dc.inl[o+1] << 16) | (dc.inl[o+2] << 8) | dc.inl[o+3];
                u32 yb = (dc.inl[o+4] << 24) | (dc.inl[o+5] << 16) | (dc.inl[o+6] << 8) | dc.inl[o+7];
                float x, y; memcpy(&x, &xb, 4); memcpy(&y, &yb, 4);
                if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
            }
            fprintf(file, "bb=%.0f,%.0f-%.0f,%.0f ", x0, y0, x1, y1);
        }
    }
    fprintf(file, "A=%u:%X/%u fmt=%u rt=%u ", sf.color_location[0], sf.color_offset[0], sf.color_pitch[0], sf.color_format, sf.raster_type);
    fprintf(file, "ct=0x%X B=%u:%X C=%u:%X D=%u:%X ", sf.color_target,
            sf.color_location[1], sf.color_offset[1], sf.color_location[2], sf.color_offset[2],
            sf.color_location[3], sf.color_offset[3]);
    fputc('\n', file);
    fflush(file);
#endif
}

/* RSX_A010_GEOM: trace one stable orphanage mesh.  This is draw 520 from
 * the healthy a010 reference: a 270-vertex triangle strip on the main world
 * surface with PSO 7d9f....  The model-initialization repair now reproduces
 * this exact tuple in the live stream.  Keeping the tuple exact makes the gate
 * quiet outside a010 while allowing capture/playback only after known-good
 * world geometry has actually reached the backend. */
static void live_a010_geom_trace(u32 prim, u32 n_tri)
{
    static int enabled = -1;
    static u32 logged = 0;
    static u32 reference_logged = 0;
    if (enabled < 0)
        enabled = getenv("RSX_A010_GEOM") ? 1 : 0;
    if (prim != PRIM_TRIANGLE_STRIP || n_tri != 270)
        return;

    const u32 target = current_surface();
    const u32 surf =
        target < g.n_surfaces ? g.surfaces[target].offset : 0;
    const u64 key = live_legacy_pso_key();
    if (surf != 0x01800000u || key != 0x7d9f528329f20c52ull)
        return;
    /*
     * This topology/PSO/surface tuple is the stable Kiryu mesh measured in
     * the healthy a010 capture. Reaching this point means vertex fetch has
     * completed and the scene has produced real world geometry, rather than
     * only clears and fullscreen post-processing draws.
     */
    if (InterlockedCompareExchange(&g_ld_a010_world_ready, 1, 0) == 0) {
        fprintf(stderr,
                "[a010-world-ready] frame=%u surface=0x%X "
                "pso=%016llx vertices=%u\n",
                g_ld_frames, surf, (unsigned long long)key, n_tri);
        fflush(stderr);
    }
    if (!enabled)
        return;
    if (InterlockedCompareExchange(
            &g_yz_a010_reference_camera_active, 0, 0) != 0) {
        if (reference_logged >= 8)
            return;
        reference_logged++;
    } else {
        if (logged >= 8)
            return;
        logged++;
    }

    float mn[4] = {1e30f, 1e30f, 1e30f, 1e30f};
    float mx[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
    u32 nan_count = 0;
    for (u32 vi = 0; vi < dc.n_verts; vi++) {
        float fallback[4];
        const float* p = dc_vertex_attr(vi, 0, fallback);
        for (u32 k = 0; k < 4; k++) {
            if (p[k] != p[k]) {
                nan_count++;
                continue;
            }
            if (p[k] < mn[k]) mn[k] = p[k];
            if (p[k] > mx[k]) mx[k] = p[k];
        }
    }

    const u32 base = rsx_dsp_vertex_data_base_offset(&g.rsx);
    const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
    rsx_dsp_index_array ia;
    rsx_dsp_get_index_array(&g.rsx, &ia);
    rsx_dsp_vertex_attr a0, a7;
    rsx_dsp_get_vertex_attr(&g.rsx, 0, &a0);
    rsx_dsp_get_vertex_attr(&g.rsx, 7, &a7);

    fprintf(stderr,
        "[a010-geom] match=%u frame=%u fetched=%u expanded=%u cuts=%u "
        "pos=[%.6g %.6g %.6g %.6g]-[%.6g %.6g %.6g %.6g] nan=%u "
        "decoded_hash=%016llx\n",
        logged, g_ld_frames, dc.n_verts, n_tri, dc.n_cuts,
        mn[0], mn[1], mn[2], mn[3], mx[0], mx[1], mx[2], mx[3],
        nan_count,
        (unsigned long long)fnv1a(
            ld_vertex_compact_payload()
                ? (const void*)dc.compact_verts
                : (const void*)dc.verts,
            ld_vertex_compact_payload()
                ? (u32)((u64)dc.n_verts * dc.layout.stride)
                : (u32)((u64)dc.n_verts * sizeof(vtx_t)),
            1469598103934665603ull));
    fprintf(stderr,
        "[a010-geom] base=0x%X base_index=%u "
        "idx(loc=%u off=0x%X u32=%u first=%u count=%u) "
        "a0(type=%u size=%u stride=%u loc=%u off=0x%X freq=%u) "
        "a7(type=%u size=%u stride=%u loc=%u off=0x%X freq=%u)\n",
        base, base_index,
        ia.location, ia.offset, ia.is_u32,
        dc.n_idx ? dc.idx[0].first : 0,
        dc.n_idx ? dc.idx[0].count : 0,
        a0.type, a0.size, a0.stride, a0.location, a0.offset, a0.frequency,
        a7.type, a7.size, a7.stride, a7.location, a7.offset, a7.frequency);

    for (u32 slot = 108; slot <= 115; slot++) {
        const u32* c = rsx_dsp_constant(&g.rsx, slot);
        union { u32 u; float f; } v[4];
        for (u32 k = 0; k < 4; k++) v[k].u = c[k];
        fprintf(stderr,
            "[a010-geom] c%u=(%.6g %.6g %.6g %.6g) "
            "raw=%08X/%08X/%08X/%08X\n",
            slot, v[0].f, v[1].f, v[2].f, v[3].f,
            v[0].u, v[1].u, v[2].u, v[3].u);
    }
    {
        const u32* c = rsx_dsp_constant(&g.rsx, 467);
        union { u32 u; float f; } v[4];
        for (u32 k = 0; k < 4; k++) v[k].u = c[k];
        fprintf(stderr,
            "[a010-geom] c467=(%.6g %.6g %.6g %.6g)\n",
            v[0].f, v[1].f, v[2].f, v[3].f);
    }
    {
        float camera[4][4];
        float c467[4];
        for (u32 row = 0; row < 4; row++)
            memcpy(camera[row], rsx_dsp_constant(&g.rsx, 108u + row),
                   sizeof(camera[row]));
        memcpy(c467, rsx_dsp_constant(&g.rsx, 467), sizeof(c467));
        rsx_dsp_viewport vp;
        rsx_dsp_get_viewport(&g.rsx, &vp);
        float sx_min = 1e30f, sy_min = 1e30f;
        float sx_max = -1e30f, sy_max = -1e30f;
        float w_min = 1e30f, w_max = -1e30f;
        u32 finite = 0, in_front = 0, in_view = 0;
        for (u32 vi = 0; vi < dc.n_verts; vi++) {
            float p_fallback[4], weight_fallback[4], bone_fallback[4];
            const float* p = dc_vertex_attr(vi, 0, p_fallback);
            const float* weight =
                dc_vertex_attr(vi, 1, weight_fallback);
            const float* bone =
                dc_vertex_attr(vi, 7, bone_fallback);
            const float pw = p[3] != 0.0f ? p[3] : 1.0f;
            /*
             * Exact transform used by healthy PSO 7d9f...: ATTR7 selects
             * four 3-row bone matrices at c112+, ATTR1.yzw supplies weights
             * 1..3, and weight 0 is implicit.  The old probe incorrectly
             * treated c112..c115 as one fixed 4x4 matrix.
             */
            const float influence[4] = {
                c467[1] - weight[1] - weight[2] - weight[3],
                weight[1], weight[2], weight[3]
            };
            float skin[3][4] = {{0}};
            for (u32 inf = 0; inf < 4; inf++) {
                const u32 base_slot =
                    (u32)(bone[inf] * c467[0]);
                for (u32 row = 0; row < 3; row++) {
                    const float* palette = (const float*)
                        rsx_dsp_constant(
                            &g.rsx, (112u + base_slot + row) & 511u);
                    for (u32 lane = 0; lane < 4; lane++)
                        skin[row][lane] +=
                            influence[inf] * palette[lane];
                }
            }
            const float r7x =
                p[0]*skin[0][0] + p[1]*skin[0][1] +
                p[2]*skin[0][2] + pw*skin[0][3];
            const float r7y =
                p[0]*skin[1][0] + p[1]*skin[1][1] +
                p[2]*skin[1][2] + pw*skin[1][3];
            const float r7z =
                p[0]*skin[2][0] + p[1]*skin[2][1] +
                p[2]*skin[2][2] + pw*skin[2][3];
            const float r4w =
                c467[2] * (p[0] + p[1] + p[2]) + pw;
            const float cx =
                r7x*camera[0][0] + r7y*camera[0][1] +
                r7z*camera[0][2] + r4w*camera[0][3];
            const float cy =
                r7x*camera[1][0] + r7y*camera[1][1] +
                r7z*camera[1][2] + r4w*camera[1][3];
            const float cw =
                r7x*camera[3][0] + r7y*camera[3][1] +
                r7z*camera[3][2] + r4w*camera[3][3];
            if (cx != cx || cy != cy || cw != cw || cw == 0.0f)
                continue;
            finite++;
            if (cw > 0.0f) in_front++;
            if (cw < w_min) w_min = cw;
            if (cw > w_max) w_max = cw;
            const float sx = (cx / cw) * vp.scale[0] + vp.translate[0];
            const float sy = (cy / cw) * vp.scale[1] + vp.translate[1];
            if (sx < sx_min) sx_min = sx;
            if (sx > sx_max) sx_max = sx;
            if (sy < sy_min) sy_min = sy;
            if (sy > sy_max) sy_max = sy;
            if (cw > 0.0f && sx >= 0.0f && sx < (float)vp.w &&
                sy >= 0.0f && sy < (float)vp.h)
                in_view++;
        }
        fprintf(stderr,
                "[a010-clip] frame=%u finite=%u/%u front=%u view=%u "
                "screen=[%.3f %.3f]-[%.3f %.3f] w=[%.6g %.6g] "
                "viewport=%ux%u\n",
                g_ld_frames, finite, dc.n_verts, in_front, in_view,
                sx_min, sy_min, sx_max, sy_max, w_min, w_max,
                vp.w, vp.h);
        if (InterlockedCompareExchange(
                &g_yz_a010_reference_camera_active, 0, 0) != 0) {
            static LONG acceptance_written = 0;
            if (InterlockedCompareExchange(
                    &acceptance_written, 1, 0) == 0) {
                FILE* acceptance =
                    fopen("scratch\\a010_acceptance.txt", "a");
                if (acceptance) {
                    fprintf(acceptance,
                            "reference-mesh frame=%u finite=%u/%u "
                            "front=%u view=%u screen=%.3f,%.3f,"
                            "%.3f,%.3f viewport=%u,%u\n",
                            g_ld_frames, finite, dc.n_verts,
                            in_front, in_view, sx_min, sy_min,
                            sx_max, sy_max, vp.w, vp.h);
                    fclose(acceptance);
                }
            }
        }
    }
    fflush(stderr);
}

typedef enum {
    LD_COMPACT_EXPAND_OK = 1,
    LD_COMPACT_EXPAND_DEGENERATE = 0,
    LD_COMPACT_EXPAND_ALLOC = -1,
    LD_COMPACT_EXPAND_PRIMITIVE = -2
} ld_compact_expand_result;

static void compact_vertex_copy(
    u8* destination, u32 destination_vertex, u32 source_vertex)
{
    if (!dc.layout.stride)
        return;
    memcpy(
        destination + (u64)destination_vertex * dc.layout.stride,
        dc.compact_verts + (u64)source_vertex * dc.layout.stride,
        dc.layout.stride);
}

static ld_compact_expand_result expand_compact_topology(
    u32 prim, u8** expanded, u32* expanded_vertices, int* owned)
{
    *expanded = dc.compact_verts;
    *expanded_vertices = 0;
    *owned = 0;
    const u32 segment_count = dc.n_cuts + 1;
    switch (prim) {
    case PRIM_TRIANGLES:
        *expanded_vertices = dc.n_verts - dc.n_verts % 3;
        break;
    case PRIM_TRIANGLE_STRIP: {
        if (dc.n_verts < 3)
            return LD_COMPACT_EXPAND_DEGENERATE;
        u32 total = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_verts, segment, &begin, &count);
            if (count >= 3) total += (count - 2) * 3;
        }
        if (!total)
            return LD_COMPACT_EXPAND_DEGENERATE;
        *expanded_vertices = total;
        if (!dc.layout.stride)
            break;
        *expanded = (u8*)malloc((size_t)total * dc.layout.stride);
        if (!*expanded)
            return LD_COMPACT_EXPAND_ALLOC;
        *owned = 1;
        u32 write = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_verts, segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 0; i + 2 < count; i++) {
                if (i & 1) {
                    compact_vertex_copy(*expanded, write++, begin + i + 1);
                    compact_vertex_copy(*expanded, write++, begin + i);
                    compact_vertex_copy(*expanded, write++, begin + i + 2);
                } else {
                    compact_vertex_copy(*expanded, write++, begin + i);
                    compact_vertex_copy(*expanded, write++, begin + i + 1);
                    compact_vertex_copy(*expanded, write++, begin + i + 2);
                }
            }
        }
        break;
    }
    case PRIM_TRIANGLE_FAN: {
        if (dc.n_verts < 3)
            return LD_COMPACT_EXPAND_DEGENERATE;
        u32 total = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_verts, segment, &begin, &count);
            if (count >= 3) total += (count - 2) * 3;
        }
        if (!total)
            return LD_COMPACT_EXPAND_DEGENERATE;
        *expanded_vertices = total;
        if (!dc.layout.stride)
            break;
        *expanded = (u8*)malloc((size_t)total * dc.layout.stride);
        if (!*expanded)
            return LD_COMPACT_EXPAND_ALLOC;
        *owned = 1;
        u32 write = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_verts, segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                compact_vertex_copy(*expanded, write++, begin);
                compact_vertex_copy(*expanded, write++, begin + i);
                compact_vertex_copy(*expanded, write++, begin + i + 1);
            }
        }
        break;
    }
    case PRIM_QUADS: {
        const u32 quads = dc.n_verts / 4;
        if (!quads)
            return LD_COMPACT_EXPAND_DEGENERATE;
        *expanded_vertices = quads * 6;
        if (!dc.layout.stride)
            break;
        *expanded = (u8*)malloc(
            (size_t)*expanded_vertices * dc.layout.stride);
        if (!*expanded)
            return LD_COMPACT_EXPAND_ALLOC;
        *owned = 1;
        u32 write = 0;
        for (u32 quad = 0; quad < quads; quad++) {
            const u32 base = quad * 4;
            compact_vertex_copy(*expanded, write++, base);
            compact_vertex_copy(*expanded, write++, base + 1);
            compact_vertex_copy(*expanded, write++, base + 2);
            compact_vertex_copy(*expanded, write++, base + 2);
            compact_vertex_copy(*expanded, write++, base + 3);
            compact_vertex_copy(*expanded, write++, base);
        }
        break;
    }
    default:
        return LD_COMPACT_EXPAND_PRIMITIVE;
    }
    return *expanded_vertices
        ? LD_COMPACT_EXPAND_OK : LD_COMPACT_EXPAND_DEGENERATE;
}

static u32 topology_index_count(u32 prim)
{
    return rsx_vertex_topology_index_count(prim, dc.n_source_refs, dc.cuts, dc.n_cuts);
}

static void write_topology_indices(u32 prim, u32* indices)
{
    rsx_vertex_write_topology_indices(prim, dc.n_source_refs, dc.cuts, dc.n_cuts,
        dc.refs_remapped ? dc.ref_remap.occurrence_to_unique : NULL, indices);
}

/* SET_SHADE_MODE FLAT (GL_FLAT): RSX colours a whole primitive from its last
 * vertex, as GL does; D3D12 uses the first. libfont's vector glyphs depend on
 * it: the vertex program gives the vertices that join one contour to the next
 * alpha 0, and with flat shading and the alpha test (NOTEQUAL 0) the fan
 * triangle and the outline segment they provoke are discarded whole.
 * Interpolated, they survived as slivers and lines across every glyph with
 * more than one contour (o, e, q, b...). The colour inputs are declared
 * nointerpolation (ld_fp_flat_inputs) and each indexed primitive is rotated so
 * that its GL provoking vertex comes first; rotation keeps the winding.
 * Polygons already start with theirs (GL takes a polygon's first vertex);
 * quads and non-indexed triangle lists keep D3D12's choice.
 * RSX_FLAT_SHADING=0 interpolates as before. */
static int ld_flat_shading(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_FLAT_SHADING"); on = !(e && e[0] == '0'); }
    return on && rsx_dsp_reg(&g.rsx, 0x0368u /* SET_SHADE_MODE */) == 0x1D00u;
}
static void ld_flat_provoking_first(u32 prim, u32* indices, u32 count)
{
    switch (prim) {
    case RSX_PRIMITIVE_LINE_STRIP:
        for (u32 i = 0; i + 1 < count; i += 2) {
            const u32 t = indices[i]; indices[i] = indices[i + 1]; indices[i + 1] = t;
        }
        break;
    case RSX_PRIMITIVE_TRIANGLES:
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
        for (u32 i = 0; i + 2 < count; i += 3) {
            const u32 last = indices[i + 2];
            indices[i + 2] = indices[i + 1]; indices[i + 1] = indices[i]; indices[i] = last;
        }
        break;
    default:
        break;
    }
}

/* YZ_PERF_PROFILE: time between marks inside a draw's preparation; stage k is the
 * span ending at LD_STAGE(k). Printed every 20000 completed draws. */
#if defined(YZ_PERF_PROFILE)
static u64 g_ld_stage_qpc[10], g_ld_stage_draws;
static LONGLONG g_ld_stage_t;
#define LD_STAGE_BEGIN() (g_ld_stage_t = ld_profile_qpc())
#define LD_STAGE(k) do { const LONGLONG _n = ld_profile_qpc(); g_ld_stage_qpc[k] += (u64)(_n - g_ld_stage_t); g_ld_stage_t = _n; } while (0)
static void ld_stage_report(void)
{
    if (++g_ld_stage_draws % 20000u) return;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    fprintf(stderr, "[draw-stages] us/draw:");
    static const char* names[10] = {"", "prep", "fetch", "pso", "checks", "textures", "vtex", "tables", "consts+packet", "record"};
    for (int k = 1; k < 10; k++)
        fprintf(stderr, " %s=%.2f", names[k], 1e6 * (double)g_ld_stage_qpc[k] / (double)f.QuadPart / 20000.0);
    fprintf(stderr, "\n");
    memset(g_ld_stage_qpc, 0, sizeof g_ld_stage_qpc);
}
#else
#define LD_STAGE_BEGIN() ((void)0)
#define LD_STAGE(k) ((void)0)
static void ld_stage_report(void) {}
#endif

/* How a draw lands on the ultrawide display (ld_aspect_map): NDC x' = x*ax + bx,
 * y' = y*ay. Text (per_vertex) is placed by its vertex shader instead, by
 * zones in NDC (ld_vp_aspect_epilogue): a vertex left of zones[0] goes to the
 * left edge, one right of zones[1] -- or right of zones[3] and below zones[2] --
 * to the right edge, any other stays around the centre. Its perspective
 * vertices (w != 1) get x' = x * persp_ax + persp_bx, y' = y * persp_ay
 * instead when persp_ax is not 0 (flat_3d, figures). */
typedef struct { float ax, bx, ay; int per_vertex; float zones[4]; float persp_ax, persp_ay, persp_bx;
                 float clip_top, clip_bottom;   /* canvas rows a band is cut to (0, 0: none) */
                 int open_sides; } ld_aspect_map;   /* a full-width scissor stays full width */
enum { LD_ASPECT_STRETCH, LD_ASPECT_CENTRE, LD_ASPECT_LEFT, LD_ASPECT_RIGHT, LD_ASPECT_COVER,
       LD_ASPECT_PILLAR, LD_ASPECT_TEXT, LD_ASPECT_KINDS };
static u32 g_ld_aspect_counts[LD_ASPECT_KINDS];
/* The last 2D panel placed on a gameplay frame: text drawn right after it
 * (libfont, whose position the CPU cannot follow) goes where it went. */
static int g_ld_aspect_container = LD_ASPECT_CENTRE;
/* Placement of the current run of libfont text draws, -1 between runs. */
static int g_ld_aspect_run = -1;
/* Menus, title and loading screens are 2D covering most of the canvas, built
 * from dozens of pieces that only make sense together; the gameplay HUD is a
 * few small elements. g_ld_aspect_grid marks the cells of a 32x18 grid this
 * frame's 2D pieces cover (overlaps count once; pieces spanning the width --
 * fades, overlays, movies -- not at all); a frame whose predecessor covered at
 * least half of it is a menu frame (g_ld_aspect_menu, decided at the flip).
 * g_ld_aspect_screen: the menu screen of rsx_aspect_layout.h this frame drew a
 * piece of (-1 none yet); until then the last one recognised stands in
 * (g_ld_aspect_screen_prev): a screen's first pieces often come before the
 * texture that tells it (the main menu's background and ring before its
 * title, an episode's illustration before the loading flames), and a
 * transition still drawing a screen's leftovers keeps that screen's layout
 * instead of falling to the gameplay rules or to black bars. It stands in for
 * at most 30 frames, and no longer once an unrecognised background turns up
 * (g_ld_aspect_newbg: a new screen not in the table). */
static u32 g_ld_aspect_grid[18];
static int g_ld_aspect_menu, g_ld_aspect_video;
/* This frame's draws of native geometry, and of 3D other than the menus' hexagon
 * floor (its particles): the level is on, a frame that is never a menu. */
static u32 g_ld_aspect_native, g_ld_aspect_3d;
/* LD_ASPECT_OFF_GATE=<file>: while it exists, nothing is placed (the canvas
 * keeps the game's own 16:9 layout) but pieces are still traced -- the
 * original next to the ultrawide frame (claude-tools/ultrawide/jogar_telas.py). */
static int g_ld_aspect_off;
static int g_ld_aspect_screen = -1, g_ld_aspect_screen_prev = -1;
static int g_ld_aspect_sticky, g_ld_aspect_newbg;
/* Canvas box (left, top, right, bottom) of the last 2D draw ld_aspect_place
 * measured, for the layout trace. */
static float g_ld_aspect_box[4];
static int g_ld_aspect_measured;   /* g_ld_aspect_box holds this draw's box */
/* LD_ASPECT_TRACE=1: the 2D pieces of every frame LD_FRAME_DUMP writes, one
 * line each ([aspect-piece]), to map a screen's layout (claude-tools/ultrawide). */
typedef struct { u64 tex, pso; float box[4]; u16 tw, th; u8 kind, opaque, fmt; u32 vp; } ld_aspect_piece;
static ld_aspect_piece g_ld_aspect_pieces[8192];
static u32 g_ld_aspect_npieces;
static int ld_aspect_grid_half(void)
{
    u32 cells = 0;
    /* The level and its characters are on: never a menu, whatever the combo's
     * tall flashes add up to. */
    if (g_ld_aspect_native || g_ld_aspect_3d >= 30) return 0;
    for (int r = 0; r < 18; r++) cells += (u32)__builtin_popcount(g_ld_aspect_grid[r]);
    return cells * 2u >= 32u * 18u;
}
/* The presented frame gets black side bars: a menu frame no table entry fills,
 * or the game's movie player. */
static int ld_aspect_bars(void)
{
    const int screen = g_ld_aspect_screen >= 0 ? g_ld_aspect_screen : g_ld_aspect_screen_prev;
    return !g_ld_aspect_off && (g_ld_aspect_video || (ld_aspect_grid_half() && screen < 0));
}
/* Figures (rsx_aspect_screen.figure): textures of the 3D draws placed as a
 * screen's figure this frame [0] and the last [1], the placement, the
 * horizontal extent of this frame's figure draws and the centre of the last
 * frame's, and that centre in the last frame the screen itself was up
 * (anchored): a figure going on past its screen (the cut-in sliding into the
 * level) eases from there back to the centre. */
static u64 g_ld_fig_tex[2][32];
static int g_ld_fig_place = -1, g_ld_fig_anchored;   /* place: 1 carried on past its screen, 0 not */
/* The transform of the last draw, if it was a figure's: the untextured pass
 * the game draws right after each part (its dark outline, by a program the
 * CPU cannot follow) gets the same. */
static int g_ld_fig_last;
static float g_ld_fig_ax = 1.0f, g_ld_fig_ay = 1.0f, g_ld_fig_bx;
static float g_ld_fig_lo = 1e30f, g_ld_fig_hi = -1e30f, g_ld_fig_cx = 640.0f, g_ld_fig_cx0 = 640.0f;
/* 1 = the screen's whole zoom, 0 = none; updated once per frame from how far the
 * figure has slid. It only goes down, at a limited rate: the extent follows the
 * model's limbs and sword back and forth, and the zoom must not. */
static float g_ld_fig_ease = 1.0f;
static ULONGLONG g_ld_fig_tick;
static int ld_fig_vp(int screen)
{
    if (screen < 0 || rsx_aspect_screens[screen].figure_vp[0] == 0) return 0;
    const u32 vp = (u32)g_ld_current_pso.vp_hash;
    return rsx_aspect_screens[screen].figure_vp[0] == vp || rsx_aspect_screens[screen].figure_vp[1] == vp;
}
static int ld_fig_seen(u64 tex)
{
    for (int i = 0; i < 32 && g_ld_fig_tex[1][i]; i++)
        if (g_ld_fig_tex[1][i] == tex) return 1;
    return 0;
}
static void ld_fig_note(u64 tex, float lo, float hi)
{
    if (lo < g_ld_fig_lo) g_ld_fig_lo = lo;
    if (hi > g_ld_fig_hi) g_ld_fig_hi = hi;
    if (!tex) return;
    for (int i = 0; i < 32; i++) {
        if (g_ld_fig_tex[0][i] == tex) return;
        if (!g_ld_fig_tex[0][i]) { g_ld_fig_tex[0][i] = tex; return; }
    }
}
/* LD_ASPECT_FRAMELOG=1: one line per frame -- the screen it ended as, the
 * one its first pieces were placed with, how many 2D pieces went before the
 * screen was recognised, and the menu/bars state: to find screens that come
 * up in one layout and switch to another. */
static u32 g_ld_aspect_early;
static int g_ld_aspect_first = -2;
static u64 g_ld_aspect_early_tex[6];
static u16 g_ld_aspect_early_h[6];

/* Textures covered on a tiled screen this frame [0] and the last [1]. */
static u64 g_ld_aspect_cover_tex[2][16];
static int ld_aspect_covered(u64 tex)
{
    for (int f = 0; f < 2; f++)
        for (int i = 0; i < 16 && g_ld_aspect_cover_tex[f][i]; i++)
            if (g_ld_aspect_cover_tex[f][i] == tex) return 1;
    return 0;
}
static void ld_aspect_cover_note(u64 tex)
{
    for (int i = 0; i < 16; i++) {
        if (g_ld_aspect_cover_tex[0][i] == tex) return;
        if (!g_ld_aspect_cover_tex[0][i]) { g_ld_aspect_cover_tex[0][i] = tex; return; }
    }
}
/* The gameplay HUD rule: in the top and bottom bands a piece goes by its
 * centre -- left 40% to the left edge, right 40% to the right edge -- so a long
 * life bar goes whole; across the middle only pieces hugging an edge (a
 * banner's caps), not whatever the fight puts near one. */
static int ld_aspect_hud_side(float lo, float top, float hi, float bottom, float Wv, float Hv)
{
    const float cx = (lo + hi) * 0.5f, cy = (top + bottom) * 0.5f;
    if (cy < 0.3f * Hv || cy > 0.7f * Hv)
        return cx < 0.4f * Wv ? RSX_LAYOUT_LEFT : cx > 0.6f * Wv ? RSX_LAYOUT_RIGHT : RSX_LAYOUT_CENTRE;
    return hi <= 0.25f * Wv ? RSX_LAYOUT_LEFT : lo >= 0.75f * Wv ? RSX_LAYOUT_RIGHT : RSX_LAYOUT_CENTRE;
}
/* The display canvas stays the game's 16:9 1280x720 and presentation stretches
 * it k times, so everything here is placed in canvas units: a 2D element keeps
 * its shape when squeezed by 1/k horizontally.
 *  - 3D (perspective w row) and passes reading a screen-sized image or the
 *    depth (composition, post-processing): untouched -- 3D already has the
 *    widened field of view (bleach_aspect_widen).
 *  - Menu screens in rsx_aspect_layout.h fill the display: background art
 *    zoomed uniformly to cover it, elements anchored to the edges or the
 *    centre by the screen's own zones.
 *  - Other menu frames: all their 2D stays around the centre, so the layout
 *    stays whole, and presentation paints the sides black instead of leaving
 *    whatever 3D lies behind (a loading screen over the level would show it).
 *  - Gameplay frames: HUD pieces in the top or bottom bands within the left
 *    40% go to the left edge, within the right 40% to the right edge; the rest
 *    (banners across the middle) stays around the centre. An image spanning
 *    the width (a screen overlay) is zoomed uniformly until it covers.
 *  - Untextured or render-target spans (fades, bars) fill the width; the
 *    game's movie player (frames in main memory) keeps its 16:9 picture
 *    between black bars.
 * Off during host movies: subtitles over them are composed on the 16:9 frame. */
static int ld_aspect_place(ld_aspect_map* m, const rsx_dsp_viewport* vp, float Wv, float Hv,
                           int native_geometry, int image, int opaque, int main_image, int screen_pass,
                           u64 tex)
{
    (void)opaque;
    const float k = g_ld_aspect_k, inv = 1.0f / k;
    memset(m, 0, sizeof(*m));
    m->ax = 1; m->ay = 1;
    if (k == 1.0f || g_ld_movie_mode) return LD_ASPECT_STRETCH;
    if (Wv != (float)g.width || Hv != (float)g.height) return LD_ASPECT_STRETCH;
    const int after_fig = g_ld_fig_last;
    g_ld_fig_last = 0;
    const int screen = g_ld_aspect_screen >= 0 ? g_ld_aspect_screen : g_ld_aspect_screen_prev;
    if (native_geometry) {
        /* Skinned bodies: no vertices to measure, the screen says. */
        g_ld_aspect_native++;
        if (screen >= 0 && rsx_aspect_screens[screen].flat_3d) { m->ax = inv; return LD_ASPECT_CENTRE; }
        if (screen >= 0 && rsx_aspect_screens[screen].figure == RSX_LAYOUT_COVER &&
            !rsx_aspect_screens[screen].figure_carry) {
            m->ax = k; m->ay = k;
            return LD_ASPECT_COVER;
        }
        return LD_ASPECT_STRETCH;
    }
    /* A screen with its own 3D: a draw reading the shadow map counts as a
     * screen pass, but in perspective it is a character's body. */
    const int fig_screen = screen >= 0 && !rsx_aspect_screens[screen].figure_carry &&
                           (rsx_aspect_screens[screen].flat_3d || rsx_aspect_screens[screen].figure_vp[0]);
    const int n = g_ld_current_pso.resources.position_block;
    if (n == -1) {
        if (screen_pass && !fig_screen) { g_ld_aspect_container = LD_ASPECT_CENTRE; return LD_ASPECT_STRETCH; }
        if (screen_pass) {
            /* Unknown program: the vertex shader tells them apart, 2D (the
             * composition passes, w = 1) stays and perspective is placed. */
            const float all = 1e9f;
            g_ld_aspect_container = LD_ASPECT_CENTRE;
            m->per_vertex = 1;
            m->persp_ax = rsx_aspect_screens[screen].flat_3d ? inv : k;
            m->persp_ay = rsx_aspect_screens[screen].flat_3d ? 1.0f : k;
            m->zones[0] = -all; m->zones[1] = all; m->zones[2] = -all; m->zones[3] = all;
            return LD_ASPECT_TEXT;
        }
        /* Whole draw one way (per-vertex zones would tear it): with the
         * panel before it on gameplay frames, else around the centre. */
        m->ax = inv; m->per_vertex = 1;
        if (screen >= 0 && rsx_aspect_screens[screen].flat_3d) { m->persp_ax = inv; m->persp_ay = 1.0f; }
        if (!screen_pass && after_fig) {
            /* A figure part's outline pass: as the part just drawn. */
            m->persp_ax = g_ld_fig_ax; m->persp_ay = g_ld_fig_ay; m->persp_bx = g_ld_fig_bx;
            g_ld_fig_last = 1;
        } else if (!screen_pass && ld_fig_vp(screen)) {
            /* A figure's model through a program whose position the CPU cannot
             * follow (the main menu's toon pass): as its other draws. */
            const int f = rsx_aspect_screens[screen].figure;
            m->persp_ax = f == RSX_LAYOUT_COVER ? k : 1.0f;
            m->persp_ay = f == RSX_LAYOUT_COVER ? k : 1.0f;
        }
        const float all = 1e9f;
        const int follow = screen < 0 && !g_ld_aspect_menu ? g_ld_aspect_container : LD_ASPECT_CENTRE;
        m->zones[0] = follow == LD_ASPECT_LEFT ? all : -all;
        m->zones[1] = follow == LD_ASPECT_RIGHT ? -all : all;
        m->zones[2] = -all; m->zones[3] = all;
        return LD_ASPECT_TEXT;
    }
    /* Position matrix rows x, y, w of the draw: one constant block, or for
     * libfont's text c256..c259 x the glyph's placement (ld_vp_position_block),
     * applied to (v.x, v.y, c467.z, v.w). */
    float x[4], y[4], w[4], extra[4] = {0, 0, 0, 0};
    const int text = n == -2;
    if (text) {
        float c[4][4], mo[4][4];
        for (u32 r = 0; r < 4; r++) {
            memcpy(c[r], rsx_dsp_constant(&g.rsx, 256u + r), sizeof(c[r]));
            memcpy(mo[r], rsx_dsp_constant(&g.rsx, 260u + r), sizeof(mo[r]));
        }
        memcpy(extra, rsx_dsp_constant(&g.rsx, 467u), sizeof(extra));
        mo[0][3] += extra[0]; mo[1][3] += extra[1];
        float* out[3] = {x, y, w};
        const int rows[3] = {0, 1, 3};
        for (int r = 0; r < 3; r++)
            for (int j = 0; j < 4; j++)
                out[r][j] = c[rows[r]][0]*mo[0][j] + c[rows[r]][1]*mo[1][j] + c[rows[r]][2]*mo[2][j] +
                            c[rows[r]][3]*mo[3][j];
    } else {
        memcpy(x, rsx_dsp_constant(&g.rsx, (u32)n), sizeof(x));
        memcpy(y, rsx_dsp_constant(&g.rsx, (u32)n + 1u), sizeof(y));
        memcpy(w, rsx_dsp_constant(&g.rsx, (u32)n + 3u), sizeof(w));
    }
    /* Perspective (3D) draws are measured too, for the layout trace. */
    const int persp = !text && (w[0] != 0.0f || w[1] != 0.0f || w[2] != 0.0f || w[3] != 1.0f);
    if (persp && (u32)g_ld_current_pso.vp_hash != 0xFCEE8AD3u) g_ld_aspect_3d++;
    if (screen_pass && !(persp && fig_screen)) {
        g_ld_aspect_container = LD_ASPECT_CENTRE;
        return LD_ASPECT_STRETCH;
    }
    const int mapped = vp->scale[0] != 0.0f || vp->translate[0] != 0.0f;
    float lo = 1e30f, hi = -1e30f, top = 1e30f, bottom = -1e30f;
    for (u32 i = 0; i < dc.n_verts; i++) {
        float fallback[4], q[4];
        const float* p = dc_vertex_attr(i, 0, fallback);
        if (text) { q[0] = p[0]; q[1] = p[1]; q[2] = extra[2]; q[3] = p[3]; p = q; }
        const float cw = p[0]*w[0] + p[1]*w[1] + p[2]*w[2] + p[3]*w[3];
        if (text && (cw < 0.999f || cw > 1.001f)) return LD_ASPECT_STRETCH;   /* text in 3D */
        if (persp && cw <= 1e-6f) continue;   /* behind the camera */
        const float pw = text || persp ? cw : p[3] != 0.0f ? p[3] : 1.0f;
        const float nx = (p[0]*x[0] + p[1]*x[1] + p[2]*x[2] + p[3]*x[3]) / pw;
        const float ny = (p[0]*y[0] + p[1]*y[1] + p[2]*y[2] + p[3]*y[3]) / pw;
        const float px = mapped ? nx * vp->scale[0] + vp->translate[0] : (nx * 0.5f + 0.5f) * Wv;
        const float py = mapped ? ny * vp->scale[1] + vp->translate[1] : (0.5f - ny * 0.5f) * Hv;
        if (px < lo) lo = px;
        if (px > hi) hi = px;
        if (py < top) top = py;
        if (py > bottom) bottom = py;
    }
    g_ld_aspect_box[0] = lo; g_ld_aspect_box[1] = top; g_ld_aspect_box[2] = hi; g_ld_aspect_box[3] = bottom;
    g_ld_aspect_measured = 1;
    if (persp) {
        /* 3D keeps the widened field of view, except scenery a menu screen
         * pins as background: undo the widening (x * k) and zoom with the 2D
         * background (y * k), so the two layers stay aligned. A screen whose
         * 3D is not widened squeezes it. */
        g_ld_aspect_container = LD_ASPECT_CENTRE;
        /* The aura around the screen in Ignition: a plane on the camera exactly as
         * big as the 16:9 frame, whatever the character. Its flames go around the
         * edges of the display, so it extends along the width and keeps its height. */
        if (image && (u32)g_ld_current_pso.vp_hash == RSX_VP_AURA && fabsf((hi - lo) * k - RSX_AURA_W) < 12.0f &&
            fabsf(top - RSX_AURA_TOP) < 6.0f && fabsf(bottom - RSX_AURA_BOTTOM) < 6.0f) {
            m->ax = k;
            return LD_ASPECT_COVER;
        }
        if (screen >= 0) {
            const int pinned = image ? rsx_aspect_screen_pin(screen, tex, lo, top, hi, bottom) : -1;
            if (pinned == RSX_LAYOUT_COVER) {
                m->ax = k; m->ay = k;
                return LD_ASPECT_COVER;
            }
            if (pinned < 0 && (ld_fig_vp(screen) || (screen_pass && rsx_aspect_screens[screen].figure_vp[0]))) {
                /* A figure: placed like its 2D render, and remembered so the
                 * same model keeps the placement once the screen is gone. */
                g_ld_fig_place = rsx_aspect_screens[screen].figure_carry;
                g_ld_fig_anchored = 1;
                g_ld_fig_last = 1;
                ld_fig_note(image ? tex : 0, lo, hi);
                m->ax = k; m->ay = k;
                g_ld_fig_ax = k; g_ld_fig_ay = k; g_ld_fig_bx = 0.0f;
                return LD_ASPECT_COVER;
            }
            if (rsx_aspect_screens[screen].flat_3d) { m->ax = inv; return LD_ASPECT_CENTRE; }
        } else if (image && g_ld_fig_place == 1 && ld_fig_seen(tex)) {
            /* The figure carried on into the level: its zoom eases out as it
             * slides back to the centre (g_ld_fig_ease, one zoom for all its parts). */
            ld_fig_note(tex, lo, hi);
            m->ax = 1.0f + g_ld_fig_ease * (k - 1.0f); m->ay = m->ax;
            g_ld_fig_last = 1;
            g_ld_fig_ax = m->ax; g_ld_fig_ay = m->ay; g_ld_fig_bx = 0.0f;
            return LD_ASPECT_COVER;
        }
        return LD_ASPECT_STRETCH;
    }
    if (text) {
        /* A run of text draws (a name, a line, a paragraph: one glyph each)
         * keeps the placement of its first glyph, so it is never split
         * between zones; any other 2D piece ends the run. */
        int kind = LD_ASPECT_CENTRE;
        if (screen >= 0 || !g_ld_aspect_menu) {
            /* Gameplay and screens over it (pause): the HUD rule, unless the
             * screen pins a text region; menu screens: their text zones. */
            const int s = screen >= 0 ? screen : RSX_SCREEN_GAMEPLAY;
            if (g_ld_aspect_run < 0) {
                int p;
                if (rsx_aspect_screens[s].hud) {
                    p = rsx_aspect_screen_pin(s, RSX_TEX_TEXT, lo, top, hi, bottom);
                    if (p < 0) p = ld_aspect_hud_side(lo, top, hi, bottom, Wv, Hv);
                } else {
                    p = rsx_aspect_screen_pin(s, RSX_TEX_TEXT, lo, top, hi, bottom);
                    if (p < 0) p = rsx_aspect_screen_text(s, (lo + hi) * 0.5f, (top + bottom) * 0.5f);
                }
                g_ld_aspect_run = p == RSX_LAYOUT_LEFT ? LD_ASPECT_LEFT
                                : p == RSX_LAYOUT_RIGHT ? LD_ASPECT_RIGHT : LD_ASPECT_CENTRE;
            }
            kind = g_ld_aspect_run;
        }
        m->ax = inv;
        m->bx = kind == LD_ASPECT_LEFT ? inv - 1.0f : kind == LD_ASPECT_RIGHT ? 1.0f - inv : 0.0f;
        return kind;
    }
    g_ld_aspect_run = -1;
    const int spanning = lo <= 0.5f && hi >= Wv - 0.5f;
    /* Only big pieces make a menu: backgrounds, panels; not HUD gauges, even
     * when dozens of them slide in. */
    if (!spanning && image && hi - lo >= Wv / 8 && bottom - top >= Hv / 8 &&
        hi > 0 && lo < Wv && bottom > 0 && top < Hv) {
        const int c0 = lo <= 0 ? 0 : (int)(lo / Wv * 32), c1 = hi >= Wv ? 31 : (int)(hi / Wv * 32);
        const int r0 = top <= 0 ? 0 : (int)(top / Hv * 18), r1 = bottom >= Hv ? 17 : (int)(bottom / Hv * 18);
        for (int r = r0; r <= r1 && r < 18; r++)
            for (int c = c0; c <= c1 && c < 32; c++) g_ld_aspect_grid[r] |= 1u << c;
    }
    const int found = rsx_aspect_screen_of(tex, bottom - top);
    if (g_ld_aspect_first == -2) g_ld_aspect_first = screen;
    if (image && found < 0 && g_ld_aspect_screen < 0) {
        if (bottom - top >= 600.0f && (screen < 0 || !rsx_aspect_screen_has(screen, tex))) g_ld_aspect_newbg = 1;
        int i = 0;
        while (i < 6 && g_ld_aspect_early_tex[i] && g_ld_aspect_early_tex[i] != tex) i++;
        if (i < 6 && !g_ld_aspect_early_tex[i]) { g_ld_aspect_early_tex[i] = tex; g_ld_aspect_early_h[i] = (u16)(bottom - top); }
        g_ld_aspect_early++;
    }
    if (found >= 0) g_ld_aspect_screen = found;
    const int now = found >= 0 ? found : screen;
    if (image && main_image && spanning) {
        g_ld_aspect_container = LD_ASPECT_CENTRE;
        g_ld_aspect_video = 1;
        m->ax = inv;
        return LD_ASPECT_PILLAR;
    }
    if (spanning && !image) { g_ld_aspect_container = LD_ASPECT_CENTRE; return LD_ASPECT_STRETCH; }
    /* A strip across the whole width (a description bar, a dialog band) is
     * uniform along it: it just extends to the display's width, where it
     * keeps its height and place; zooming it like a background would move it
     * off the text drawn on it. */
    const int strip = spanning && bottom - top <= 0.25f * Hv;
    int kind;
    const int hud = now >= 0 ? rsx_aspect_screens[now].hud : !g_ld_aspect_menu;
    if (hud) {
        /* Gameplay, or a screen over it: pinned pieces first, then the HUD
         * rules below. */
        const int pinned = image && (u32)g_ld_current_pso.vp_hash != RSX_VP_MINIMAP
            ? rsx_aspect_screen_pin(now >= 0 ? now : RSX_SCREEN_GAMEPLAY, tex, lo, top, hi, bottom) : -1;
        /* The level's map behind the minimap, as big as the level and scrolling
         * with the player: always around the minimap's centre, whatever its box. */
        const int minimap_map = image && (u32)g_ld_current_pso.vp_hash == RSX_VP_MINIMAP && hi - lo >= 256.0f &&
                                lo < RSX_MINIMAP_X && hi > RSX_MINIMAP_X && top < RSX_MINIMAP_Y && bottom > RSX_MINIMAP_Y;
        if (minimap_map) {
            kind = LD_ASPECT_RIGHT;
        } else if (pinned >= 0 && (!spanning || pinned == RSX_LAYOUT_COVER)) {
            kind = pinned == RSX_LAYOUT_COVER ? LD_ASPECT_COVER : pinned == RSX_LAYOUT_LEFT ? LD_ASPECT_LEFT
                 : pinned == RSX_LAYOUT_RIGHT ? LD_ASPECT_RIGHT : LD_ASPECT_CENTRE;
        } else if (spanning) {
            g_ld_aspect_container = LD_ASPECT_CENTRE;
            if (strip) return LD_ASPECT_STRETCH;
            kind = LD_ASPECT_COVER;
        } else if (hi - lo >= 0.5f * Wv) {
            /* A wide band between two edge pieces (the objective banner: caps
             * at both edges, a uniform band between): each end keeps its
             * distance to its own edge, the band stretches between them. In
             * canvas units the display is Wv * k wide. */
            const float x0 = lo / k, x1 = (Wv * k - Wv + hi) / k, a = (x1 - x0) / (hi - lo);
            m->ax = a; m->bx = a + (x0 - a * lo) / (Wv * 0.5f) - 1.0f;
            g_ld_aspect_container = LD_ASPECT_CENTRE;
            return LD_ASPECT_CENTRE;
        } else {
            const int p = ld_aspect_hud_side(lo, top, hi, bottom, Wv, Hv);
            kind = p == RSX_LAYOUT_LEFT ? LD_ASPECT_LEFT : p == RSX_LAYOUT_RIGHT ? LD_ASPECT_RIGHT : LD_ASPECT_CENTRE;
        }
    } else if (now >= 0) {
        int p = rsx_aspect_screen_piece(now, image ? tex : 0, lo, top, hi, bottom, strip);
        if (p == RSX_LAYOUT_STRETCH) {
            g_ld_aspect_container = LD_ASPECT_CENTRE;
            return LD_ASPECT_STRETCH;
        }
        if (image && (rsx_aspect_screens[now].cover_h > 0 || rsx_aspect_screens[now].compose)) {
            /* A picture drawn as tiles (an episode illustration, a character's
             * art sliding in) goes whole: once one tile is tall enough to
             * cover, every tile of its texture does, this frame and the next
             * (a short one sliding in too). Only tiles count, not overlays
             * across the width: their atlas also draws strips and chevrons. */
            if (p == RSX_LAYOUT_CENTRE && !spanning && ld_aspect_covered(tex)) p = RSX_LAYOUT_COVER;
            if (p == RSX_LAYOUT_COVER && !spanning) ld_aspect_cover_note(tex);
        }
        /* Spanning the width (a flash drawn from a logo atlas's white texel)
         * never goes to one edge, whatever its texture's pin says. */
        if (spanning && p != RSX_LAYOUT_COVER) {
            g_ld_aspect_container = LD_ASPECT_CENTRE;
            if (strip) return LD_ASPECT_STRETCH;
            m->ay = k;
            return LD_ASPECT_COVER;
        }
        if (p == RSX_LAYOUT_BAND) {
            g_ld_aspect_container = LD_ASPECT_CENTRE;
            m->ay = k; m->clip_top = top; m->clip_bottom = bottom;
            return LD_ASPECT_COVER;
        }
        kind = p == RSX_LAYOUT_COVER ? LD_ASPECT_COVER : p == RSX_LAYOUT_LEFT ? LD_ASPECT_LEFT
             : p == RSX_LAYOUT_RIGHT ? LD_ASPECT_RIGHT : LD_ASPECT_CENTRE;
    } else {
        kind = LD_ASPECT_CENTRE;   /* a menu not in the table: whole, in the centre */
    }
    switch (kind) {
    case LD_ASPECT_COVER: m->ay = k; break;
    case LD_ASPECT_LEFT: m->ax = inv; m->bx = inv - 1.0f; break;
    case LD_ASPECT_RIGHT: m->ax = inv; m->bx = 1.0f - inv; break;
    default: m->ax = inv; m->open_sides = now >= 0 && rsx_aspect_screens[now].open_sides; break;
    }
    g_ld_aspect_container = kind == LD_ASPECT_COVER ? LD_ASPECT_CENTRE : kind;
    return kind;
}

static void sink_end_geometry_impl(void* user, const rsx_dispatch* r,const rsx_geometry_lease* geometry)
{
    (void)user; (void)r;
    /* Shadow integration: prove identity at the real draw boundary before
     * enabling GPU selection. The original CPU result is still rendered. */
    int native_geometry=0;
    if(geometry->token && !geometry->deferred) {
        u32 indices=0;int contiguous=!dc.n_arr && !dc.inl;
        for(u32 i=0;i<dc.n_idx;i++) {
            if(dc.idx[i].first!=indices)contiguous=0;
            indices+=dc.idx[i].count;
        }
        rsx_geometry_handoff_shadow_check(geometry,g.rsx.current_primitive,indices,contiguous);
    }
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) return;
    if (!dc.n_packets) { g_ld_stats.groups_empty++; return; }
    LD_STAGE_BEGIN();
    g_ld_stats.groups_seen++;
    /*
     * a010's native per-mesh constant builder can overwrite the otherwise
     * valid reconstructed camera with NaNs after the ordinary camera upload.
     * Repair at the last measured boundary, immediately before the draw
     * consumes the constants. Once the synchronized a010 reference is armed,
     * replace finite-but-wrong native uploads too: the measured failure
     * matrix is finite, yet projects the model thousands of pixels offscreen.
     */
    if (InterlockedCompareExchange(&g_ld_a010_camera_ready, 0, 0) != 0) {
        int camera_has_nan = 0;
        for (u32 slot = 108; slot <= 111 && !camera_has_nan; slot++)
            for (u32 lane = 0; lane < 4; lane++) {
                const u32 bits = g.rsx.constants[slot][lane];
                if ((bits & 0x7F800000u) == 0x7F800000u &&
                    (bits & 0x007FFFFFu) != 0) {
                    camera_has_nan = 1;
                    break;
                }
            }
        if (camera_has_nan || g_yz_runtime_config.a010_start_reference ||
            InterlockedCompareExchange(
                &g_yz_a010_reference_camera_active, 0, 0) != 0) {
            for (u32 i = 0; i < 16; i++)
                g.rsx.constants[108 + i / 4][i % 4] =
                    g_ld_a010_camera_bits[i];
            static u64 repairs = 0;
            repairs++;
            if (repairs <= 16 || (repairs & (repairs - 1)) == 0) {
                fprintf(stderr,
                        "[a010-rsx-camera-repair] n=%llu frame=%u "
                        "c108=%08X/%08X/%08X/%08X\n",
                        (unsigned long long)repairs, g_ld_frames,
                        g.rsx.constants[108][0], g.rsx.constants[108][1],
                        g.rsx.constants[108][2], g.rsx.constants[108][3]);
                fflush(stderr);
            }
        }
    }
    /* A polygon is a convex fan around its first vertex -- what the register-
     * file engine (rsx_draw_engine.c) and the null backend already make of one.
     * Left unmapped it was dropped outright: Twisted Metal's legal screens
     * submit 16-vertex polygons. */
    const u32 prim = g.rsx.current_primitive == 10u /* POLYGON */
        ? PRIM_TRIANGLE_FAN : g.rsx.current_primitive;
    rsx_vertex_layout_plan used_layout;
    ld_current_vertex_layout(&used_layout);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.used_attribute_draws++;
    g_ld_profile.total.used_attribute_sum += used_layout.count;
    if (used_layout.count > g_ld_profile.frame_used_attribute_max)
        g_ld_profile.frame_used_attribute_max = used_layout.count;
    if (used_layout.count > g_ld_profile.total_used_attribute_max)
        g_ld_profile.total_used_attribute_max = used_layout.count;
    LD_STAGE(1);
    const LONGLONG fetch_pack_begin = ld_profile_qpc();
#endif
    if(geometry->deferred) {
        u32 n=0;int compatible=prim==5 && !dc.n_arr && !dc.inl &&
            !rsx_dsp_vertex_data_base_index(&g.rsx) && ld_vertex_compact_payload() && ld_vertex_mask_signature() && ld_vertex_pull();
        for(u32 i=0;i<dc.n_idx;i++){if(dc.idx[i].first!=n)compatible=0;n+=dc.idx[i].count;}
        if(n!=geometry->input_indices)compatible=0;
        if(compatible && geometry_gpu_init(g.dev,g.frame)) {
            dc.layout=used_layout;
            rsx_vertex_fetch_plan_init(&dc.fetch_plan,&g.rsx,&used_layout,(rsx_vertex_guest_ptr_fn)g.guest_ptr,g.guest_user);
            rsx_vertex_ref endpoints[2]={{0,0},{geometry->vertices-1,0}};
            rsx_vertex_fetch_plan_prepare(&dc.fetch_plan,endpoints,2);
            if(rsx_vertex_pull_prepare_linear(&dc.pull_plan,&dc.fetch_plan,geometry->vertices,VERT_BUFFER_SIZE)) {
                native_geometry=1;dc.native_pull=1;dc.n_verts=geometry->vertices;dc.n_source_refs=geometry->input_indices;
            }
        }
        if(!native_geometry)geometry_cpu_recover(geometry);
    }
    if(native_geometry) { /* owned original vertex order, no CPU index expansion */
    } else if (ld_vertex_compact_payload()) {
        fetch_batches_hoisted(&used_layout, 1);
    } else if (ld_vertex_hoist_fetch()) {
        rsx_vertex_layout_plan legacy_layout;
        ld_layout_plan_get(0xFFFFu, &legacy_layout);
        fetch_batches_hoisted(&legacy_layout, 0);
    } else {
        fetch_batches();
    }
    if (!dc.n_source_refs)
        dc.n_source_refs = dc.n_verts;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.vertex_fetch_pack_qpc +=
        (u64)(ld_profile_qpc() - fetch_pack_begin);
#endif
    if (!dc.n_verts || !dc.fetch_ok) { g_ld_stats.group_drop_fetch++; return; }

    /* Preserve validated occurrence order while allowing repeated compact
     * vertex references to share one fetched and uploaded payload. */
    int indexed = 0;
    if (!rsx_vertex_topology_plan(prim, dc.refs_remapped, &indexed)) {
        g_ld_stats.group_drop_primitive++;
        if (getenv("LD_PRIMITIVE_DIAG")) {
            static unsigned seen[32];
            if (prim < 32 && seen[prim]++ < 3)
                fprintf(stderr, "[primitive-missing] type=%u vertices=%u surface=%u frame=%u\n",
                        prim, dc.n_source_refs, current_surface(), g_ld_frames);
        }
        return;
    }
    u32 n_tri = indexed
        ? topology_index_count(prim)
        : dc.n_source_refs - dc.n_source_refs % 3;
    if(native_geometry){indexed=1;n_tri=geometry->input_indices;}

    if (!n_tri) {
        g_ld_stats.group_drop_degenerate++;
        return;
    }

    if (prim == RSX_PRIMITIVE_QUAD_STRIP && getenv("LD_PRIMITIVE_DIAG")) {
        static unsigned reported;
        if (reported++ < 3)
            fprintf(stderr, "[primitive-expanded] type=%u vertices=%u indices=%u cuts=%u surface=%u frame=%u\n",
                    prim, dc.n_source_refs, n_tri, dc.n_cuts, current_surface(), g_ld_frames);
    }

    live_a010_geom_trace(prim, n_tri);

    const u32 vertex_stride =
        ld_vertex_compact_payload() ? used_layout.stride : VERT_STRIDE;
    const u32 uploaded_vertices = indexed ? dc.n_verts : n_tri;
    const void* vertex_data = ld_vertex_compact_payload()
        ? (const void*)dc.compact_verts : (const void*)dc.verts;
    const u64 draw_vb_bytes = dc.native_pull ? dc.pull_plan.bytes : (u64)uploaded_vertices * vertex_stride;
    const u64 draw_ib_bytes = indexed && !native_geometry ? (u64)n_tri * sizeof(u32) : 0;
    const u64 vb_capacity = VERT_BUFFER_SIZE;
    if (draw_vb_bytes > vb_capacity || draw_ib_bytes > INDEX_BUFFER_SIZE) {
        live_draw_csv_emit(prim, n_tri, "drop_vbring_oversize");
        g_ld_stats.group_drop_ring++;
        return;
    }
    if ((u64)g.frame->vb_used + draw_vb_bytes > vb_capacity ||
        (u64)g.frame->ib_used + draw_ib_bytes > INDEX_BUFFER_SIZE) {
        /*
         * The replay-proven renderer recycles this upload ring mid-frame.
         * Live used to drop every remaining draw instead, which discarded
         * 181/184 sampled a010 groups after the orphanage workload filled the
         * 256K-vertex ring.  We are still before this draw's memcpy, so it is
         * safe to submit/wait and reuse every transient draw ring here.
         */
        ld_flush(LD_FLUSH_VERTEX_RING);
        g.frame->vb_used = 0;
        g.frame->ib_used = 0;
        g.frame->cb_used = 0;
        g.frame->ps_cb_used = 0;
        g.frame->srv_ring_used = 0;
        g.frame->smp_ring_used = 0;
    }
    if(dc.native_pull) {
        if(!rsx_vertex_pull_write(&dc.pull_plan,g.frame->vb_mapped+g.frame->vb_used,
            (u32)(vb_capacity-g.frame->vb_used))) {g_ld_stats.group_drop_fetch++;return;}
    } else if (draw_vb_bytes)
        memcpy(
            g.frame->vb_mapped + g.frame->vb_used, vertex_data,
            (size_t)draw_vb_bytes);
    if (indexed && !native_geometry) {
        u32* topology = (u32*)((u8*)g.frame->ib_mapped + g.frame->ib_used);
        write_topology_indices(prim, topology);
        if (ld_flat_shading()) ld_flat_provoking_first(prim, topology, n_tri);
    }

#if defined(YZ_PERF_PROFILE)
    LD_STAGE(2);
    const u64 pso_begin = ld_profile_qpc();
#endif
    ID3D12PipelineState* pso = get_pso(
        ld_vertex_mask_signature() ? &used_layout : NULL,
        dc.native_pull ? 2 : ld_vertex_compact_payload());
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.pso_total_qpc += ld_profile_qpc() - pso_begin;
#endif
    if (!pso) {
#if defined(YZ_PERF_PROFILE)
        ld_profile_note_rejected_pso();
#endif
        live_draw_csv_emit(prim, n_tri, "drop_pso");
        g_ld_stats.group_drop_pso++;
        return;   /* no fallback in live path */
    }
    u32 vertex_cb_offset = 0;
    int vertex_cb_plan = rsx_vertex_constant_ring_plan(
        g.frame->cb_used, CB_RING_BYTES, CB_BLOCK_ALIGNED, &vertex_cb_offset);
    if (vertex_cb_plan == 0) {
#if !defined(YZ_PERF_CLEAN)
        g_ld_vertex_constant_ring_recycles++;
        g_ld_frame_vertex_constant_ring_recycles++;
        yz_frontier_trace_emit(
            YZ_FT_PARITY_CB_RECYCLE, g_ld_frames, g_ld_last_present_target,
            g.frame->cb_used, CB_RING_BYTES, CB_BLOCK_ALIGNED,
            (u32)g_ld_stats.groups_executed,
            g_ld_fifo_get, g_ld_fifo_put);
#endif
        ld_flush(LD_FLUSH_VERTEX_CONSTANT_RING);
        if (!g.ready)
            return;
        g.frame->cb_used = 0;
        vertex_cb_plan = rsx_vertex_constant_ring_plan(
            g.frame->cb_used, CB_RING_BYTES, CB_BLOCK_ALIGNED,
            &vertex_cb_offset);
    }
    if (vertex_cb_plan != 1) {
        live_draw_csv_emit(prim, n_tri, "drop_cbring_invalid");
        g_ld_stats.group_drop_ring++;
        return;
    }
    g.frame->cb_used = vertex_cb_offset;
    u32 ps_cb_offset = 0;
    { static u64 watched = 1; static unsigned hits;
      if (watched == 1) { const char* e = getenv("LD_FP_PAYLOAD"); watched = e ? _strtoui64(e, 0, 16) : 0; }
      if (watched && hits < 3 && live_legacy_pso_key() == watched) {
          ++hits;
          fprintf(stderr, "[fp-payload] frame=%u constants=%u\n", g_ld_frames, g.fp_constants.count);
          for (u32 i=0;i<g.fp_constants.count;i++) fprintf(stderr,"[fp-payload] c%u = %g %g %g %g\n",i,
              ld_c2f(g.fp_constants.values[i][0]),ld_c2f(g.fp_constants.values[i][1]),
              ld_c2f(g.fp_constants.values[i][2]),ld_c2f(g.fp_constants.values[i][3]));
      } }
    LD_STAGE(3);
    if (!ld_upload_pixel_constants(&ps_cb_offset)) {
        live_draw_csv_emit(prim, n_tri, "drop_ps_cbring");
        g_ld_stats.group_drop_ring++;
        return;
    }

    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) {
        live_draw_csv_emit(prim, n_tri, "drop_surface");
        g_ld_stats.group_drop_surface++;
        return;
    }
    /* LD_SKIP_SAMPLING=<hex offset>: drop draws that sample a texture at that
     * offset. Debug only -- removing a post-process pass shows whether the
     * scene under it has any colour at all. */
    { static int64_t skip = -2;
      if (skip == -2) { const char* e = getenv("LD_SKIP_SAMPLING"); skip = e ? (int64_t)strtoul(e, 0, 16) : -1; }
      if (skip >= 0)
          for (u32 u = 0; u < 16; u++) {
              rsx_dsp_texture st; rsx_dsp_get_texture(&g.rsx, u, &st);
              if (st.enabled && st.offset == (u32)skip) return;
          } }
    /* LD_TRACE_PSO=<hex key>: raw vertex attributes + unit-0 texels of the
     * first three draws with that CSV pso_key. */
    { static u64 tk = 1; static int tn;
      if (tk == 1) { const char* e = getenv("LD_TRACE_PSO"); tk = e ? _strtoui64(e, 0, 16) : 0; }
      if (tk && live_legacy_pso_key() == tk && tn >= 3) {
          static u32 every;
          if (dc.inl && (++every % 60) == 0)
              fprintf(stderr, "[pso-trace] frame=%u col=%02X%02X%02X%02X\n", g_ld_frames,
                      dc.inl[16], dc.inl[17], dc.inl[18], dc.inl[19]);
      }
      /* LD_TRACE_PSO_FROM=<frame>: the first three draws at or after that
       * frame -- a PSO's first uses can predate the data it later draws. */
      static long tfrom = -1;
      if (tfrom < 0) { const char* e = getenv("LD_TRACE_PSO_FROM"); tfrom = e ? atol(e) : 0; }
      if (tk && tn < 3 && live_legacy_pso_key() == tk && (long)g_ld_frames >= tfrom) {
          tn++;
          if (dc.inl) {
              fprintf(stderr, "[pso-trace] inline stride=%u bytes=%u:", dc.inl_stride, dc.inl_bytes);
              for (u32 k = 0; k < dc.inl_bytes && k < 6 * dc.inl_stride; k++)
                  fprintf(stderr, "%s%02X", (k % dc.inl_stride) ? "" : " |", dc.inl[k]);
              fprintf(stderr, "\n");
          }
          for (u32 ai = 0; ai < 16; ai++) {
              rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, ai, &va);
              if (!va.type) continue;
              fprintf(stderr, "[pso-trace] attr%u type=%u size=%u stride=%u loc=%u off=0x%X:",
                      ai, va.type, va.size, va.stride, va.location, va.offset);
              for (u32 vi = 0; vi < 6; vi++) {
                  const u8* p = guest_ptr(va.location, va.offset + vi * va.stride, 16);
                  if (!p) break;
                  fprintf(stderr, " |");
                  for (u32 k = 0; k < (va.type == RSX_VTX_TYPE_FLOAT ? 4u * va.size : 4u) && k < 16; k++)
                      fprintf(stderr, "%02X", p[k]);
              }
              fprintf(stderr, "\n");
          }
          /* The raw array registers and base (the attrN lines above print the
           * decoded offset only), and the constant colour a disabled attr3
           * feeds -- the two things a draw that vanishes is usually missing. */
          fprintf(stderr, "[pso-trace] vdata base_offset=0x%X base_index=%u raw attr0 off=0x%08X fmt=0x%08X\n",
                  rsx_dsp_vertex_data_base_offset(&g.rsx), rsx_dsp_vertex_data_base_index(&g.rsx),
                  rsx_dsp_reg(&g.rsx, 0x1680), rsx_dsp_reg(&g.rsx, 0x1740));
          { float d3[4]; rsx_dsp_vertex_default(&g.rsx, 3, d3);
            fprintf(stderr, "[pso-trace] attr3 const=%g %g %g %g\n", d3[0], d3[1], d3[2], d3[3]); }
          /* Every non-zero transform constant (raw words), so the VS can be
           * evaluated offline against exactly what this draw saw. */
          for (u32 q = 0; q < 512; q++) {
              const u32* w = g.rsx.constants[q];
              if (!(w[0] | w[1] | w[2] | w[3])) continue;
              fprintf(stderr, "[pso-const] %u %08X %08X %08X %08X\n", q, w[0], w[1], w[2], w[3]);
          }
          for (u32 ai = 0; ai < 16; ai++) {   /* the constant value each attribute falls back to */
              float d[4]; rsx_dsp_vertex_default(&g.rsx, ai, d);
              fprintf(stderr, "[pso-vdef] %u %g %g %g %g\n", ai, d[0], d[1], d[2], d[3]);
          }
          { u32 fl = 0; const u32 fo = rsx_dsp_fragment_program(&g.rsx, &fl);
            const u8* fp = guest_ptr(fl, fo, 16 * 16);
            if (fp) { fprintf(stderr, "[pso-trace] fp %u:0x%X:", fl, fo);
                      for (u32 k = 0; k < 16 * 16; k++) fprintf(stderr, "%s%02X", (k & 15) ? "" : " ", fp[k]);
                      fprintf(stderr, "\n"); } }
          rsx_dsp_texture t0; rsx_dsp_get_texture(&g.rsx, 0, &t0);
          const u8* tp = guest_ptr(t0.location, t0.offset, 32);
          if (tp) { { rsx_dsp_texture t1; rsx_dsp_get_texture(&g.rsx, 1, &t1); fprintf(stderr, "[pso-trace] outmask=0x%X inmask=0x%X tex0 fmt=0x%X remap=0x%X wrap=0x%X border=0x%08X filter=0x%X tex1 remap=0x%X wrap=0x%X:", rsx_dsp_reg(&g.rsx, 0x1FF4), rsx_dsp_reg(&g.rsx, 0x1FF0), t0.format, t0.remap, t0.wrap, rsx_dsp_reg(&g.rsx, 0x1A1C), t0.filter, t1.remap, t1.wrap); }
                    for (u32 k = 0; k < 32; k++) fprintf(stderr, "%02X", tp[k]);
                    fprintf(stderr, "\n"); }
      } }
    /* LD_FORCE_VA=1: debug -- inline draws whose UB4 vertex colour (attr 3)
     * has alpha 0 everywhere get alpha 255, to see what a faded-out element is. */
    { static int fva = -1; if (fva < 0) fva = getenv("LD_FORCE_VA") ? 1 : 0;
      if (fva && dc.inl && dc.inl_stride) {
          rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, 3, &va);
          if (va.type == RSX_VTX_TYPE_UNORM8 && va.size == 4) {
              u32 amax = 0;
              for (u32 o = dc.inl_off[3] + 3; o < dc.inl_bytes; o += dc.inl_stride)
                  if (dc.inl[o] > amax) amax = dc.inl[o];
              if (!amax)
                  for (u32 o = dc.inl_off[3] + 3; o < dc.inl_bytes; o += dc.inl_stride)
                      ((u8*)dc.inl)[o] = 255;
          } } }
    /* LD_SKIP_PSO=<hex key>: drop draws with that CSV pso_key. Debug only. */
    { static u64 skipk = 1;
      if (skipk == 1) { const char* e = getenv("LD_SKIP_PSO"); skipk = e ? _strtoui64(e, 0, 16) : 0; }
      if (skipk && live_legacy_pso_key() == skipk) return; }
    /* MRT: colour targets B..D, resolved (and created) here, before any of
     * this draw's commands are recorded. They share A's format and clip. */
    u32 mrt[4] = { target, 0, 0, 0 };
    u32 n_mrt = 1;
    {
        rsx_dsp_surface msf;
        rsx_dsp_get_surface(&g.rsx, &msf);
        const u32 want = ld_rt_count(msf.color_target);
        for (u32 r = 1; r < want; r++) {
            const u32 s = surface_get(msf.color_location[r], msf.color_offset[r],
                                      msf.clip_w, msf.clip_h,
                                      msf.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16
                                          ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                          : DXGI_FORMAT_R8G8B8A8_UNORM);
            if (s == LD_INVALID_SURFACE || s == target) break;
            mrt[n_mrt++] = s;
        }
    }
    LD_STAGE(4);
    live_draw_csv_emit(prim, n_tri, "execute");
    if (rsx_live_draw_a010_probe_active() && target < 64)
        g_ld_a010_probe_touched |= 1ull << target;
    { static u32 last_target = LD_INVALID_SURFACE;
      if (target != last_target) { ld_trace_target("draw", target, 0); last_target = target; } }
    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    const u32 current_zslot = g.depth
        ? zdepth_get(sf.zeta_location, sf.zeta_offset, sf.clip_w, sf.clip_h,
                     sf.zeta_pitch, sf.depth_format, sf.raster_type)
        : 0;
    u32 slots[SRV_TABLE_SIZE], smp_slots[SMP_TABLE_SIZE];
#if defined(YZ_PERF_PROFILE)
    const u64 texture_resolve_begin=ld_profile_qpc();
#endif
    u32 surf_used[SRV_TABLE_SIZE], n_surf_used = 0;
    /* CPU cache descriptors selected earlier in this draw stay immutable
     * until all of them have been copied to its shader-visible table. */
    ++g_surface_views.draw_serial;
    u32 zdepth_used[SRV_TABLE_SIZE], n_zdepth_used = 0;
    u32 texture_mask = 0;
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++) slots[u] = SRV_WHITE;
    for (u32 u = 0; u < SMP_TABLE_SIZE; u++) smp_slots[u] = SMP_DEFAULT;
    /* A draw that samples its own zeta while not writing depth (deferred
     * lighting reading the scene depth it also depth-tests against, soft
     * particles) reads the snapshot: the zeta does not change during such a
     * draw, so that is what RSX returns. Before, it fell back to guest VRAM,
     * which the host-side depth never reaches (depth 0). A draw that writes
     * the zeta it reads keeps that fallback. */
    const int self_zeta_read = current_zslot &&
        !((rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1u) &&
          (rsx_dsp_reg(&g.rsx, M_DEPTH_WRITE) & 1u));
    /* LD_UNITS=1: every enabled texture unit of every draw that samples PS1
     * VRAM, once per distinct combination.
     *
     * ps1_netemu's display composite is a Cg program picked from seven
     * (CG_fp_gradient / orientation / mofix / smart / upscale / upscale_smart /
     * sharpen) and its uniforms are texture0, texture1 and hwidth -- TWO
     * textures. Every probe here so far has looked at unit 0 only, so "the
     * renderer reads the right bytes" was never actually tested for the second
     * one. This lists them. */
    { static int lu = -1; static u32 seen[8]; static int ns;
      if (lu < 0) lu = getenv("LD_UNITS") ? 1 : 0;
      if (lu) {
          u32 key = 0; int ps1 = 0;
          for (u32 q = 0; q < SRV_TABLE_SIZE; q++) {
              rsx_dsp_texture tq; rsx_dsp_get_texture(&g.rsx, q, &tq);
              if (!tq.enabled) continue;
              key = key * 31u + (tq.offset ^ (tq.format << 8) ^ (q << 24));
              if (tq.location == 1u && tq.offset >= 0x400000u &&
                  tq.offset < 0x480000u) ps1 = 1;
          }
          if (ps1) {
              int fresh = 1;
              for (int z = 0; z < ns; z++) if (seen[z] == key) fresh = 0;
              if (fresh && ns < 8) {
                  seen[ns++] = key;
                  fprintf(stderr, "[units] draw sampling PS1 VRAM:");
                  for (u32 q = 0; q < SRV_TABLE_SIZE; q++) {
                      rsx_dsp_texture tq; rsx_dsp_get_texture(&g.rsx, q, &tq);
                      if (!tq.enabled) continue;
                      fprintf(stderr, " u%u[%u:0x%08X fmt=0x%02X %ux%u p=%u]",
                              q, tq.location, tq.offset, tq.format,
                              tq.width, tq.height, tq.pitch);
                  }
                  fprintf(stderr, "\n");
              }
          }
      } }
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++) {
        rsx_dsp_texture t = g_ld_draw_textures[u];
        /* LD_PS1_BUF0=1 -- DIAGNOSTIC, not a fix. Force a 24-bit PS1 VRAM
         * bind at buffer 1 (offset 0x4003C0) to sample buffer 0 (0x400000).
         *
         * After the intro FMV both the driven and undriven paths end on a blank
         * screen, and during it every bind is 0x4003C0 while the content that
         * exists -- the decoded FMV frame -- sits in buffer 0. That is the shape
         * of a display flip parked on the buffer the game is not drawing into.
         * If forcing buffer 0 puts a picture back on screen the flip is the bug;
         * if the screen stays blank it is not. Nothing else distinguishes those
         * two, and the override is one line. */
        { static int fb0 = -1;
          if (fb0 < 0) fb0 = getenv("LD_PS1_BUF0") ? 1 : 0;
          if (fb0 && t.location == 1u && t.offset == 0x4003C0u)
              t.offset = 0x400000u; }
        if (!t.enabled) continue;
        if (!(g_ld_current_pso.resources.fragment_textures & (1u << u))) {
#if defined(YZ_PERF_PROFILE)
            g_ld_profile.total.texture_unused_bindings++;
#endif
            if (ld_shader_resources_enabled()) continue;
        }
#if defined(YZ_PERF_PROFILE)
        g_ld_profile.total.texture_dependencies++;
#endif
        texture_mask |= 1u << u;
        smp_slots[u] = sampler_slot(&t);
        /* Pick WHICH surface at this offset to sample.
         *
         * Matching on (location, offset) alone was fine when only one surface
         * could exist per offset. Now that surfaces are keyed on size too --
         * they have to be, or the two passes destroy each other's contents --
         * several can, and taking the first match by offset takes an arbitrary
         * one. ps1_netemu composites the PS1 framebuffer into a 720x512 surface
         * at offset 0x0 and then upscales into a 1280x720 output; the upscale
         * pass samples offset 0x0 and was getting the EMPTY 1280x720 surface
         * that happens to sit earlier in the table.
         *
         * Prefer the surface whose dimensions match what the texture
         * descriptor actually declares; failing that, the most recently drawn
         * one, which is the only other defensible answer. */
        int packed_slot = -1;
        for (u32 zi = 0; zi < g.n_zdepths; zi++) {
            if (zdepth_packed_match(&g.zdepths[zi], &t)) {
                packed_slot = zdepth_packed_view(zi + 1, &t, u);
                break;
            }
        }
        if (packed_slot >= 0) {
            slots[u] = (u32)packed_slot;
            g_ld_zdepth_srv_binds++;
            continue;
        }
        int sampled = -1;
        /* An address can be both a colour target and a zeta (a shadow map
         * pass that binds the same memory as both, colour writes masked).
         * A depth-format read of an address with a written host zeta is a
         * depth read: try the zeta first. Before, the colour surface at the
         * same address was taken (zeros). */
        int zeta_pref = 0;
        if ((t.format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM) == TEX_FMT_DEPTH24_D8)
            for (u32 i = 0; i < g.n_zdepths; i++)
                if (g.zdepths[i].location == t.location &&
                    g.zdepths[i].offset == t.offset &&
                    (g.zdepths[i].had_write || g.zdepths[i].snapshot_valid)) {
                    zeta_pref = 1;
                    break;
                }
        if (!zeta_pref) { u32 best_gen = 0;
          for (u32 i = 0; i < g.n_surfaces; i++) {
              const surface_t* sf_i = &g.surfaces[i];
              if (sf_i->location != t.location || sf_i->offset != t.offset ||
                  i == target)
                  continue;
              if (sf_i->w == t.width && sf_i->h == t.height) { sampled = (int)i; break; }
              if (sampled < 0 || sf_i->last_draw_generation > best_gen) {
                  sampled = (int)i;
                  best_gen = sf_i->last_draw_generation;
              }
          } }
        /* LD_UVDBG=1: the texture coordinates of the draw that samples the PS1
         * framebuffer. Placed here rather than in fetch_batches because that
         * function is not on this draw's path -- probing it printed nothing at
         * all, which is how that was established.
         *
         * This is the last unmeasured link: the PS3 side binds all 1024x512 of
         * PS1 VRAM as ONE texture, so which part reaches the screen is decided
         * entirely by these UVs, and every pixel the PS1 draws lives at
         * x >= 640 while columns 0..639 are black. */
        { static int uvd = -1;
          if (uvd < 0) uvd = getenv("LD_UVDBG") ? 1 : 0;
          if (uvd && t.location == 1u && t.offset == 0x400000u) {
              static unsigned long n = 0;
              if (PS3_LOG_FIRST(n, 4)) {
                  const u32 vbase = rsx_dsp_vertex_data_base_offset(&g.rsx);
                  fprintf(stderr, "[uvdbg] unit=%u ps1fb %ux%u pitch=%u fmt=0x%02X"
                                  " base=0x%X target=%u\n",
                          u, t.width, t.height, t.pitch, t.format,
                          vbase, target);
                  /* The GPU state that can silently discard a draw. Clears are
                   * visible on these surfaces and the draws are not, so one of
                   * these is throwing the fragments away. */
                  { const u32 sch = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
                    const u32 scv = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
                    fprintf(stderr, "[uvdbg]   vp=(%u,%u %ux%u) scale=(%.2f %.2f)"
                                    " xlate=(%.2f %.2f) scissor=(%u+%u, %u+%u)"
                                    " depth_test=%u zwrite=%u zfunc=0x%X"
                                    " colmask=0x%08X blend=%u cull=%u/%u"
                                    " surf=%ux%u\n",
                            vp.x, vp.y, vp.w, vp.h,
                            vp.scale[0], vp.scale[1],
                            vp.translate[0], vp.translate[1],
                            sch & 0xFFFFu, sch >> 16, scv & 0xFFFFu, scv >> 16,
                            rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1u,
                            rsx_dsp_reg(&g.rsx, 0x0A78) & 1u,
                            rsx_dsp_reg(&g.rsx, 0x0A7C),
                            rsx_dsp_reg(&g.rsx, M_COLOR_MASK),
                            rsx_dsp_reg(&g.rsx, M_BLEND_ENABLE) & 1u,
                            rsx_dsp_reg(&g.rsx, 0x1918) & 1u,
                            rsx_dsp_reg(&g.rsx, 0x090C),
                            sf.clip_w, sf.clip_h);
                    /* The PS1 framebuffer's fragment program unpacks the
                     * sampled value arithmetically (NV40 has no integer ops):
                     *
                     *   r0 = sample(tex0, tc0 * (1/1024, 1/512))
                     *   r0 = r0 * fp_constants[0].x + fp_constants[0].y
                     *   r0 = floor(r0) / 8.0
                     *   r0 = sign-preserving truncate
                     *   r0 = r0 * fp_constants[1].x + fp_constants[1].y
                     *
                     * So the output depends entirely on those two constants.
                     * Wrong or absent, it produces exactly the repeating stripe
                     * pattern the composite currently shows. */
                    fprintf(stderr, "[uvdbg]   fp_const count=%u mode=%c"
                                    " c0=(%.6f %.6f %.6f %.6f)"
                                    " c1=(%.6f %.6f %.6f %.6f)\n",
                            g.fp_constants.count, g.fp_constant_mode,
                            ld_c2f(g.fp_constants.count > 0u ? g.fp_constants.values[0][0] : 0u),
                            ld_c2f(g.fp_constants.count > 0u ? g.fp_constants.values[0][1] : 0u),
                            ld_c2f(g.fp_constants.count > 0u ? g.fp_constants.values[0][2] : 0u),
                            ld_c2f(g.fp_constants.count > 0u ? g.fp_constants.values[0][3] : 0u),
                            ld_c2f(g.fp_constants.count > 1u ? g.fp_constants.values[1][0] : 0u),
                            ld_c2f(g.fp_constants.count > 1u ? g.fp_constants.values[1][1] : 0u),
                            ld_c2f(g.fp_constants.count > 1u ? g.fp_constants.values[1][2] : 0u),
                            ld_c2f(g.fp_constants.count > 1u ? g.fp_constants.values[1][3] : 0u));
                  }
                  for (u32 vi = 0; vi < 4; vi++) {
                      float pos[4] = {0,0,0,1}, tc[4] = {0,0,0,1};
                      const int okp = fetch_attr(0, vbase, vi, 0, pos);
                      const int okt = fetch_attr(8, vbase, vi, 0, tc);
                      fprintf(stderr, "[uvdbg]   v%u pos%s(%.1f %.1f) tc%s(%.4f %.4f)\n",
                              vi, okp ? "" : "!", pos[0], pos[1],
                              okt ? "" : "!", tc[0], tc[1]);
                  }
              }
          } }
        if (sampled < 0 && LD_DIAG_ENABLED("LD_ALIAS_DBG")) {
            static u32 n_dbg = 0;
            if (PS3_LOG_FIRST(n_dbg, 24)) {
                fprintf(stderr, "[alias-miss] tex %u:0x%08X fmt=0x%02X %ux%u target=%u surf:",
                        t.location, t.offset, t.format, t.width, t.height, target);
                for (u32 i = 0; i < g.n_surfaces; i++)
                    fprintf(stderr, " [%u]%u:0x%08X %ux%u", i, g.surfaces[i].location,
                            g.surfaces[i].offset, g.surfaces[i].w, g.surfaces[i].h);
                fprintf(stderr, "\n");
            }
        }
        if (sampled < 0 && !zeta_pref && g.surfaces[target].location == t.location &&
            g.surfaces[target].offset == t.offset) {
            /* Reads its own render target: sample a copy of it. */
            slots[u] = surface_feedback_srv(target);
            g_ld_bind_surf++;
        } else if (sampled >= 0) {
            /* LD_ALIAS_DBG also reports the HIT. A texture that aliases a render
             * surface when it should not is exactly as interesting as one that
             * fails to alias when it should, and only the miss was visible --
             * so a title whose every bind resolves to a surface (binds[real=0
             * surf=N] in the frame stats) had no way to show WHY. Guitar Hero
             * III does that: every bind lands on a surface and not one real
             * texture is ever created, so the screen stays black. */
            static int alias_dbg = -1;
            if (alias_dbg < 0) alias_dbg = getenv("LD_ALIAS_DBG") != NULL;
            if (alias_dbg) {
                static u32 n_hit = 0;
                if (PS3_LOG_FIRST(n_hit, 24))
                    fprintf(stderr, "[alias-hit] tex %u:0x%08X fmt=0x%02X %ux%u "
                            "-> surface[%d] %u:0x%08X %ux%u" "\n",
                            t.location, t.offset, t.format, t.width, t.height,
                            sampled, g.surfaces[sampled].location,
                            g.surfaces[sampled].offset,
                            g.surfaces[sampled].w, g.surfaces[sampled].h);
            }
            {   /* a smaller texture reads the surface's top-left samples, texel for sample */
                const int sw = surface_window((u32)sampled, &t, alias_dbg);
                if (sw >= 0) { slots[u] = (u32)sw; g_ld_bind_surf++; continue; }
            }
            if (getenv("LD_REMAP_DIAG") && (t.remap & 0xFFFFu) != 0xAAE4u) {
                static unsigned n;
                if (n++ < 24) fprintf(stderr, "[rt-remap] unit=%u offset=%08X remap=%08X frame=%u\n", u, t.offset, t.remap, g_ld_frames);
            }
            if (g.surfaces[target].scale != g.surfaces[sampled].scale) {
                const int lv = surface_logical_view((u32)sampled);
                if (lv >= 0) { slots[u] = (u32)lv; g_ld_bind_surf++; continue; }
            }
            slots[u] = SRV_SURFACE_BASE + sampled;
            g_ld_bind_surf++;
            int seen = 0;
            for (u32 k = 0; k < n_surf_used; k++) if (surf_used[k] == (u32)sampled) seen = 1;
            if (!seen && n_surf_used < SRV_TABLE_SIZE) surf_used[n_surf_used++] = (u32)sampled;
        } else {
            int sampled_depth = -1;
            const u32 texture_base_fmt =
                t.format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
            /* Exact depth identity and authoritative GPU contents are needed.
             * A clear is a content write even when no geometry follows it. */
            if (texture_base_fmt == TEX_FMT_DEPTH24_D8)
                for (u32 i = 0; i < g.n_zdepths; i++)
                    if (g.zdepths[i].location == t.location &&
                        g.zdepths[i].offset == t.offset &&
                        (current_zslot != 1 + i || self_zeta_read)) {
                        if (zdepth_snapshot(1 + i))
                            sampled_depth = (int)i;
                        else {
                            g_ld_zdepth_srv_reject_no_write++;
                            if (!g.zdepths[i].reject_logged &&
                                rsx_live_draw_a010_probe_active()) {
                                g.zdepths[i].reject_logged = 1;
                                fprintf(stderr,
                                    "[zetatrack] a010 reject #%u %u:0x%X "
                                    "draws=%llu dtest=%llu dwrite=%llu "
                                    "both=%llu snapshot=%d\n",
                                    i, g.zdepths[i].location,
                                    g.zdepths[i].offset,
                                    (unsigned long long)g.zdepths[i].draws,
                                    (unsigned long long)
                                        g.zdepths[i].depth_test_draws,
                                    (unsigned long long)
                                        g.zdepths[i].depth_write_draws,
                                    (unsigned long long)
                                        g.zdepths[i].depth_both_draws,
                                    g.zdepths[i].snapshot_valid);
                            }
                        }
                        break;
                    }
            if (sampled_depth >= 0) {
                slots[u] = SRV_ZDEPTH_BASE + sampled_depth;
                g_ld_zdepth_srv_binds++;
                int seen = 0;
                for (u32 k = 0; k < n_zdepth_used; k++)
                    if (zdepth_used[k] == (u32)sampled_depth) seen = 1;
                if (!seen && n_zdepth_used < SRV_TABLE_SIZE)
                    zdepth_used[n_zdepth_used++] = (u32)sampled_depth;
            } else {
                slots[u] = texture_srv_slot(&t);
                if (slots[u] == SRV_WHITE) g_ld_bind_white++; else g_ld_bind_real++;
            }
        }
    }

    LD_STAGE(5);
    const u32 vtex_mask = ld_shader_resources_enabled()
        ? g_ld_current_pso.resources.vertex_textures : vertex_texture_mask();
    u32 vtex_slots[SRV_TABLE_SIZE];
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++)
        vtex_slots[u] = SRV_WHITE;
    /* Resolve and upload every vertex texture before recording this draw's
     * graphics state.  Cache refresh can retire a texture and flush the open
     * command list; doing that after SetPipelineState/root bindings silently
     * loses those bindings on the replacement list. */
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        if (!((vtex_mask >> u) & 1u)) continue;
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        vtex_slots[u] = vertex_texture_srv_slot(&vt);
        if (vtex_slots[u] != SRV_WHITE)
            g_ld_vtex_binds++;
        else
            g_ld_vtex_unsupported++;
    }
    const u32 required_srv_tables = 1u + (vtex_mask != 0);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.texture_resolve_qpc+=ld_profile_qpc()-texture_resolve_begin;
    const u64 command_encode_begin=ld_profile_qpc();
#endif
    if (g.frame->srv_ring_used + required_srv_tables > SRV_RING_TABLES ||
        sampler_table_needs_flush(smp_slots)) {
        /* Every descriptor referenced by the old command list is consumed
         * before recycling its shader-visible heap entries. Texture upload
         * commands recorded while resolving slots are also completed here;
         * no render-state command for this draw has been recorded yet. */
        ld_flush(LD_FLUSH_DESCRIPTOR_RING);
        if (!g.ready) return;
        g.frame->srv_ring_used = 0;
        g.frame->smp_ring_used = 0;
    }

    /* Source bytes and resources are resolved before native recording.
     * GPU payloads belong to this submission slot, not the dispatcher or dc.
     * Consumption stays synchronous until auxiliary operations and cache
     * leases join the same ordered protocol. */
    rsx_native_draw_packet packet = {0};
    packet.pso = pso;
    packet.root = g.rootsig_x;
    packet.heaps[0] = g.frame->srv_heap;
    packet.heaps[1] = g.frame->smp_heap;
    packet.sampled_target_count = n_surf_used;
    for (u32 k = 0; k < n_surf_used; k++)
        packet.sampled_targets[k] = g.surfaces[surf_used[k]].tex;
    const u32 logic_rtv = (rsx_dsp_reg(&g.rsx, M_LOGIC_OP_ENABLE) & 1u) &&
        g.surfaces[target].fmt == DXGI_FORMAT_R8G8B8A8_UNORM ? MAX_SURFACES : 0;
    packet.target_count = n_mrt;
    for (u32 r = 0; r < n_mrt; r++)
        packet.rtvs[r] = rtv_handle(LD_SWAP_BUFFERS + logic_rtv + mrt[r]);
    if (g.depth) {
        packet.dsv = dsv_handle(current_zslot);
        packet.have_dsv = 1;
        if (current_zslot) {
            zdepth_t* z = &g.zdepths[current_zslot - 1];
            if (!z->cleared) {
                packet.clear_depth = 1;
                z->cleared = 1;
                z->had_write = 0;
                g_ld_stats.implicit_depth_clears++;
            }
        } else if (!g.depth_cleared) {
            packet.clear_depth = 1;
            g.depth_cleared = 1;
            g_ld_stats.implicit_depth_clears++;
        }
    }
    LD_STAGE(6);
    const D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots);
    const D3D12_GPU_DESCRIPTOR_HANDLE stbl = sampler_table(smp_slots);
    D3D12_GPU_DESCRIPTOR_HANDLE vtex_table = {0};
    if (vtex_mask)
        vtex_table = srv_table(vtex_slots);
    u64 descriptor_signature =
        fnv1a(slots, sizeof(slots), 1469598103934665603ull);
    descriptor_signature =
        fnv1a(smp_slots, sizeof(smp_slots), descriptor_signature);
    if (vtex_mask)
        descriptor_signature =
            fnv1a(vtex_slots, sizeof(vtex_slots), descriptor_signature);

    const float W = sf.clip_w ? (float)sf.clip_w : (float)g.width;
    const float H = sf.clip_h ? (float)sf.clip_h : (float)g.height;
    /* The guest viewport is in pixels; an antialiased surface's clip is in
     * samples (aa_sx x aa_sy per pixel), so map NDC over the pixel extent. */
    const float Wv = W / (float)sf.aa_sx;
    const float Hv = H / (float)sf.aa_sy;
    float xf[8] = {1, 1, 1, 0, 0, 0, 0, 0};
    if (vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        xf[0] = vp.scale[0] / (Wv * 0.5f);
        xf[1] = -(vp.scale[1] / (Hv * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - Wv * 0.5f) / (Wv * 0.5f);
        xf[5] = -((vp.translate[1] - Hv * 0.5f) / (Hv * 0.5f));
        xf[6] = vp.translate[2];
    }
    int screen_pass = n_zdepth_used != 0;
    for (u32 i = 0; i < n_surf_used; i++)
        if (g.surfaces[surf_used[i]].w * 2u > g.width || g.surfaces[surf_used[i]].h * 2u > g.height)
            screen_pass = 1;
    int main_image = 0, tex_unit = -1;
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++) {
        if (!((texture_mask >> u) & 1u)) continue;
        if (g_ld_draw_textures[u].location == 1u) main_image = 1;
        if (tex_unit < 0 && slots[u] >= SRV_TEXTURE_BASE && slots[u] < SRV_TEXTURE_BASE + MAX_TEXTURES)
            tex_unit = (int)u;
    }
    /* A game texture's identity across runs: its content hash (texcache_t). */
    const u64 aspect_tex = tex_unit < 0 ? 0 : g.textures[slots[tex_unit] - SRV_TEXTURE_BASE].content_hash;
    ld_aspect_map aspect;
    const int opaque = !(rsx_dsp_reg(&g.rsx, M_BLEND_ENABLE) & 1u);
    g_ld_aspect_measured = 0;
    const int aspect_kind = ld_aspect_place(&aspect, &vp, Wv, Hv, native_geometry,
        texture_mask && !n_surf_used, opaque, main_image, screen_pass, aspect_tex);
    g_ld_aspect_counts[aspect_kind]++;
    if (g_ld_aspect_off) {   /* identity: the game's own layout, still traced */
        memset(&aspect, 0, sizeof(aspect));
        aspect.ax = aspect.ay = 1;
    }
    { static int trace = -1;
      if (trace < 0) trace = getenv("LD_ASPECT_TRACE") != NULL;
      if (trace && g_ld_aspect_k != 1.0f && g_ld_aspect_npieces < 8192 &&
          (g_ld_aspect_measured || aspect_kind == LD_ASPECT_TEXT)) {
          ld_aspect_piece* p = &g_ld_aspect_pieces[g_ld_aspect_npieces++];
          p->tex = aspect_tex; memcpy(p->box, g_ld_aspect_box, sizeof(p->box));
          p->tw = tex_unit < 0 ? 0 : (u16)g_ld_draw_textures[tex_unit].width;
          p->th = tex_unit < 0 ? 0 : (u16)g_ld_draw_textures[tex_unit].height;
          p->fmt = tex_unit < 0 ? 0 : (u8)g_ld_draw_textures[tex_unit].format;
          p->kind = (u8)aspect_kind; p->opaque = (u8)opaque; p->vp = (u32)g_ld_current_pso.vp_hash;
          p->pso = g_ld_current_pso.key;
      } }
    if (!aspect.per_vertex && aspect_kind != LD_ASPECT_STRETCH) {
        xf[0] *= aspect.ax; xf[4] = xf[4] * aspect.ax + aspect.bx;
        xf[1] *= aspect.ay; xf[5] *= aspect.ay;
    }
    u8* cbdst = g.frame->cb_mapped + g.frame->cb_used;
    LD_STAGE(7);
    memcpy(cbdst, g.rsx.constants, RSX_DSP_NUM_CONSTANTS * 16);
    memcpy(cbdst + 512 * 16, xf, sizeof(xf));
    /* Ultrawide text zones, vp_aspect0/vp_aspect1 (ld_vp_aspect_epilogue):
     * inside the block's alignment padding, written for every draw. */
    if (g_ld_aspect_k != 1.0f) {
        const float za[8] = {aspect.zones[0], aspect.zones[1], aspect.zones[2], aspect.zones[3],
                             aspect.per_vertex ? aspect.ax : 0.0f, aspect.per_vertex ? aspect.persp_ax : 0.0f,
                             aspect.persp_ay, aspect.persp_bx};
        memcpy(cbdst + 514 * 16, za, sizeof(za));
    }

    packet.vertex_constants =
        g.frame->cb->lpVtbl->GetGPUVirtualAddress(g.frame->cb) + g.frame->cb_used;
    packet.have_pixel_constants = ld_fp_constants_buffered();
    if (packet.have_pixel_constants)
        packet.pixel_constants =
            g.frame->ps_cb->lpVtbl->GetGPUVirtualAddress(g.frame->ps_cb) + ps_cb_offset;
    packet.textures = table;
    packet.samplers = stbl;
    packet.vertex_textures = vtex_table;
    packet.have_vertex_textures = vtex_mask != 0;
    g.frame->cb_used += CB_BLOCK_ALIGNED;

    const surface_t* target_surface = &g.surfaces[target];
    /* Zetas stay at the full scale. A reduced target (ld_target_scale) that
     * depth- or stencil-tests would address its zeta on the wrong grid; no
     * known title does, so name it if one ever does. */
    if (target_surface->scale != g.resolution.scale &&
        ((rsx_dsp_reg(&g.rsx, M_DEPTH_TEST_ENABLE) & 1u) ||
         (rsx_dsp_reg(&g.rsx, M_STENCIL_TEST_ENABLE) & 1u))) {
        static u32 reduced_depth_logs;
        if (PS3_LOG_FIRST(reduced_depth_logs, 8))
            fprintf(stderr, "[resolution-image] reduced target 0x%X %ux%u drawn with depth/stencil "
                    "test; its zeta is at scale %.1f\n", target_surface->offset, target_surface->w,
                    target_surface->h, g.resolution.scale);
    }
    D3D12_VIEWPORT dvp = {0, 0, W * target_surface->scale, H * target_surface->scale, 0.0f, 1.0f};
    D3D12_RECT sc = {
        0, 0,
        (LONG)(g.surfaces[target].w ? g.surfaces[target].w : g.width),
        (LONG)(g.surfaces[target].h ? g.surfaces[target].h : g.height)
    };
    /* Guest scissor, intersected with the surface rect. The nv40 reset value
     * is a full-window 4096x4096; never-written regs read 0 here, so an
     * unwritten scissor means "no scissor". A WRITTEN zero width or height is
     * a real, empty scissor and must clip everything: GH3's gameplay venue
     * sits under a ViewportElement whose clip window is off-screen (scissor
     * x=1039 w=0), and drawing it full-screen painted its never-rendered
     * texture over the whole scene. */
    {
        const u32 sh = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 sv = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        /* The scissor is in pixels too: scale it to the sample grid. */
        const LONG ax = (LONG)sf.aa_sx, ay = (LONG)sf.aa_sy;
        const LONG sx = (LONG)(sh & 0xFFFFu) * ax, sw = (LONG)(sh >> 16) * ax;
        const LONG sy = (LONG)(sv & 0xFFFFu) * ay, svh = (LONG)(sv >> 16) * ay;
        const int written = g.rsx.seen[M_SCISSOR_HORIZONTAL >> 2] != 0 ||
                            g.rsx.seen[M_SCISSOR_VERTICAL >> 2] != 0;
        if (written) {
            if (sx > sc.left)            sc.left   = sx;
            if (sy > sc.top)             sc.top    = sy;
            if (sx + sw  < sc.right)     sc.right  = sx + sw;
            if (sy + svh < sc.bottom)    sc.bottom = sy + svh;
            if (sc.right  < sc.left)     sc.right  = sc.left;
            if (sc.bottom < sc.top)      sc.bottom = sc.top;
        }
    }
    /* The draw's scissor moves with it (a per-vertex squeeze only when the
     * scissor actually clips: a 3D draw there would keep a full one). */
    if (aspect.per_vertex && sc.right - sc.left < (LONG)W) {
        /* text: the zone of the scissor's centre */
        const float nx = (sc.left + sc.right) / W - 1.0f, ny = 1.0f - (sc.top + sc.bottom) / H;
        const float* z = aspect.zones;
        aspect.bx = nx < z[0] ? aspect.ax - 1.0f
                  : (nx > z[1] || (nx > z[3] && ny < z[2])) ? 1.0f - aspect.ax : 0.0f;
    }
    if ((aspect_kind != LD_ASPECT_STRETCH && !aspect.per_vertex) ||
        (aspect.per_vertex && sc.right - sc.left < (LONG)W)) {
        const float c = W * 0.5f, bx = aspect.bx * c, cy = H * 0.5f;
        const int open = aspect.open_sides && sc.left <= 0 && sc.right >= (LONG)W;
        sc.left = open ? 0 : (LONG)floorf((sc.left - c) * aspect.ax + bx + c);
        sc.right = open ? (LONG)W : (LONG)ceilf((sc.right - c) * aspect.ax + bx + c);
        sc.top = (LONG)floorf((sc.top - cy) * aspect.ay + cy);
        sc.bottom = (LONG)ceilf((sc.bottom - cy) * aspect.ay + cy);
        if (sc.left < 0) sc.left = 0;
        if (sc.top < 0) sc.top = 0;
        if (aspect.clip_bottom > aspect.clip_top) {   /* a band: its own rows */
            const float rows = H / Hv;
            const LONG t = (LONG)floorf(aspect.clip_top * rows), b = (LONG)ceilf(aspect.clip_bottom * rows);
            if (t > sc.top) sc.top = t;
            if (b < sc.bottom) sc.bottom = b;
            if (sc.bottom < sc.top) sc.bottom = sc.top;
        }
    }
    sc.left = ld_surface_edge(target_surface, sc.left); sc.top = ld_surface_edge(target_surface, sc.top);
    sc.right = ld_surface_edge(target_surface, sc.right);
    sc.bottom = ld_surface_edge(target_surface, sc.bottom);
    packet.viewport = dvp;
    packet.scissor = sc;
    /* Dynamic stencil reference (kept out of the PSO key on purpose). */
    packet.stencil = rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu;
    /* CONSTANT_COLOR/CONSTANT_ALPHA blend factors consume NV4097's blend
     * color, not D3D12's initial all-ones factor. Bleach accumulates a bloom
     * mask in destination alpha with CONSTANT_ALPHA; leaving it at one
     * makes the full scene bloom and saturate to white. */
    const u32 blend_color = rsx_dsp_reg(&g.rsx, NV4097_SET_BLEND_COLOR);
    const FLOAT blend_factor[4] = {
        ((blend_color >> 16) & 255u) / 255.0f,
        ((blend_color >> 8) & 255u) / 255.0f,
        (blend_color & 255u) / 255.0f,
        ((blend_color >> 24) & 255u) / 255.0f
    };
    memcpy(packet.blend, blend_factor, sizeof(packet.blend));

    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = g.frame->vb->lpVtbl->GetGPUVirtualAddress(g.frame->vb) + g.frame->vb_used;
    vbv.StrideInBytes = vertex_stride;
    vbv.SizeInBytes = (u32)draw_vb_bytes;
    packet.vertices = vbv;
    packet.vertex_pull = dc.native_pull;
    packet.topology = prim == RSX_PRIMITIVE_LINE_STRIP
        ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    packet.indexed = indexed;
    packet.count = n_tri;
    {
        const u64 serial = ++g_ld_recent_draw_total;
        ld_recent_draw* draw =
            &g_ld_recent_draws[(serial - 1u) & (LD_RECENT_DRAW_CAP - 1u)];
        memset(draw, 0, sizeof(*draw));
        draw->serial = serial;
        draw->frame = g_ld_frames;
        draw->pso_key = g_ld_current_pso.key;
        draw->descriptor_signature = descriptor_signature;
        draw->vp_start = g_ld_current_pso.vp_start;
        draw->vp_instrs = g_ld_current_pso.vp_instrs;
        draw->fp_location = g_ld_current_pso.fp_location;
        draw->fp_offset = g_ld_current_pso.fp_offset;
        draw->fp_size = g_ld_current_pso.fp_size;
        draw->fp_control = g_ld_current_pso.fp_control;
        draw->cube_mask = g_ld_current_pso.cube_mask;
        draw->vtex_mask = g_ld_current_pso.vtex_mask;
        draw->txl_mask = g_ld_current_pso.txl_mask;
        draw->texture_mask = texture_mask;
        draw->primitive = prim;
        draw->vertices = n_tri;
        draw->target = target;
        draw->zslot = current_zslot;
        draw->clip_x = sf.clip_x;
        draw->clip_y = sf.clip_y;
        draw->clip_w = sf.clip_w;
        draw->clip_h = sf.clip_h;
        draw->srv_ring_used = g.frame->srv_ring_used;
        draw->sampler_ring_used = g.frame->smp_ring_used;
        draw->cb_used = g.frame->cb_used;
        draw->vb_used = g.frame->vb_used;
    }
    if (indexed) {
        packet.indices.BufferLocation =
            g.frame->ib->lpVtbl->GetGPUVirtualAddress(g.frame->ib) + g.frame->ib_used;
        packet.indices.SizeInBytes = (u32)draw_ib_bytes;
        packet.indices.Format = DXGI_FORMAT_R32_UINT;
    }
    if(native_geometry) {
        if(g.frame->geometry_input_used+((geometry->bytes+15)&~15ull)>GEOMETRY_RING_SIZE ||
           g.frame->geometry_output_used+geometry->input_indices/3*16>GEOMETRY_RING_SIZE)
            ld_flush(LD_FLUSH_VERTEX_RING);
        /* Flush retains vertex/constant offsets for this prepared draw. A
         * rotated slot is never used by this mid-frame flush. */
        if(!geometry_gpu_init(g.dev,g.frame))abort();
        geometry_gpu_packet(g.frame,geometry,&packet.geometry);
    }
    LD_STAGE(8);
    rsx_rec_draw(&packet, g.frame);
    ld_surface_note_write(target, LD_SURFACE_WRITE_DRAW);
    for (u32 r = 1; r < n_mrt; r++)
        ld_surface_note_write(mrt[r], LD_SURFACE_WRITE_DRAW);
    if (current_zslot) {
        render_state_t depth_state;
        decode_render_state(&depth_state);
        zdepth_t* z = &g.zdepths[current_zslot - 1];
        z->draws++;
        if (depth_state.depth_test) z->depth_test_draws++;
        if (depth_state.depth_write) z->depth_write_draws++;
        if (depth_state.depth_test && depth_state.depth_write)
            z->depth_both_draws++;
        /* A write-enable bit alone does not prove that this draw produced a
         * usable depth map.  With depth testing disabled RSX does not execute
         * the depth pass represented by this tracked zeta. */
        if (depth_state.depth_test && depth_state.depth_write)
            z->had_write = 1;
        const int stencil_changes = depth_state.stencil_enable && depth_state.s_write_mask &&
            (gcm_stencil_op(depth_state.s_fail) != D3D12_STENCIL_OP_KEEP ||
             gcm_stencil_op(depth_state.s_zfail) != D3D12_STENCIL_OP_KEEP ||
             gcm_stencil_op(depth_state.s_zpass) != D3D12_STENCIL_OP_KEEP ||
             (depth_state.stencil_two_sided &&
              (gcm_stencil_op(depth_state.bs_fail) != D3D12_STENCIL_OP_KEEP ||
               gcm_stencil_op(depth_state.bs_zfail) != D3D12_STENCIL_OP_KEEP ||
               gcm_stencil_op(depth_state.bs_zpass) != D3D12_STENCIL_OP_KEEP)));
        if ((depth_state.depth_test && depth_state.depth_write) || stencil_changes) {
            z->content_generation = ++g_ld_surface_generation;
            z->packed_valid = 0;
        }
    }
    g_ld_stats.groups_executed++;
    LD_STAGE(9); ld_stage_report();
    if(dc.native_pull){++g_pull_draws;g_pull_vertices+=dc.n_verts;g_pull_bytes+=draw_vb_bytes;}
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.input_vertices += dc.n_source_refs;
    g_ld_profile.total.expanded_vertices += n_tri;
    g_ld_profile.total.legacy_vertex_upload_bytes +=
        (u64)(prim == PRIM_TRIANGLES ? n_tri : dc.n_source_refs) *
        VERT_STRIDE;
    g_ld_profile.total.vertex_upload_bytes += draw_vb_bytes;
#endif

    /* A guest-side failure can occur before the next flip even though useful
     * orphanage draws have already reached internal world targets.  Preserve
     * a few progressively later surface snapshots directly from the draw
     * stream so presentation is not a prerequisite for visual diagnosis. */
    if (LD_DIAG_ENABLED("RSX_A010_EAGER_DUMP") &&
        InterlockedCompareExchange(&g_ld_a010_world_ready, 0, 0) != 0) {
        static u64 eager_origin = 0;
        static u32 eager_sample = 0;
        if (!eager_origin)
            eager_origin = g_ld_stats.groups_executed;
        if (eager_sample < 4u &&
            g_ld_stats.groups_executed >=
                eager_origin + (u64)eager_sample * 32u) {
            CreateDirectoryA("scratch\\a010_probe", NULL);
            for (u32 i = 0; i < g.n_surfaces; i++) {
                const u32 off = g.surfaces[i].offset;
                if (off != 0x00E40000u && off != 0x01800000u &&
                    off != 0x02D10000u && off != 0x02710000u &&
                    off != 0x01440000u)
                    continue;
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch\\a010_probe\\eager_%03u_group_%08llu_"
                         "surface_%02u_off_%08X.ppm",
                         eager_sample,
                         (unsigned long long)g_ld_stats.groups_executed,
                         i, off);
                ld_dump_surface_ppm(path, &g.surfaces[i]);
            }
            fprintf(stderr,
                    "[a010-eager-dump] sample=%u groups=%llu surfaces=%u\n",
                    eager_sample,
                    (unsigned long long)g_ld_stats.groups_executed,
                    g.n_surfaces);
            eager_sample++;
            fflush(stderr);
        }
    }
    g.frame->vb_used += (u32)draw_vb_bytes;
    g.frame->ib_used += (u32)draw_ib_bytes;
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.command_encode_qpc+=ld_profile_qpc()-command_encode_begin;
#endif
    ld_profile_note_ring_highwater();
    /* Default-off, bounded by the draw CSV frame range. Capture the completed
     * post-process target and its input images, after restoring resource states
     * and advancing upload rings. Readback flushes must not change those rings
     * while a draw still has pending buffer bookkeeping. */
    if (s_csv_file && s_csv_gate_open && LD_DIAG_ENABLED("LD_PASS_AUDIT")) {
        render_state_t audit_rs; decode_render_state(&audit_rs);
        D3D12_RESOURCE_DESC ad;
        const u8* audit_fp = guest_ptr(g_ld_current_pso.fp_location,g_ld_current_pso.fp_offset,g_ld_current_pso.fp_size);
        const u64 audit_fp_hash = audit_fp ? rsx_fp_structural_hash(audit_fp,g_ld_current_pso.fp_size,1469598103934665603ull) : 0;
        g.surfaces[target].tex->lpVtbl->GetDesc(g.surfaces[target].tex, &ad);
        fprintf(s_csv_file, "#audit,%u,%llu,pipeline=%016llx,target=%u:%X,logical=%ux%u,physical=%llux%u,samples=%u,slots=",
            g_ld_frames, (unsigned long long)s_csv_last_draw,
            (unsigned long long)g_ld_current_pso.key, target, g.surfaces[target].offset,
            g.surfaces[target].w, g.surfaces[target].h,
            (unsigned long long)ad.Width, ad.Height, ad.SampleDesc.Count);
        for (u32 u=0;u<SRV_TABLE_SIZE;u++) if (texture_mask & (1u<<u))
            fprintf(s_csv_file,"%u:%u;",u,slots[u]);
        fprintf(s_csv_file,",program=%016llx,fp=",(unsigned long long)audit_fp_hash);
        for (u32 i=0;i<g.fp_constants.count;i++)
            fprintf(s_csv_file,"%g/%g/%g/%g;",ld_c2f(g.fp_constants.values[i][0]),
                ld_c2f(g.fp_constants.values[i][1]),ld_c2f(g.fp_constants.values[i][2]),
                ld_c2f(g.fp_constants.values[i][3]));
        fputc('\n',s_csv_file); fflush(s_csv_file);
        const char* dir = getenv("LD_PASS_AUDIT");
        /* LD_PASS_AUDIT_ALL=1: the target after every draw of the frame, to
         * see which draw puts a value (e.g. the bloom mask in alpha) somewhere. */
        if (dir && (n_tri > 6 || audit_rs.depth_test || audit_rs.blend_enable) &&
            LD_DIAG_ENABLED("LD_PASS_AUDIT_ALL")) {
            char path[MAX_PATH*2];
            snprintf(path,sizeof(path),"%s/draw_%04llu_out_%08X.ppm",dir,
                (unsigned long long)s_csv_last_draw,g.surfaces[target].offset);
            ld_dump_surface_ppm(path,&g.surfaces[target]);
        }
        if (dir && n_tri <= 6 && !audit_rs.depth_test && !audit_rs.blend_enable) {
            char path[MAX_PATH*2];
            snprintf(path,sizeof(path),"%s/pass_%04llu_out_%08X.ppm",dir,
                (unsigned long long)s_csv_last_draw,g.surfaces[target].offset);
            ld_dump_surface_ppm(path,&g.surfaces[target]);
            for (u32 u=0;u<SRV_TABLE_SIZE;u++) if ((texture_mask & (1u<<u)) &&
                slots[u]>=SRV_SURFACE_BASE && slots[u]<SRV_SURFACE_BASE+g.n_surfaces) {
                const surface_t* src=&g.surfaces[slots[u]-SRV_SURFACE_BASE];
                snprintf(path,sizeof(path),"%s/pass_%04llu_in%u_%08X.ppm",dir,
                    (unsigned long long)s_csv_last_draw,u,src->offset);
                ld_dump_surface_ppm(path,src);
            }
        }
    }
}

static void sink_end(void* user, const rsx_dispatch* r)
{
#if defined(YZ_PERF_PROFILE)
    const LONGLONG begin = ld_profile_qpc();
#endif
    rsx_geometry_lease geometry=rsx_geometry_handoff_take();
    sink_end_geometry_impl(user,r,&geometry);
    rsx_geometry_handoff_release(geometry.token);
#if defined(YZ_PERF_PROFILE)
    g_ld_profile.total.sink_end_qpc += (u64)(ld_profile_qpc() - begin);
#endif
}

static void sink_clear(void* user, const rsx_dispatch* r, u32 mask)
{
    (void)user; (void)r;
    if (g_ld_movie_mode && !ld_movie_composite_ui_enabled()) return;
    g_ld_stats.clears++;
    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) { g_ld_stats.clear_drop_surface++; return; }
    if (rsx_live_draw_a010_probe_active() && target < 64)
        g_ld_a010_probe_touched |= 1ull << target;
    if (ld_target_trace_enabled()) {
        static u32 last_clear_target = LD_INVALID_SURFACE;
        const int changed = target != last_clear_target;
        if (changed) {
            ld_trace_target("clear-target", target, mask);
            last_clear_target = target;
        }
        /* A broken scene can issue tens of thousands of clears. Preserve the
         * opening sequence and a periodic heartbeat without turning tracing
         * itself into a scheduler perturbation. Target changes are always
         * emitted above. */
        if (g_ld_stats.clears <= 256 || (g_ld_stats.clears & 1023) == 0) {
            const u32 z = rsx_dsp_clear_zstencil(&g.rsx);
            fprintf(stderr,
                    "[rsx-clear] frame=%u n=%llu target=%u mask=0x%02X "
                    "argb=0x%08X z24=0x%06X stencil=0x%02X\n",
                    g_ld_frames, g_ld_stats.clears, target, mask,
                    rsx_dsp_clear_color(&g.rsx), z >> 8, z & 0xFF);
        }
    }
    if (s_csv_file && s_csv_gate_open) {
        fprintf(s_csv_file, "#clear,%u,0x%X,mask=0x%02X,scissor=0x%08X/0x%08X,cmask=0x%08X,argb=0x%08X\n",
                g_ld_frames, g.surfaces[target].offset, mask,
                rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL), rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL),
                rsx_dsp_reg(&g.rsx, M_COLOR_MASK), rsx_dsp_clear_color(&g.rsx));
        fflush(s_csv_file);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(LD_SWAP_BUFFERS + target);
    if (mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G | RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A)) {
        const u32 c = rsx_dsp_clear_color(&g.rsx);
        float col[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                         (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f };
        /* LD_CLEAR_TEST=1: clear to magenta instead of the guest's colour.
         *
         * This validates the MEASUREMENT, not the renderer. LD_SURF_DUMP
         * reports nonblack=0 for every surface, and every conclusion drawn from
         * that depends on the readback actually being able to see a non-black
         * pixel. If magenta clears still read back as 0, the readback is broken
         * and those conclusions are void. Establish that X would be visible
         * before concluding X never happens. */
        { static int ct = -1;
          if (ct < 0) ct = getenv("LD_CLEAR_TEST") ? 1 : 0;
          if (ct) { col[0] = 1.0f; col[1] = 0.0f; col[2] = 1.0f; col[3] = 1.0f; } }
        rsx_rec_ClearRenderTargetView(g.frame, rtv, col, 0, NULL);
        ld_surface_note_write(target, LD_SURFACE_WRITE_CLEAR);
    }
    if ((mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) && g.depth) {
        rsx_dsp_surface sf;
        rsx_dsp_get_surface(&g.rsx, &sf);
        const u32 zslot = zdepth_get(sf.zeta_location, sf.zeta_offset,
                                     sf.clip_w, sf.clip_h, sf.zeta_pitch,
                                     sf.depth_format, sf.raster_type);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_handle(zslot);
        D3D12_CLEAR_FLAGS clear_flags = (D3D12_CLEAR_FLAGS)0;
        if (mask & RSX_CLEAR_DEPTH)
            clear_flags = (D3D12_CLEAR_FLAGS)(clear_flags | D3D12_CLEAR_FLAG_DEPTH);
        if (mask & RSX_CLEAR_STENCIL)
            clear_flags = (D3D12_CLEAR_FLAGS)(clear_flags | D3D12_CLEAR_FLAG_STENCIL);
        /* ZSTENCIL_CLEAR_VALUE is laid out as the zeta is: depth24 << 8 |
         * stencil8 for Z24S8, a bare depth16 for Z16 (SURFACE_FORMAT depth
         * field 1). Read as Z24S8, a Z16 surface's 0xFFFF -- its far plane --
         * would clear it to 255/16777215, nearly the near plane, and every
         * LESS or LEQUAL draw into it would then fail. The nv40 reset
         * 0xFFFFFF00 seeded by rsx_dispatch_init clears to 1.0 / 0. Same
         * decode as the draw engine's clear. */
        {
            const u32 zs = rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR);
            const int z16 = sf.depth_format == 1u;
            /* Zero is a valid explicit clear value. The dispatcher already
             * seeds the reset value; zero must never mean "not initialized". */
            const float cd = z16 ? (float)(zs & 0xFFFFu) / 65535.0f
                                 : (float)(zs >> 8) / 16777215.0f;
            rsx_rec_ClearDepthStencilView(g.frame, dsv,
                clear_flags, cd, z16 ? (UINT8)0 : (UINT8)(zs & 0xFFu), 0, NULL);
        }
        if (zslot) {
            g.zdepths[zslot - 1].cleared = 1;
            g.zdepths[zslot - 1].content_generation = ++g_ld_surface_generation;
            g.zdepths[zslot - 1].packed_valid = 0;
            if (mask & RSX_CLEAR_DEPTH) {
                g.zdepths[zslot - 1].had_write = 1;
                /* A clear creates a new content version just like a depth
                 * draw. Resolve lazily at the first texture consumer, even
                 * if there are no casters/draws after this clear. */
                g.zdepths[zslot - 1].snapshot_valid = 0;
            }
        } else {
            if (mask & RSX_CLEAR_DEPTH)
                g.depth_cleared = 1;
        }
    }
}

static void sink_flip(void* user, const rsx_dispatch* r, u32 arg)
{
    (void)user; (void)r;
    const u32 buffer_id = arg & 7u;
    g_ld_flip_requested++;
    g_ld_last_requested_buffer = buffer_id;
    if (g_ld_movie_mode) {
        if (ld_movie_composite_ui_enabled()) ld_movie_capture_overlay();
        g_ld_flip_consumed++;
        g_ld_last_consumed_buffer = buffer_id;
        return;
    }
    rsx_live_draw_present(buffer_id);
    g_ld_flip_consumed++;
    g_ld_last_consumed_buffer = buffer_id;
}

/* ---------------------------------------------------------------------------
 * device / resource setup
 * -----------------------------------------------------------------------*/
static int make_root_signature(void)
{
    D3D12_DESCRIPTOR_RANGE xrange = {0};
    xrange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    xrange.NumDescriptors = SRV_TABLE_SIZE;
    D3D12_DESCRIPTOR_RANGE srange = {0};
    srange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    srange.NumDescriptors = SMP_TABLE_SIZE;
    D3D12_DESCRIPTOR_RANGE vrange = {0};
    vrange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    vrange.NumDescriptors = RSX_DSP_NUM_VERTEX_TEXTURES;
    vrange.BaseShaderRegister = 16;
    D3D12_ROOT_PARAMETER xp[6] = {0};
    xp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    xp[0].Descriptor.ShaderRegister = 0;
    xp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    xp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xp[1].DescriptorTable.NumDescriptorRanges = 1;
    xp[1].DescriptorTable.pDescriptorRanges = &xrange;
    xp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    xp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xp[2].DescriptorTable.NumDescriptorRanges = 1;
    xp[2].DescriptorTable.pDescriptorRanges = &srange;
    xp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    xp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    xp[3].DescriptorTable.NumDescriptorRanges = 1;
    xp[3].DescriptorTable.pDescriptorRanges = &vrange;
    xp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    xp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    xp[4].Descriptor.ShaderRegister = 1;
    xp[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    xp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    xp[5].Descriptor.ShaderRegister = 20;
    xp[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_STATIC_SAMPLER_DESC vsmp[RSX_DSP_NUM_VERTEX_TEXTURES] = {0};
    for (u32 i = 0; i < RSX_DSP_NUM_VERTEX_TEXTURES; i++) {
        vsmp[i].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        vsmp[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        vsmp[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        vsmp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        vsmp[i].MaxLOD = D3D12_FLOAT32_MAX;
        vsmp[i].ShaderRegister = i;
        vsmp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = {0};
    rsd.NumParameters = 6; rsd.pParameters = xp;
    rsd.NumStaticSamplers = RSX_DSP_NUM_VERTEX_TEXTURES;
    rsd.pStaticSamplers = vsmp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        if (err) err->lpVtbl->Release(err); return -1;
    }
    HRESULT hr = g.dev->lpVtbl->CreateRootSignature(g.dev, 0, sig->lpVtbl->GetBufferPointer(sig),
                                                    sig->lpVtbl->GetBufferSize(sig),
                                                    &IID_ID3D12RootSignature, (void**)&g.rootsig_x);
    sig->lpVtbl->Release(sig);
    return SUCCEEDED(hr) ? 0 : -1;
}

static int make_depth_snapshot_pipeline(void)
{
    static const char source[] =
        "Texture2D<float> depth_source : register(t0);\n"
        "RWTexture2D<float> depth_destination : register(u0);\n"
        "[numthreads(8, 8, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "    uint width, height;\n"
        "    depth_destination.GetDimensions(width, height);\n"
        "    if (id.x < width && id.y < height)\n"
        "        depth_destination[id.xy] = "
        "depth_source.Load(int3(id.xy, 0));\n"
        "}\n";
    ID3DBlob* shader = NULL;
    ID3DBlob* errors = NULL;
    HRESULT hr = D3DCompile(
        source, sizeof(source) - 1u, "depth_snapshot_compute", NULL, NULL,
        "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &shader, &errors);
    if (FAILED(hr)) {
        fprintf(stderr,
                "[zetatrack] depth snapshot shader compile failed "
                "hr=0x%08lX: %s\n",
                (unsigned long)hr,
                errors ? (const char*)errors->lpVtbl->GetBufferPointer(errors)
                       : "<no diagnostics>");
        if (errors) errors->lpVtbl->Release(errors);
        return -1;
    }
    if (errors) errors->lpVtbl->Release(errors);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {0};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER parameters[2] = {0};
    for (u32 i = 0; i < 2; ++i) {
        parameters[i].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable.NumDescriptorRanges = 1;
        parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC description = {0};
    description.NumParameters = 2;
    description.pParameters = parameters;
    ID3DBlob* serialized = NULL;
    ID3DBlob* signature_errors = NULL;
    hr = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &signature_errors);
    if (FAILED(hr)) {
        fprintf(stderr,
                "[zetatrack] depth snapshot root signature failed "
                "hr=0x%08lX: %s\n",
                (unsigned long)hr,
                signature_errors
                    ? (const char*)signature_errors->lpVtbl->GetBufferPointer(
                          signature_errors)
                    : "<no diagnostics>");
        if (signature_errors)
            signature_errors->lpVtbl->Release(signature_errors);
        shader->lpVtbl->Release(shader);
        return -1;
    }
    if (signature_errors)
        signature_errors->lpVtbl->Release(signature_errors);
    hr = g.dev->lpVtbl->CreateRootSignature(
        g.dev, 0, serialized->lpVtbl->GetBufferPointer(serialized),
        serialized->lpVtbl->GetBufferSize(serialized),
        &IID_ID3D12RootSignature, (void**)&g.depth_snapshot_rootsig);
    serialized->lpVtbl->Release(serialized);
    if (FAILED(hr)) {
        shader->lpVtbl->Release(shader);
        return -1;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {0};
    pipeline.pRootSignature = g.depth_snapshot_rootsig;
    pipeline.CS.pShaderBytecode = shader->lpVtbl->GetBufferPointer(shader);
    pipeline.CS.BytecodeLength = shader->lpVtbl->GetBufferSize(shader);
    hr = g.dev->lpVtbl->CreateComputePipelineState(
        g.dev, &pipeline, &IID_ID3D12PipelineState,
        (void**)&g.depth_snapshot_pso);
    shader->lpVtbl->Release(shader);
    if (FAILED(hr)) return -1;
    /* Pack before filtering: quantizing an already filtered float depth is
     * not equivalent to sampling the original packed-byte texture. */
    static const char packed_source[] =
        "Texture2D<float> depth_source : register(t0);\n"
        "Texture2D<uint> stencil_source : register(t1);\n"
        "RWTexture2D<float4> packed_destination : register(u0);\n"
        "uint unorm24(float d) {\n"
        "    if (!(d > 0.0)) return 0;\n"
        "    if (d >= 1.0) return 16777215;\n"
        "    precise float scaled = d * 16777216.0;\n"
        "    uint n = (uint)scaled;\n"
        "    if (d > 0.5) return n - 1;\n"
        "    return n + (scaled - (float)n > 0.5 ? 1 : 0);\n"
        "}\n"
        "[numthreads(8,8,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "    uint width, height; packed_destination.GetDimensions(width, height);\n"
        "    if (id.x >= width || id.y >= height) return;\n"
        "    uint z = unorm24(depth_source.Load(int3(id.xy,0)));\n"
        "    uint st = stencil_source.Load(int3(id.xy,0));\n"
        "    // Big-endian Z24S8 bytes read as ARGB: R=mid, G=low, B=stencil, A=high.\n"
        "    packed_destination[id.xy] = float4((z >> 8)&255, z&255, st&255, z>>16) / 255.0;\n"
        "}\n";
    shader = NULL; errors = NULL;
    hr = D3DCompile(packed_source, sizeof(packed_source)-1, "packed_depth", NULL, NULL,
        "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "[zetatrack] packed shader failed: %s\n",
            errors ? (const char*)errors->lpVtbl->GetBufferPointer(errors) : "no diagnostics");
        if (errors) errors->lpVtbl->Release(errors);
        return -1;
    }
    if (errors) errors->lpVtbl->Release(errors);
    pipeline.CS.pShaderBytecode = shader->lpVtbl->GetBufferPointer(shader);
    pipeline.CS.BytecodeLength = shader->lpVtbl->GetBufferSize(shader);
    hr = g.dev->lpVtbl->CreateComputePipelineState(g.dev, &pipeline,
        &IID_ID3D12PipelineState, (void**)&g.packed_depth_pso);
    shader->lpVtbl->Release(shader);
    return SUCCEEDED(hr) ? 0 : -1;
}

int rsx_live_draw_init(void* hwnd, u32 width, u32 height,
                       rsx_live_guest_ptr_fn guest_fn, void* guest_user)
{
    if (!rsx_live_draw_enabled()) return 0;
    if (g.ready) return 0;
    ld_present_measure_init();
#if defined(YZ_PERF_PROFILE)
    memset(&g_ld_profile, 0, sizeof(g_ld_profile));
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_ld_profile.qpc_frequency = frequency.QuadPart;
        g_ld_profile.previous_present_qpc = ld_profile_qpc();
    }
#endif
    g.width = width; g.height = height;
    g.resolution.scale = 1;
    const char* scale = getenv("BLEACH_RENDER_SCALE");
    if (scale) {
        char* end = NULL;
        g.resolution.scale = strtof(scale, &end);
        if (!end || end == scale || *end || !rsx_resolution_valid(g.resolution) || g.resolution.scale > 4) {
            fprintf(stderr, "[resolution] BLEACH_RENDER_SCALE must be 1..4 in steps of 0.5\n");
            return -1;
        }
    }
    if (!ld_physical(width) || !ld_physical(height)) return -1;
    fprintf(stderr, "[resolution] logical=%ux%u native=%ux%u scale=%.1f asset_textures=original\n",
        width, height, ld_physical(width), ld_physical(height), g.resolution.scale);
    g_ld_aspect_k = rsx_aspect_factor();
    if (g_ld_aspect_k != 1.0f)
        fprintf(stderr, "[aspect] display=%.4f k=%.4f swap=%ux%u (2D squeezed by 1/k, canvas stretched by k)\n",
            rsx_aspect_ratio(), g_ld_aspect_k, ((u32)(ld_physical(height) * rsx_aspect_ratio() + 1)) & ~1u,
            ld_physical(height));
    g.guest_ptr = guest_fn; g.guest_user = guest_user;
    {
        const char* requested = getenv("RSX_FP_CONSTANT_MODE");
        char mode = 'B';
        if (requested && requested[0]) {
            if ((requested[0] == 'L' || requested[0] == 'l') &&
                (!requested[1] ||
                 _stricmp(requested, "literal") == 0))
                mode = 'L';
            else if ((requested[0] == 'B' || requested[0] == 'b') &&
                     (!requested[1] ||
                      _stricmp(requested, "buffered") == 0))
                mode = 'B';
            else
                fprintf(
                    stderr,
                    "[rsx-fp-constants] unknown "
                    "RSX_FP_CONSTANT_MODE='%s'; using buffered\n",
                    requested);
        }
        g.fp_constant_mode = mode;
        fprintf(
            stderr,
            "[rsx-fp-constants] mode=%s (startup-selected) "
            "d3dcompile_flags1=0x0 d3dcompile_flags2=0x0\n",
            ld_fp_constant_mode_name());
    }
    {
        const char* requested = getenv("RSX_VERTEX_MODE");
        const char* old_path = getenv("RSX_VERTEX_PATH");
        char mode = 'C';
        if (requested && requested[0] && !requested[1]) {
            mode = (char)toupper((unsigned char)requested[0]);
        } else if (!requested && old_path) {
            mode = strcmp(old_path, "compact") == 0 ? 'C' : 'L';
        }
        switch (mode) {
        case 'L':
            g.vertex_features = 0;
            break;
        case 'H':
            g.vertex_features = LD_VERTEX_HOIST_FETCH;
            break;
        case 'M':
            g.vertex_features =
                LD_VERTEX_HOIST_FETCH | LD_VERTEX_MASK_SIGNATURE;
            break;
        case 'C':
            g.vertex_features =
                LD_VERTEX_HOIST_FETCH | LD_VERTEX_MASK_SIGNATURE |
                LD_VERTEX_COMPACT_PAYLOAD;
            break;
        default:
            fprintf(stderr,
                    "[rsx-vertex] unknown RSX_VERTEX_MODE='%s'; "
                    "using C\n",
                    requested ? requested : "");
            mode = 'C';
            g.vertex_features =
                LD_VERTEX_HOIST_FETCH | LD_VERTEX_MASK_SIGNATURE |
                LD_VERTEX_COMPACT_PAYLOAD;
            break;
        }
        g.vertex_mode = mode;
#if !defined(YZ_PERF_CLEAN)
        const char* diag_dir = getenv("RSX_VERTEX_DIAG_DIR");
        if (diag_dir && diag_dir[0]) {
            strncpy(
                g.vertex_diag_dir, diag_dir,
                sizeof(g.vertex_diag_dir) - 1u);
            g.vertex_diag_dir[sizeof(g.vertex_diag_dir) - 1u] = '\0';
        }
#endif
        fprintf(stderr,
                "[rsx-vertex] mode=%s features=0x%X "
                "(startup-selected)\n",
                ld_vertex_mode_name(), g.vertex_features);
    }
    g_ld_flip_requested = 0;
    g_ld_flip_consumed = 0;
    g_ld_last_requested_buffer = UINT32_MAX;
    g_ld_last_consumed_buffer = UINT32_MAX;
    g_ld_last_present_target = UINT32_MAX;
#if !defined(YZ_PERF_CLEAN)
    g_ld_surface_generation = 0;
    g_ld_surface_resource_serial = 0;
    g_ld_guest_blit_generation = 0;
    g_ld_present_copy_generation = 0;
    g_ld_vertex_constant_ring_recycles = 0;
    g_ld_frame_vertex_constant_ring_recycles = 0;
    g_ld_fifo_get = 0;
    g_ld_fifo_put = 0;
    InterlockedExchange(&g_ld_diag_post_movie_pending, 0);
    InterlockedExchange(&g_ld_diag_post_movie_presents, 0);
#endif
    g_ld_last_dump_fingerprint = 0;
    memset(g_ld_layout_cache, 0, sizeof(g_ld_layout_cache));
    g_ld_layout_cache_count = 0;

    ld_enable_debug_layer();
    ld_enable_dred();
    IDXGIFactory4* factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory))) return -1;
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&g.dev))) {
        factory->lpVtbl->Release(factory); return -1;
    }
#if defined(YZ_PERF_PROFILE)
    {
        LUID luid = {0};
        g.dev->lpVtbl->GetAdapterLuid(g.dev, &luid);
        factory->lpVtbl->EnumAdapterByLuid(
            factory, luid, &IID_IDXGIAdapter3,
            (void**)&g_ld_profile.adapter);
    }
#endif
    ld_open_info_queue();
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    g.dev->lpVtbl->CreateCommandQueue(g.dev, &qd, &IID_ID3D12CommandQueue, (void**)&g.queue);
    g.frame_count = g_requested_frames_in_flight;
    g.frame_index = 0; g.frame = &g.frames[0];
    for(u32 slot=0;slot<g.frame_count;++slot) {
        if(!ld_frame_resources_create(g.dev,&g.frames[slot]) ||
           (slot && FAILED(g.frames[slot].list->lpVtbl->Close(g.frames[slot].list)))) {
            for(u32 i=0;i<=slot;++i)ld_frame_resources_destroy(&g.frames[i]);
            return -1;
        }
    }
    fprintf(stderr,"[native-submission] resource_slots=%u completion=host-fence-tickets\n",g.frame_count);
    if (g.queue) g.queue->lpVtbl->SetName(g.queue, L"rsx-live-queue");
    if (g.frame->list) g.frame->list->lpVtbl->SetName(g.frame->list, L"rsx-live-list");
    g.dev->lpVtbl->CreateFence(g.dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&g.fence);
    g.fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
#if defined(YZ_PERF_PROFILE)
    /* Each flush already waits for its fence; read the timestamp pair there
     * without adding another synchronization point. CPU fence wait and GPU
     * execution duration are deliberately separate measurements. */
    if (g.queue && g.frame->list && SUCCEEDED(g.queue->lpVtbl->GetTimestampFrequency(g.queue, &g.timing_frequency))) {
        if (g.frame->timing_heap && g.frame->timing_readback)
            rsx_rec_EndQuery(g.frame, g.frame->timing_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    }
#endif

    /* swap chain bound to the runtime's HWND */
    const char* vsync=getenv("BLEACH_VSYNC");
    g.present_interval=!(vsync && !strcmp(vsync,"0"));
    IDXGIFactory5* factory5=NULL;
    if(SUCCEEDED(factory->lpVtbl->QueryInterface(factory,&IID_IDXGIFactory5,(void**)&factory5))) {
        BOOL support=FALSE;
        if(SUCCEEDED(factory5->lpVtbl->CheckFeatureSupport(factory5,
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,&support,sizeof(support))))g.allow_tearing=support;
        factory5->lpVtbl->Release(factory5);
    }
    DXGI_SWAP_CHAIN_DESC1 scd = {0};
    scd.Width = ld_physical(width); scd.Height = ld_physical(height); scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    /* Ultrawide: the swap chain has the monitor's shape; presentation stretches the
     * canvas onto it (the size mismatch takes the filtered transfer). */
    if (g_ld_aspect_k != 1.0f) scd.Width = ((u32)(scd.Height * rsx_aspect_ratio() + 1)) & ~1u;
    scd.SampleDesc.Count = 1; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = LD_SWAP_BUFFERS; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags=g.allow_tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    fprintf(stderr,"[native-presentation] vsync=%u tearing_supported=%u\n",g.present_interval,(unsigned)g.allow_tearing);
    IDXGISwapChain1* sc1 = NULL;
    if (FAILED(factory->lpVtbl->CreateSwapChainForHwnd(factory, (IUnknown*)g.queue,
            (HWND)hwnd, &scd, NULL, NULL, &sc1))) {
        factory->lpVtbl->Release(factory); return -1;
    }
    sc1->lpVtbl->QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&g.swap);
    sc1->lpVtbl->Release(sc1);
    /* BLEACH_DISPLAY_MODE=exclusiva: exclusive fullscreen on the window's
     * monitor; on refusal the borderless popup window remains. */
    { const char* mode=getenv("BLEACH_DISPLAY_MODE");
      if(mode && !strcmp(mode,"exclusiva")) {
          HRESULT fs=g.swap->lpVtbl->SetFullscreenState(g.swap,TRUE,NULL);
          /* BLEACH_FULLSCREEN_HZ=<hz>: switch the monitor to the closest mode at that
           * refresh rate (keeping its resolution). 60 FPS on a 144 Hz panel without
           * VRR holds frames for 2 and 3 refreshes in turn -- a double image on
           * anything the eye tracks; at 120 Hz every frame is exactly 2 refreshes. */
          { const char* hz=getenv("BLEACH_FULLSCREEN_HZ"); IDXGIOutput* output=NULL;
            if(SUCCEEDED(fs) && hz && atoi(hz)>0 &&
               SUCCEEDED(g.swap->lpVtbl->GetContainingOutput(g.swap,&output))) {
                DXGI_OUTPUT_DESC od; DXGI_MODE_DESC want={0}, got={0};
                output->lpVtbl->GetDesc(output,&od);
                want.Width=(UINT)(od.DesktopCoordinates.right-od.DesktopCoordinates.left);
                want.Height=(UINT)(od.DesktopCoordinates.bottom-od.DesktopCoordinates.top);
                want.RefreshRate.Numerator=(UINT)atoi(hz); want.RefreshRate.Denominator=1;
                want.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                HRESULT mr=output->lpVtbl->FindClosestMatchingMode(output,&want,&got,NULL);
                if(SUCCEEDED(mr)) mr=g.swap->lpVtbl->ResizeTarget(g.swap,&got);
                fprintf(stderr,"[native-presentation] fullscreen mode %ux%u @ %.3f Hz (asked %s Hz, hr=0x%08lX)\n",
                        got.Width,got.Height,got.RefreshRate.Denominator?(double)got.RefreshRate.Numerator/got.RefreshRate.Denominator:0.0,
                        hz,(unsigned long)mr);
                output->lpVtbl->Release(output);
            } }
          if(SUCCEEDED(fs)) fs=g.swap->lpVtbl->ResizeBuffers(g.swap,0,0,0,DXGI_FORMAT_UNKNOWN,scd.Flags);
          fprintf(stderr,"[native-presentation] exclusive fullscreen %s (hr=0x%08lX)\n",
                  SUCCEEDED(fs)?"active":"refused, borderless kept",(unsigned long)fs);
      } }
    factory->lpVtbl->Release(factory);

    /* RTV heap: [0..1] backbuffers, [2..] surface cache */
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = RTV_VIEW_CACHE_BASE + LD_VIEW_CACHE_CAP;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.rtv_heap);
    g.rtv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (u32 i = 0; i < LD_SWAP_BUFFERS; i++) {
        g.swap->lpVtbl->GetBuffer(g.swap, i, &IID_ID3D12Resource, (void**)&g.backbuf[i]);
        rsx_rec_drain();
        if (g.frame) g.frame->bindings.valid &= ~8u;
        g.dev->lpVtbl->CreateRenderTargetView(g.dev, g.backbuf[i], NULL, rtv_handle(i));
    }

    /* SRV heaps */
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = SRV_CPU_HEAP_SLOTS; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g.srv_cpu_heap);
    g.srv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    /* sampler heaps */
    D3D12_DESCRIPTOR_HEAP_DESC shd = {0};
    shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    shd.NumDescriptors = SMP_CACHE_SLOTS + 1; shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &shd, &IID_ID3D12DescriptorHeap, (void**)&g.smp_cpu_heap);
    g.smp_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    /* Fail init (fall back to null present) rather than crash later if any
     * shader-visible/CPU descriptor heap didn't create -- e.g. an over-limit
     * sampler heap would return NULL here and fault in sampler_table. */
    if (!g.srv_cpu_heap || !g.frame->srv_heap || !g.smp_cpu_heap || !g.frame->smp_heap) {
        fprintf(stderr, "[live-draw] descriptor heap alloc failed "
                "(srv_cpu=%p srv=%p smp_cpu=%p smp=%p)\n",
                (void*)g.srv_cpu_heap, (void*)g.frame->srv_heap,
                (void*)g.smp_cpu_heap, (void*)g.frame->smp_heap);
        return -1;
    }
    {
        D3D12_SAMPLER_DESC def = {0};
        def.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        def.AddressU = def.AddressV = def.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        def.MaxLOD = D3D12_FLOAT32_MAX; def.MaxAnisotropy = 1;
        g.dev->lpVtbl->CreateSampler(g.dev, &def, smp_cpu(SMP_DEFAULT));
    }

    /* depth */
    D3D12_DESCRIPTOR_HEAP_DESC dhd = {0};
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dhd.NumDescriptors = 1 + MAX_SURFACES;
    g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &dhd, &IID_ID3D12DescriptorHeap, (void**)&g.dsv_heap);
    g.dsv_step = g.dev->lpVtbl->GetDescriptorHandleIncrementSize(
        g.dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    {
        D3D12_HEAP_PROPERTIES dhp = {0}; dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC drd = {0};
        drd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        drd.Width = ld_physical(width); drd.Height = ld_physical(height); drd.DepthOrArraySize = 1;
        drd.MipLevels = 1; drd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; drd.SampleDesc.Count = 1;
        drd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE dcv = {0}; dcv.Format = drd.Format; dcv.DepthStencil.Depth = 1.0f;
        if (SUCCEEDED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &dhp, D3D12_HEAP_FLAG_NONE, &drd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, &IID_ID3D12Resource, (void**)&g.depth))) {
            D3D12_CPU_DESCRIPTOR_HANDLE dh;
            g.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.dsv_heap, &dh);
            rsx_rec_drain();
            if (g.frame) g.frame->bindings.valid &= ~8u;
            g.dev->lpVtbl->CreateDepthStencilView(g.dev, g.depth, NULL, dh);
        } else {
            g.depth = NULL;
        }
    }

    if (make_root_signature() != 0 ||
        make_depth_snapshot_pipeline() != 0)
        return -1;

    static const u8 white[4] = {255, 255, 255, 255};
    g.white_tex = create_texture_rgba(white, 1, 1);
    if (g.white_tex) srv_write(SRV_WHITE, g.white_tex);

    /* dispatcher + sink */
    rsx_dispatch_sink sink = {0};
    sink.user = &g;
    sink.clear = sink_clear;
    sink.begin = sink_begin;
    sink.end = sink_end;
    sink.draw_arrays = sink_draw_arrays;
    sink.inline_array = sink_inline_array;
    sink.draw_index_array = sink_draw_index;
    sink.flip = sink_flip;
    rsx_dispatch_init(&g.rsx, &sink);

    g.ready = 1;
    rsx_rec_initialize();
    g.enabled = 1;
    return 0;
}

void rsx_live_draw_seed_registers(const u32* regs, u32 count)
{
    if (g.ready) rsx_dispatch_seed_registers(&g.rsx, regs, count);
}
void rsx_live_draw_seed_transform_program(const u32* words, u32 count)
{
    if (g.ready) rsx_dispatch_seed_transform_program(&g.rsx, words, count);
}
void rsx_live_draw_seed_transform_constants(const u32* words, u32 count)
{
    if (g.ready) rsx_dispatch_seed_transform_constants(&g.rsx, words, count);
}

void rsx_live_draw_set_display_buffer(
    u32 buffer_id, u32 location, u32 offset, u32 pitch, u32 width, u32 height)
{
    if (buffer_id >= 8) return;
    display_buffer_t* display = &g.display_buffers[buffer_id];
    display->location = location;
    display->offset = offset;
    display->pitch = pitch;
    display->width = width;
    display->height = height;
    display->valid = width && height;
    display->resolve_src = 0;
    fprintf(stderr,
            "[live-draw] display buffer %u = loc%u:0x%08X pitch=%u %ux%u\n",
            buffer_id, location, offset, pitch, width, height);
}

static u64 g_ingress_packets,g_ingress_words;
static void ld_ingress_report(void) {
    fprintf(stderr,"[native-ingress] packets=%llu words=%llu\n",g_ingress_packets,g_ingress_words);
}
int rsx_live_draw_native_ingress(void) {
    static int enabled=-1;
    if(enabled<0) {
        const char* e=getenv("RSX_NATIVE_INGRESS");enabled=e && !strcmp(e,"1");
        const char* diagnostics[]={"GCM_OBJDBG","GCM_SUBCH_GATE","GCM_VAL_WATCH","GCM_METHOD_HIST",
            "RSX_LIVE_FEED_DBG","RSX_TRACE","SURFDBG","RTT_DUMP","VDATA_DBG","LOAD_DBG",
            "TCONST_DBG","SEQ_DBG","MVPDBG","RSX_VERTEX_STATE_TRACE"};
        for(unsigned i=0;i<sizeof(diagnostics)/sizeof(*diagnostics);i++)
            if(getenv(diagnostics[i]))enabled=0;
        if(enabled)atexit(ld_ingress_report);
        fprintf(stderr,"[native-ingress] enabled=%d\n",enabled);
    }
    const rsx_backend* backend=rsx_get_backend();
    return enabled && g.ready && (!backend || backend->redundant_with_live_renderer);
}
void rsx_live_draw_packet(u32 method,const void* be_words,u32 count,int increment) {
    ++g_ingress_packets;g_ingress_words+=count;
    if(!ld_movie_composite_ui_enabled() && !g_ld_debug_layer_enabled && !g_ld_movie_mode && g.ready) {
        rsx_dispatch_packet(&g.rsx,method,be_words,count,increment);return;
    }
    const unsigned char* p=(const unsigned char*)be_words;
    for(u32 i=0;i<count;i++) {
        const unsigned char* b=p+i*4;
        const u32 value=((u32)b[0]<<24)|((u32)b[1]<<16)|((u32)b[2]<<8)|b[3];
        rsx_live_draw_method(method+(increment?i*4:0),value);
    }
}

void rsx_live_draw_method(u32 method, u32 arg)
{
    /* Bounded ingress trace for the a010 live-vs-capture comparison.  Keep the
     * raw method (including its FIFO subchannel bits) visible: if an SPU-built
     * command list binds NV4097 on a different subchannel, feeding the raw
     * 0x2xxx-shifted method into the canonical dispatcher silently stores the
     * state in the wrong register bank. */
    static int state_trace = -1;
    static u32 state_trace_lines = 0;
    if (state_trace < 0)
        state_trace = getenv("RSX_VERTEX_STATE_TRACE") ? 1 : 0;
    if (state_trace && state_trace_lines < 512) {
        const u32 canonical = method & 0x1FFCu;
        const int vertex_texture =
            canonical >= 0x0900u && canonical < 0x0980u;
        const int vertex_format =
            canonical >= 0x1740u && canonical < 0x1780u;
        const int frequency_divider = canonical == 0x1FC0u;
        if (vertex_texture || vertex_format || frequency_divider) {
            fprintf(stderr,
                    "[rsx-vstate] frame=%u raw=0x%04X sub=%u canonical=0x%04X "
                    "arg=0x%08X\n",
                    g_ld_frames, method & 0xFFFFu, (method >> 13) & 7u,
                    canonical, arg);
            fflush(stderr);
            state_trace_lines++;
        }
    }

    const int composite = ld_movie_composite_ui_enabled();
    const int serialized = composite || g_ld_debug_layer_enabled;
    if (serialized) {
        for (;;) {
            if (g_ld_movie_mode || g_ld_host_waiting) {
                while (g_ld_host_waiting)
                    SwitchToThread();
                AcquireSRWLockExclusive(&g_ld_access_lock);
                if (!g.ready) {
                    ReleaseSRWLockExclusive(&g_ld_access_lock);
                    return;
                }
                if (g_ld_movie_mode && !composite) {
                    if (g_ld_movie_track_rsx < 0)
                        g_ld_movie_track_rsx =
                            getenv("RSX_MOVIE_TRACK") ? 1 : 0;
                    if (!g_ld_movie_track_rsx) {
                        ReleaseSRWLockExclusive(&g_ld_access_lock);
                        return;
                    }
                }
                rsx_dispatch_method(&g.rsx, method, arg);
                ReleaseSRWLockExclusive(&g_ld_access_lock);
                return;
            }
            InterlockedIncrement(&g_ld_guest_active);
            MemoryBarrier();
            if (!g_ld_movie_mode && !g_ld_host_waiting) {
                if (g.ready)
                    rsx_dispatch_method(&g.rsx, method, arg);
                InterlockedDecrement(&g_ld_guest_active);
                return;
            }
            InterlockedDecrement(&g_ld_guest_active);
        }
    }

    if (!g.ready) return;
    if (g_ld_movie_mode) {
        if (g_ld_movie_track_rsx < 0)
            g_ld_movie_track_rsx = getenv("RSX_MOVIE_TRACK") ? 1 : 0;
        if (!g_ld_movie_track_rsx) return;
    }
    rsx_dispatch_method(&g.rsx, method, arg);
}

void rsx_live_draw_set_movie_mode(int on)
{
    static unsigned long long suppressed_at_start = 0;
    const int composite = ld_movie_composite_ui_enabled();
    const int serialized = composite || g_ld_debug_layer_enabled;
    if (on && rsx_live_draw_a010_probe_active()) {
        fprintf(stderr,
                "[a010-probe] END movie-mode live_frame=%u elapsed_frames=%u "
                "surfaces=%u samples=%u touched=0x%016llX\n",
                g_ld_frames, g_ld_frames - g_ld_a010_probe_start_frame,
                g.n_surfaces, g_ld_a010_probe_sample,
                (unsigned long long)g_ld_a010_probe_touched);
        fflush(stderr);
        InterlockedExchange(&g_ld_a010_probe_active, 0);
    }
    if (serialized) {
        InterlockedExchange(&g_ld_host_waiting, 1);
        if (on) InterlockedExchange((volatile LONG*)&g_ld_movie_mode, 1);
        while (InterlockedCompareExchange(&g_ld_guest_active, 0, 0) != 0)
            SwitchToThread();
        AcquireSRWLockExclusive(&g_ld_access_lock);
    }
    if (on) {
        suppressed_at_start = g_ld_stats.packets_movie;
        if (composite) ld_movie_overlay_begin();
        else g_ld_movie_mode = 1;
    } else {
        if (composite) {
            g.movie_overlay_valid = 0;
            /* Do not expose a partially rendered auth/fade surface between
             * the last host movie frame and the next clean guest scene. */
            ld_flush(LD_FLUSH_MOVIE);
            const float black[4] = {0, 0, 0, 1};
            for (u32 i = 0; i < g.n_surfaces; i++) {
                rsx_rec_ClearRenderTargetView(g.frame, rtv_handle(LD_SWAP_BUFFERS + i), black, 0, NULL);
                ld_surface_note_write(i, LD_SURFACE_WRITE_OTHER);
            }
            if (g.n_surfaces) ld_flush(LD_FLUSH_MOVIE);
            ld_movie_reset_rings();
            InterlockedExchange((volatile LONG*)&g_ld_movie_mode, 0);
            fprintf(stderr, "[movie-ui] compositor disarmed after %llu guest overlays\n",
                    (unsigned long long)g.movie_overlay_frames);
            fflush(stderr);
        } else {
            g_ld_movie_mode = 0;
        }
        if (!composite && g_ld_movie_track_rsx > 0) {
            fprintf(stderr,
                    "[live-draw] movie handoff: tracked RSX state, suppressed %llu guest draw packets\n",
                    g_ld_stats.packets_movie - suppressed_at_start);
            fflush(stderr);
        }
#if !defined(YZ_PERF_CLEAN)
        if (g.vertex_diag_dir[0]) {
            InterlockedExchange(&g_ld_diag_post_movie_presents, 0);
            InterlockedExchange(&g_ld_diag_post_movie_pending, 1);
        }
#endif
    }
    ld_vertex_diag_emit(on ? "movie-begin" : "movie-end", 0);
    if (serialized) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
    }
}

u32 rsx_live_draw_get_frames(void) { return g_ld_frames; }

void rsx_live_draw_set_fifo_position(u32 get, u32 put)
{
#if defined(YZ_PERF_CLEAN)
    (void)get;
    (void)put;
#else
    g_ld_fifo_get = get;
    g_ld_fifo_put = put;
#endif
}

void rsx_live_draw_note_inline_transfer(u32 dma, u32 offset, u32 value)
{
#if defined(YZ_PERF_CLEAN)
    (void)dma;
    (void)offset;
    (void)value;
#else
    if (!g.ready || !yz_frontier_trace_is_armed())
        return;
    const u32 location = dma == 0xFEED0000u ? 0u :
                         dma == 0xFEED0001u ? 1u : UINT32_MAX;
    if (location == UINT32_MAX)
        return;
    for (u32 i = 0; i < g.n_surfaces; ++i) {
        surface_t* surface = &g.surfaces[i];
        const u64 begin = surface->offset;
        const u64 end = begin + (u64)surface->w * surface->h * 4u;
        if (surface->location != location || offset < begin || offset >= end)
            continue;
        surface->last_guest_blit_generation = ++g_ld_guest_blit_generation;
        yz_frontier_trace_emit(
            YZ_FT_PARITY_GUEST_BLIT, i, location,
            surface->last_guest_blit_generation, offset, value,
            g_ld_fifo_get, g_ld_fifo_put, surface->last_write_generation);
    }
#endif
}

void* rsx_live_draw_get_present_thread_handle(void)
{
#if defined(YZ_PPU_SAMPLE)
    return (void*)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_ld_present_thread_handle, NULL, NULL);
#else
    return NULL;
#endif
}

static u32 g_ld_last_frame_draws = 0;
/* Draws in the last COMPLETED frame (title-bar telemetry: distinguishes
 * "presenting fresh content" from "flipping a static image" -- the dead
 * journal-consumer limp state renders ~0 draws/frame while flips tick). */
u32 rsx_live_draw_get_last_draws(void) { return g_ld_last_frame_draws; }

/* Successful presentations in the trailing five seconds, divided by elapsed
 * time. During startup use the available history; include stalls in the window.
 * The short shared lock protects only title timestamps, never GPU operations. */
double rsx_live_draw_get_present_fps(void)
{
    LARGE_INTEGER now;
    double fps = 0.0;
    if (g_ld_qpc_frequency <= 0) return 0.0;
    AcquireSRWLockShared(&g_ld_title_present_lock);
    if (g_ld_title_present_total > 1 && QueryPerformanceCounter(&now)) {
        LONGLONG start = now.QuadPart - 5 * g_ld_qpc_frequency;
        if (start < g_ld_title_first_present_qpc)
            start = g_ld_title_first_present_qpc;
        const u64 available = g_ld_title_present_total < LD_PRESENT_RING_CAP
            ? g_ld_title_present_total : LD_PRESENT_RING_CAP;
        const LONGLONG oldest = g_ld_title_present_qpc[
            (g_ld_title_present_total - available) & (LD_PRESENT_RING_CAP - 1u)];
        if (start < oldest) start = oldest;
        u64 frames = 0;
        for (u64 i = 0; i < available; ++i) {
            if (g_ld_title_present_qpc[(g_ld_title_present_total - 1u - i)
                    & (LD_PRESENT_RING_CAP - 1u)] <= start) break;
            ++frames;
        }
        if (now.QuadPart > start)
            fps = (double)frames * (double)g_ld_qpc_frequency / (double)(now.QuadPart - start);
    }
    ReleaseSRWLockShared(&g_ld_title_present_lock);
    return fps;
}

void rsx_live_draw_dump_present_samples(void)
{
    ld_present_measure_dump();
    /* The host may exit after draining without destroying the renderer. This
     * entry point is called on its owning renderer thread, before exit. */
    ld_native_graphics_report();
}

/* Present a host-decoded RGBA8 frame to the window: copy it straight into the
 * swap-chain backbuffer (both R8G8B8A8_UNORM at the swap size) and Present.
 * The frame is clamped to the backbuffer size. Call from a single thread with
 * movie mode on (so guest draws don't touch g.frame->list). */
void rsx_live_draw_present_rgba(const uint8_t* rgba, u32 w, u32 h)
{
    const int composite = ld_movie_composite_ui_enabled();
    const int serialized = composite || g_ld_debug_layer_enabled;
    if (serialized) {
        InterlockedExchange(&g_ld_host_waiting, 1);
        AcquireSRWLockExclusive(&g_ld_access_lock);
    }
    if (!g.ready || !rgba) {
        if (serialized) {
            ReleaseSRWLockExclusive(&g_ld_access_lock);
            InterlockedExchange(&g_ld_host_waiting, 0);
        }
        return;
    }
    if (composite && !ld_movie_overlay_ensure()) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
        return;
    }
    if (w > g.width)  w = g.width;
    if (h > g.height) h = g.height;
    const u32 pitch = (w * 4 + 255) & ~255u;          /* D3D12 copy pitch align */
    if ((UINT64)pitch * h > UPLOAD_SIZE) {
        if (serialized) {
            ReleaseSRWLockExclusive(&g_ld_access_lock);
            InterlockedExchange(&g_ld_host_waiting, 0);
        }
        return;
    }

    u8* host_upload = composite ? g.movie_upload_mapped : g.frame->upload_mapped;
    for (u32 y = 0; y < h; y++) {
        u8* dstrow = host_upload + (size_t)y * pitch;
        memcpy(dstrow, rgba + (size_t)y * w * 4, (size_t)w * 4);
        if (composite && g.movie_overlay_valid && g.movie_overlay_rgba &&
            g.movie_overlay_mask) {
            const u8* ov = g.movie_overlay_rgba + (size_t)y * g.width * 4;
            const u8* mask =
                g.movie_overlay_mask + (size_t)y * g.width;
            for (u32 x = 0; x < w; x++, ov += 4) {
                const int coverage = mask[x];
                if (!coverage) continue;
                u8* out = dstrow + (size_t)x * 4;
                if (coverage == 1) {
                    out[0] = (u8)((out[0] * 64 + 127) / 255);
                    out[1] = (u8)((out[1] * 64 + 127) / 255);
                    out[2] = (u8)((out[2] * 64 + 127) / 255);
                    continue;
                }
                const int white = ov[0] > ov[1]
                    ? (ov[0] > ov[2] ? ov[0] : ov[2])
                    : (ov[1] > ov[2] ? ov[1] : ov[2]);
                for (int c = 0; c < 3; c++) {
                    const int v =
                        (white * coverage +
                         out[c] * (255 - coverage) + 127) / 255;
                    out[c] = (u8)v;
                }
                out[3] = 255;
            }
        }
    }

    const u32 bbi = g.swap->lpVtbl->GetCurrentBackBufferIndex(g.swap);
    ID3D12Resource* bb = g.backbuf[bbi];

    ID3D12Resource* destination = bb;
    if (g.resolution.scale != 1 || g_ld_aspect_k != 1.0f) {
        destination = ld_host_image(0, w, h);
        if (!destination) {
            if (serialized) {
                ReleaseSRWLockExclusive(&g_ld_access_lock);
                InterlockedExchange(&g_ld_host_waiting, 0);
            }
            return;
        }
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = destination;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = destination == bb ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    rsx_rec_ResourceBarrier(g.frame, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = destination; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    src.pResource = composite ? g.movie_upload : g.frame->upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    rsx_rec_CopyTextureRegion(g.frame, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = destination == bb ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_RENDER_TARGET;
    rsx_rec_ResourceBarrier(g.frame, 1, &b);

    if (destination != bb) {
        const float uv[4] = {0, 0, 1, 1};
        /* Ultrawide: the movie keeps its 16:9 shape, centred between black bars. */
        D3D12_RESOURCE_STATES bb_state = D3D12_RESOURCE_STATE_PRESENT;
        u32 x = 0;
        if (g_ld_aspect_k != 1.0f) {
            D3D12_RESOURCE_DESC bd; bb->lpVtbl->GetDesc(bb, &bd);
            x = (u32)bd.Width > ld_physical(g.width) ? ((u32)bd.Width - ld_physical(g.width)) / 2 : 0;
            b.Transition.pResource = bb;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.StateAfter = bb_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            rsx_rec_ResourceBarrier(g.frame, 1, &b);
            const FLOAT black[4] = {0, 0, 0, 1};
            rsx_rec_ClearRenderTargetView(g.frame, rtv_handle(bbi), black, 0, NULL);
        }
        if (!ld_transfer_image(destination, SRV_HOST_IMAGE_BASE,
            D3D12_RESOURCE_STATE_RENDER_TARGET, bb, rtv_handle(bbi),
            bb_state, DXGI_FORMAT_R8G8B8A8_UNORM,
            uv, x, 0, ld_physical(g.width), ld_physical(g.height))) {
            fprintf(stderr, "[image-transfer] movie composition failed\n");
            g.ready = 0;
        }
        if (bb_state != D3D12_RESOURCE_STATE_PRESENT) {
            b.Transition.StateBefore = bb_state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            rsx_rec_ResourceBarrier(g.frame, 1, &b);
        }
    }
    ld_flush(LD_FLUSH_MOVIE_PRESENT);
    {
        const HRESULT present_hr = ld_present_swapchain();
        if (SUCCEEDED(present_hr))
            ld_present_measure_record(g_ld_frames);
        else {
            fprintf(stderr,
                    "[d3d-fail] movie Present hr=0x%08lX frame=%u\n",
                    (unsigned long)present_hr, g_ld_frames);
            ld_dump_dred("MoviePresent", present_hr);
            g.ready = 0;
        }
    }
    if (serialized) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
    }
}

/* Env-gated (RSX_DUMP) framebuffer dump: read the current color surface back
 * and write a binary PPM. Self-contained -- creates + releases its own readback
 * buffer, so no init/struct changes. Uses g.frame->list which ld_flush leaves open. */
/* Readback texel -> 8-bit RGB.  This title renders HDR into FP16 targets, so a
 * dump has to tone it down rather than reinterpret the bytes.  Reinhard keeps
 * the whole range visible without pretending to match the guest tonemapper. */
static void ld_dump_texel_rgb(const u8* p, int fp16, u8* out)
{
    if (!fp16) { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; return; }
    for (int c = 0; c < 3; c++) {
        const u16 h = (u16)(p[c * 2] | (p[c * 2 + 1] << 8));
        const int ex = (h >> 10) & 0x1F, ma = h & 0x3FF;
        float f;
        if (ex == 0)       f = (float)ma * (1.0f / 16384.0f) * (1.0f / 64.0f);
        else if (ex == 31) f = 65504.0f;
        else { f = 1.0f + (float)ma / 1024.0f;
               int k = ex - 15;
               while (k > 0) { f *= 2.0f; k--; }
               while (k < 0) { f *= 0.5f; k++; } }
        if (h & 0x8000) f = 0.0f;
        f = f / (1.0f + f);                          /* Reinhard */
        out[c] = (u8)(f * 255.0f + 0.5f);
    }
}

/* --record: when set, ld_dump_surface_ppm appends to this ffmpeg pipe instead
 * of opening `path`, and skips its per-dump diagnostics (every frame goes
 * through here while recording). */
static FILE* g_ld_record_to;

static u64 ld_dump_surface_ppm(const char* path, const surface_t* surface)
{
    ID3D12Resource* rt = surface ? surface->tex : NULL;
    if (!rt) return UINT64_MAX;
    D3D12_RESOURCE_DESC physical; rt->lpVtbl->GetDesc(rt, &physical);
    const u32 width = (u32)physical.Width;
    const u32 height = physical.Height;
    if (!width || !height) return UINT64_MAX;
    const int fp16 = surface->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const u32 texel = fp16 ? 8u : 4u;
    const u32 pitch = (width * texel + 255) & ~255u;             /* 256-align */
    const UINT64 rb_size = (UINT64)pitch * height;

    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = rb_size;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&rb)))
        return UINT64_MAX;

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rsx_rec_ResourceBarrier(g.frame, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = surface->fmt;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    rsx_rec_CopyTextureRegion(g.frame, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rsx_rec_ResourceBarrier(g.frame, 1, &b);

    ld_flush(LD_FLUSH_READBACK);                   /* copy completes on the GPU */

    u64 nonblack = UINT64_MAX;
    u64 fingerprint = 1469598103934665603ull;
    u8* px = NULL; D3D12_RANGE rr = {0, (SIZE_T)rb_size};
    if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, (void**)&px))) {
        nonblack = 0;
        if (!g_ld_record_to) {   /* raw texels, to tell an FP16 target from 8-bit data read as one */
            const u8* r0 = px + (SIZE_T)(height / 4) * pitch + (width / 4) * texel;
            fprintf(stderr, "[dump-raw] %s fmt=%d texel=%u bytes:", path, (int)surface->fmt, texel);
            for (u32 bi = 0; bi < 16; bi++) fprintf(stderr, " %02X", r0[bi]);
            fprintf(stderr, "\n");
        }
        FILE* f = g_ld_record_to ? g_ld_record_to : fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", width, height);
            for (u32 y = 0; y < height; y++) {
                const u8* row = px + (SIZE_T)y * pitch;
                for (u32 x = 0; x < width; x++) {
                    const u8* pixel = row + x * texel;
                    u8 rgb[3]; ld_dump_texel_rgb(pixel, fp16, rgb);
                    if (rgb[0] || rgb[1] || rgb[2])
                        nonblack++;
                    fingerprint = fnv1a(rgb, 3, fingerprint);
                    fwrite(rgb, 1, 3, f);  /* RGBA->RGB */
                }
            }
            if (f != g_ld_record_to) {
                fclose(f);
                fprintf(stderr, "[live-draw] wrote %s\n", path);
            }
            /* LD_DUMP_ALPHA=1: the alpha channel beside it as <path>.a.pgm.
             * Bleach keeps its bloom mask in the scene's destination alpha. */
            if (f != g_ld_record_to && !fp16 && LD_DIAG_ENABLED("LD_DUMP_ALPHA")) {
                char apath[MAX_PATH * 2];
                snprintf(apath, sizeof(apath), "%s.a.pgm", path);
                FILE* af = fopen(apath, "wb");
                if (af) {
                    fprintf(af, "P5\n%u %u\n255\n", width, height);
                    for (u32 y = 0; y < height; y++) {
                        const u8* row = px + (SIZE_T)y * pitch;
                        for (u32 x = 0; x < width; x++) fputc(row[x * texel + 3], af);
                    }
                    fclose(af);
                }
            }
        } else {
            for (u32 y = 0; y < height; y++) {
                const u8* row = px + (SIZE_T)y * pitch;
                for (u32 x = 0; x < width; x++) {
                    const u8* pixel = row + x * texel;
                    u8 rgb[3]; ld_dump_texel_rgb(pixel, fp16, rgb);
                    if (rgb[0] || rgb[1] || rgb[2])
                        nonblack++;
                    fingerprint = fnv1a(rgb, 3, fingerprint);
                }
            }
        }
        D3D12_RANGE wr = {0, 0};
        rb->lpVtbl->Unmap(rb, 0, &wr);
        g_ld_last_dump_fingerprint = fingerprint;
    }
    rb->lpVtbl->Release(rb);
    return nonblack;
}

#if !defined(YZ_PERF_CLEAN)
static void ld_parity_capture_surface(
    const char* directory, const char* phase, u32 serial,
    u32 buffer_id, u32 target)
{
    if (!directory || !directory[0] || target >= g.n_surfaces)
        return;
    surface_t* surface = &g.surfaces[target];
    char path[MAX_PATH * 2];
    snprintf(
        path, sizeof(path),
        "%s\\%s_%03u_flip_%llu_buf_%u_surface_%u_write_%u_kind_%u.ppm",
        directory, phase, serial,
        (unsigned long long)g_ld_flip_requested,
        buffer_id, target, surface->last_write_generation,
        surface->last_write_kind);
    const u64 nonblack = ld_dump_surface_ppm(path, surface);
    fprintf(
        stderr,
        "[parity-capture] phase=%s serial=%u flip=%llu buffer=%u "
        "surface=%u resource=%u write=%u kind=%u draw=%u clear=%u "
        "copy=%u blit=%u resolve=%u other=%u guest_blit=%u "
        "present_copy=%u get=%08X put=%08X cb=%u vb=%u srv=%u "
        "groups=%llu drop_ring=%llu cb_recycles=%llu frame_cb_recycles=%u "
        "hash=%016llX nonblack=%s%llu path=%s\n",
        phase, serial, (unsigned long long)g_ld_flip_requested,
        buffer_id, target, surface->resource_serial,
        surface->last_write_generation, surface->last_write_kind,
        surface->last_draw_generation, surface->last_clear_generation,
        surface->last_copy_generation, surface->last_blit_generation,
        surface->last_resolve_generation, surface->last_other_generation,
        surface->last_guest_blit_generation,
        surface->last_present_copy_generation,
        g_ld_fifo_get, g_ld_fifo_put, g.frame->cb_used, g.frame->vb_used,
        g.frame->srv_ring_used,
        (unsigned long long)g_ld_stats.groups_executed,
        (unsigned long long)g_ld_stats.group_drop_ring,
        (unsigned long long)g_ld_vertex_constant_ring_recycles,
        g_ld_frame_vertex_constant_ring_recycles,
        (unsigned long long)g_ld_last_dump_fingerprint,
        nonblack == UINT64_MAX ? "unknown:" : "",
        (unsigned long long)(nonblack == UINT64_MAX ? 0 : nonblack),
        path);
    fflush(stderr);
}
#endif

int rsx_live_draw_debug_dump_surface(
    u32 location, u32 offset, const char* path)
{
    if (!g.ready || !path || !path[0]) return -1;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const surface_t* surface = &g.surfaces[i];
        if (surface->location != location || surface->offset != offset)
            continue;
        const u64 nonblack = ld_dump_surface_ppm(path, surface);
        if (nonblack == UINT64_MAX) return -1;
        fprintf(stderr,
                "[live-replay-boundary] surface=%u:%08X index=%u "
                "size=%ux%u fmt=%d nonblack=%llu path=%s\n",
                location, offset, i, surface->w, surface->h, (int)surface->fmt,
                (unsigned long long)nonblack, path);
        fflush(stderr);
        return 0;
    }
    fprintf(stderr,
            "[live-replay-boundary] missing surface=%u:%08X path=%s\n",
            location, offset, path);
    return 1;
}

static void ld_vertex_diag_emit(const char* reason, int dump_surface)
{
    if (!g.ready || !g.vertex_diag_dir[0])
        return;
    rsx_dsp_surface state_surface;
    rsx_dsp_get_surface(&g.rsx, &state_surface);
    u32 current = LD_INVALID_SURFACE;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        if (g.surfaces[i].location == state_surface.color_location[0] &&
            g.surfaces[i].offset == state_surface.color_offset[0]) {
            current = i;
            break;
        }
    }
    const u32 presented = g_ld_last_present_target;
    const surface_t* presented_surface =
        presented < g.n_surfaces ? &g.surfaces[presented] : NULL;
    char attrs[80];
    size_t used = 0;
    attrs[0] = '\0';
    for (u32 attr = 0; attr < 16; attr++) {
        if (!(g_ld_current_pso.input_mask & (1u << attr)))
            continue;
        const int written = snprintf(
            attrs + used, sizeof(attrs) - used,
            "%s%u", used ? "," : "", attr);
        if (written < 0 || (size_t)written >= sizeof(attrs) - used)
            break;
        used += (size_t)written;
    }
    const UINT64 d3d_messages = g_ld_info_queue
        ? g_ld_info_queue->lpVtbl->GetNumStoredMessages(g_ld_info_queue)
        : 0;
#if defined(YZ_PERF_PROFILE)
    const u64 pso_lookups = g_ld_profile.total.pso_lookups;
    const u64 pso_hits = g_ld_profile.total.pso_hits;
    const u64 pso_misses = g_ld_profile.total.pso_misses;
    const u64 pso_full = g_ld_profile.total.pso_full;
#else
    const u64 pso_lookups = 0, pso_hits = 0, pso_misses = 0, pso_full = 0;
#endif
    fprintf(stderr,
            "[rsx-vertex-state] reason=%s mode=%s frame=%u "
            "flip{requested=%llu last_req=%u consumed=%llu last_cons=%u} "
            "surface{present=%u off=0x%08X size=%ux%u current=%u} "
            "draw{packets=%llu groups=%llu exec=%llu "
            "drop_fetch=%llu drop_pso=%llu drop_ring=%llu} "
            "pso{look=%llu hit=%llu miss=%llu full=%llu "
            "cached=%u capacity=%u} "
            "vp{hash=%016llX start=%u instrs=%u mask=0x%04X "
            "attrs=[%s] stride=%u packed=%u} "
            "d3d{debug=%d queued_messages=%llu}\n",
            reason ? reason : "unknown", ld_vertex_mode_name(), g_ld_frames,
            (unsigned long long)g_ld_flip_requested,
            g_ld_last_requested_buffer,
            (unsigned long long)g_ld_flip_consumed,
            g_ld_last_consumed_buffer,
            presented,
            presented_surface ? presented_surface->offset : 0,
            presented_surface ? presented_surface->w : 0,
            presented_surface ? presented_surface->h : 0,
            current,
            (unsigned long long)g_ld_stats.packets_seen,
            (unsigned long long)g_ld_stats.groups_seen,
            (unsigned long long)g_ld_stats.groups_executed,
            (unsigned long long)g_ld_stats.group_drop_fetch,
            (unsigned long long)g_ld_stats.group_drop_pso,
            (unsigned long long)g_ld_stats.group_drop_ring,
            (unsigned long long)pso_lookups,
            (unsigned long long)pso_hits,
            (unsigned long long)pso_misses,
            (unsigned long long)pso_full,
            g.n_psos, MAX_PSOS,
            (unsigned long long)g_ld_current_pso.vp_hash,
            g_ld_current_pso.vp_start,
            g_ld_current_pso.vp_instrs,
            g_ld_current_pso.input_mask,
            attrs,
            g_ld_current_pso.input_stride,
            g_ld_current_pso.packed_offsets,
            g_ld_debug_layer_enabled,
            (unsigned long long)d3d_messages);
    if (dump_surface && presented_surface && g.vertex_diag_dir[0]) {
        char path[MAX_PATH * 2];
        snprintf(
            path, sizeof(path), "%s\\last_present_surface.ppm",
            g.vertex_diag_dir);
        const u64 nonblack =
            ld_dump_surface_ppm(path, presented_surface);
        fprintf(stderr,
                "[rsx-vertex-surface] mode=%s target=%u "
                "pixels=%llu nonblack=%s%llu path=%s\n",
                ld_vertex_mode_name(), presented,
                (unsigned long long)presented_surface->w *
                    presented_surface->h,
                nonblack == UINT64_MAX ? "unknown:" : "",
                (unsigned long long)(
                    nonblack == UINT64_MAX ? 0 : nonblack),
                path);
    }
    fflush(stderr);
}

void rsx_live_draw_present(u32 buffer_id)
{
    if (!g.ready) return;

    /* LD_SURF_DUMP=<dir>: dump EVERY live surface once, with its non-black
     * count, at LD_SURF_DUMP_FRAME (default 1200).
     *
     * LD_FRAME_DUMP only reads back the presented surface, which answers "is
     * the screen black" (it is: nonblack=0 on every frame) but not WHERE the
     * pixels stop. ps1_netemu composites PS1 VRAM into a 720x512 surface and
     * then upscales into the presented 1280x720 one, so the interesting
     * question is which of those is already empty. */
    { static int done = 0; const char* sd = getenv("LD_SURF_DUMP");
      u32 at = 1200; { const char* fe = getenv("LD_SURF_DUMP_FRAME");
                       if (fe) at = (u32)strtoul(fe, 0, 0); }
      /* LD_SURF_DUMP_ON_VRAM=1 waits for PS1 VRAM to hold a drawn frame instead
       * of firing at a frame number. Needs PS1_PC=1, which is what samples it. */
      int gate;
      { static int onv = -1;
        if (onv < 0) onv = getenv("LD_SURF_DUMP_ON_VRAM") ? 1 : 0;
        gate = onv ? (g_ld_ps1_vram_ready != 0) : (g_ld_frames >= at);
        /* LD_SURF_DUMP_GATE=<file>: fire when <file> appears instead. */
        static const char* gf = (const char*)-1;
        if (gf == (const char*)-1) gf = getenv("LD_SURF_DUMP_GATE");
        /* Re-arms when the file goes away, so each appearance dumps once. */
        if (gf && sd) { FILE* f = fopen(gf, "rb"); gate = f != NULL; if (f) fclose(f);
                        if (!gate) done = 0; } }
      if (sd && sd[0] && !done && gate) {
          done = 1;
          for (u32 i = 0; i < g.n_surfaces; i++) {
              char path[MAX_PATH * 2];
              snprintf(path, sizeof(path), "%s\\f%06u_surf_%02u_%08X_%ux%u.ppm",
                       sd, g_ld_frames, i, g.surfaces[i].offset,
                       g.surfaces[i].w, g.surfaces[i].h);
              const u64 nb = ld_dump_surface_ppm(path, &g.surfaces[i]);
              fprintf(stderr, "[surf-dump] slot=%u %u:0x%08X %ux%u nonblack=%llu"
                              " draw_gen=%u clear_gen=%u\n",
                      i, g.surfaces[i].location, g.surfaces[i].offset,
                      g.surfaces[i].w, g.surfaces[i].h,
                      (unsigned long long)(nb == UINT64_MAX ? 0 : nb),
                      g.surfaces[i].last_draw_generation,
                      g.surfaces[i].last_clear_generation);
          }
      } }
    /* LD_FRAME_DUMP=<dir> [+ LD_FRAME_DUMP_EVERY=<n>, default 300]: write the
     * PRESENTED surface to a .ppm every n flips. The engine could already dump
     * a surface, but only at shutdown or behind the parity harness -- neither
     * reachable when a title is simply left running, which is exactly when you
     * want to see what is on screen. PrintWindow and CopyFromScreen both come
     * back white for a D3D12 swapchain, so a readback here is the only way to
     * answer "what is it actually drawing?". */
    /* The dump itself runs once the presented surface is known (below): a
     * title that scans out through cellResc never matches a registered display
     * buffer and presents the fallback surface, so matching here dumped nothing. */
    static int dump_every = -1; static const char* dump_dir; static unsigned dump_n;
    if (dump_every < 0) { dump_dir = getenv("LD_FRAME_DUMP");
                          const char* e = getenv("LD_FRAME_DUMP_EVERY");
                          dump_every = (dump_dir && dump_dir[0]) ? (e ? atoi(e) : 300) : 0;
                          if (dump_every <= 0 && dump_dir && dump_dir[0]) dump_every = 300; }
    /* LD_FRAME_DUMP_FROM=<frame>: start dumping there (dense captures of a
     * late scene, e.g. frames for a GIF, without thousands of early files). */
    static long dump_from = -1;
    if (dump_from < 0) { const char* e = getenv("LD_FRAME_DUMP_FROM"); dump_from = e ? atol(e) : 0; }
    /* Keep image readback out of a timed benchmark even when FPS changes. */
    static ULONGLONG dump_origin, dump_after_ms, dump_interval_ms, dump_next_ms;
    if (!dump_origin) {
        dump_origin=GetTickCount64();
        const char* e=getenv("LD_FRAME_DUMP_AFTER");
        dump_after_ms=e?1000ull*strtoull(e,NULL,10):0;
        e=getenv("LD_FRAME_DUMP_INTERVAL_MS");
        dump_interval_ms=e?strtoull(e,NULL,10):0;
        dump_next_ms=dump_after_ms;
    }
    const ULONGLONG dump_elapsed_ms=GetTickCount64()-dump_origin;
    /* LD_FRAME_DUMP_GATE=<file>: dump only while that file exists, once per
     * file (it is deleted after the dump): a player's hotkey creates it. */
    static int dump_gated = -1; static char dump_gate[MAX_PATH];
    if (dump_gated < 0) { const char* e = getenv("LD_FRAME_DUMP_GATE");
                          dump_gated = e && e[0]; if (dump_gated) snprintf(dump_gate, sizeof dump_gate, "%s", e); }
    const int dump_open = !dump_gated || GetFileAttributesA(dump_gate) != INVALID_FILE_ATTRIBUTES;
    const int dump_now = dump_every > 0 && dump_open && dump_elapsed_ms>=dump_after_ms &&
                         (!dump_interval_ms || dump_elapsed_ms>=dump_next_ms) &&
                         (long)g_ld_frames >= dump_from &&
                         (dump_n++ % (unsigned)dump_every) == 0;
    /* A flip names a registered display buffer. The current color target may
     * be an offscreen shadow/postprocess surface at that instant; copying it
     * caused a010 to present black despite executing the scene's draws. */
    u32 target = LD_INVALID_SURFACE;
    if (buffer_id < 8 && g.display_buffers[buffer_id].valid) {
        const display_buffer_t* display = &g.display_buffers[buffer_id];
        for (u32 i = 0; i < g.n_surfaces; i++) {
            if (g.surfaces[i].location == display->location &&
                g.surfaces[i].offset == display->offset) {
                target = i;
                break;
            }
        }
    }
    if (target == LD_INVALID_SURFACE && buffer_id < 8 &&
        g.display_buffers[buffer_id].valid && g.display_buffers[buffer_id].resolve_src > 0 &&
        (u32)g.display_buffers[buffer_id].resolve_src <= g.n_surfaces)
        target = (u32)g.display_buffers[buffer_id].resolve_src - 1;   /* see note_resolve */
    if (target == LD_INVALID_SURFACE) {
        target = current_surface();
        static u32 fallback_logs = 0;
        if (PS3_LOG_FIRST(fallback_logs, 32))
            fprintf(stderr,
                    "[live-draw] flip %u has no registered/rendered scanout; "
                    "falling back to current surface %u\n",
                    buffer_id, target);
    }
    if (target == LD_INVALID_SURFACE) {
        fprintf(stderr, "[live-draw] frame present skipped: no color surface\n");
        return;
    }
    g_ld_last_present_target = target;
    if (dump_now) {
        // Optional wall-clock cadence aligns idle/input comparisons across
        // different renderer rates. Retain phase and skip missed captures.
        if(dump_interval_ms)
            dump_next_ms=dump_after_ms+((dump_elapsed_ms-dump_after_ms)/dump_interval_ms+1)*dump_interval_ms;
        char path[MAX_PATH * 2];
        snprintf(path, sizeof(path), "%s\\frame_%06u.ppm", dump_dir, dump_n - 1u);
        const u64 nb = ld_dump_surface_ppm(path, &g.surfaces[target]);
        fprintf(stderr, "[frame-dump] %s %ux%u nonblack=%llu elapsed_ms=%llu tick_ms=%llu\n", path,
                ld_surface_physical(&g.surfaces[target], g.surfaces[target].w),
                ld_surface_physical(&g.surfaces[target], g.surfaces[target].h),
                (unsigned long long)(nb == UINT64_MAX ? 0 : nb),(unsigned long long)dump_elapsed_ms,
                (unsigned long long)GetTickCount64());
        /* Ultrawide: the dump is the canvas; say whether presentation adds the
         * black sides of a menu or movie frame (claude-tools/ultrawide). */
        if (g_ld_aspect_k != 1.0f)
            fprintf(stderr, "[aspect-dump] %s bars=%d\n", path,
                    ld_aspect_bars());
        for (u32 i = 0; i < g_ld_aspect_npieces; i++) {
            const ld_aspect_piece* p = &g_ld_aspect_pieces[i];
            fprintf(stderr, "[aspect-piece] %u kind=%u tex=%016llx %ux%u fmt=%02X opaque=%u vp=%08X "
                    "box=%.0f,%.0f,%.0f,%.0f pso=%016llx\n", i, p->kind, (unsigned long long)p->tex, p->tw, p->th,
                    p->fmt, p->opaque, p->vp, p->box[0], p->box[1], p->box[2], p->box[3],
                    (unsigned long long)p->pso);
        }
        if (dump_gated) DeleteFileA(dump_gate);
        fflush(stderr);
    }
    /* LD_RECORD=<out.mp4> (boot_main's --record): every presented frame, as
     * PPM, into an ffmpeg pipe -- ffmpeg's image2pipe reads concatenated PPMs
     * as a stream. Fragmented MP4 so a run that is killed rather than exiting
     * still leaves a playable file. Frames are stamped at a fixed 30 fps, so
     * playback speed is only right when the title presents at 30.
     * ponytail: one blocking GPU readback per frame; a ring of readback
     * buffers if recording ever has to run at full speed. */
    { static FILE* rec = (FILE*)-1;
      if (rec == (FILE*)-1) {
          const char* out = getenv("LD_RECORD");
          rec = NULL;
          if (out && out[0]) {
              char cmd[MAX_PATH * 2];
              snprintf(cmd, sizeof(cmd),
                       "ffmpeg -loglevel error -y -f image2pipe -framerate 30 -c:v ppm -i - "
                       "-vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\" -pix_fmt yuv420p "
                       "-movflags frag_keyframe+empty_moov \"%s\"", out);
              rec = _popen(cmd, "wb");
              fprintf(stderr, "[record] %s -> %s\n", rec ? "ffmpeg" : "FAILED to start ffmpeg", out);
          }
      }
      if (rec) {
          g_ld_record_to = rec;
          ld_dump_surface_ppm(NULL, &g.surfaces[target]);
          g_ld_record_to = NULL;
          fflush(rec);
      } }
    const u32 current = current_surface();
    surface_t* presented_surface = &g.surfaces[target];
    /* LD_PRESENT_DBG=1: is this surface actually finished? A surface whose last
     * clear is NEWER than its last draw has been wiped and not redrawn -- that
     * presents as a black frame. Counting them separates "we present the wrong
     * buffer" from "we present the right buffer too early". */
    { static int dbg = -1;
      if (dbg < 0) dbg = getenv("LD_PRESENT_DBG") ? 1 : 0;
      if (dbg) {
          static unsigned long long n = 0, blank = 0;
          n++;
          if (presented_surface->last_draw_generation <
              presented_surface->last_clear_generation) blank++;
          if ((n % 256) == 0)
              /* Also say WHICH surface. Surfaces are now keyed on size as well
               * as offset, so several exist at once and "we present the wrong
               * one" is a live possibility -- the PS1 composite is 720x512 and
               * the PS3 output 1280x720. */
              fprintf(stderr,
                      "[present] %llu/%llu presents showed a surface cleared "
                      "after its last draw (%.1f%%); target=%u %u:0x%X %ux%u"
                      " draw_gen=%u clear_gen=%u\n",
                      (unsigned long long)blank, (unsigned long long)n,
                      100.0 * (double)blank / (double)n,
                      target, presented_surface->location,
                      presented_surface->offset,
                      presented_surface->w, presented_surface->h,
                      presented_surface->last_draw_generation,
                      presented_surface->last_clear_generation);
      } }
#if !defined(YZ_PERF_CLEAN)
    presented_surface->last_present_copy_generation =
        ++g_ld_present_copy_generation;
    yz_frontier_trace_emit(
        YZ_FT_PARITY_FLIP, buffer_id, target,
        (u32)g_ld_flip_requested,
        presented_surface->last_draw_generation,
        presented_surface->last_clear_generation,
        g_ld_fifo_get, g_ld_fifo_put, current);
    yz_frontier_trace_emit(
        YZ_FT_PARITY_SURFACE, target, presented_surface->last_write_kind,
        presented_surface->last_write_generation,
        presented_surface->last_draw_generation,
        presented_surface->last_clear_generation,
        presented_surface->last_copy_generation,
        presented_surface->last_blit_generation,
        presented_surface->last_resolve_generation);
    yz_frontier_trace_emit(
        YZ_FT_PARITY_SURFACE_AUX, target,
        presented_surface->resource_serial,
        presented_surface->last_create_generation,
        presented_surface->last_other_generation,
        presented_surface->last_guest_blit_generation,
        presented_surface->offset, presented_surface->location,
        presented_surface->last_present_copy_generation);
    yz_frontier_trace_emit(
        YZ_FT_PARITY_RENDER, target, current,
        (u32)g_ld_stats.groups_executed,
        (u32)g_ld_stats.group_drop_ring,
        g.frame->cb_used, g.frame->vb_used, g.frame->srv_ring_used,
        presented_surface->last_present_copy_generation);
#endif
    { static u32 last_present_target = LD_INVALID_SURFACE;
      if (target != last_present_target) {
          ld_trace_target("present", target, buffer_id);
          last_present_target = target;
      } }
    ID3D12Resource* srcimg = presented_surface->tex;
    const u32 bbi = g.swap->lpVtbl->GetCurrentBackBufferIndex(g.swap);
    ID3D12Resource* bb = g.backbuf[bbi];

    D3D12_RESOURCE_DESC source_desc, display_desc;
    srcimg->lpVtbl->GetDesc(srcimg, &source_desc);
    bb->lpVtbl->GetDesc(bb, &display_desc);
    if (source_desc.Width != display_desc.Width || source_desc.Height != display_desc.Height ||
        presented_surface->fmt != DXGI_FORMAT_R8G8B8A8_UNORM) {
        float uv[4] = {0, 0, 1, 1};
        u32 x = 0, width = (u32)display_desc.Width;
        D3D12_RESOURCE_STATES bb_state = D3D12_RESOURCE_STATE_PRESENT;
        D3D12_RESOURCE_BARRIER bb_bar = {0};
        /* Ultrawide: a menu frame or the game's movie gets black sides
         * (ld_aspect_place) instead of whatever 3D lies behind it: only the
         * centre of the canvas, the 16:9 picture, goes onto a black frame. */
        if (g_ld_aspect_k != 1.0f &&
            ld_aspect_bars()) {
            const float half = 0.5f / g_ld_aspect_k;
            uv[0] = 0.5f - half; uv[2] = 0.5f + half;
            width = (u32)(display_desc.Width / g_ld_aspect_k + 0.5f);
            x = ((u32)display_desc.Width - width) / 2;
            bb_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bb_bar.Transition.pResource = bb;
            bb_bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bb_bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            bb_bar.Transition.StateAfter = bb_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            rsx_rec_ResourceBarrier(g.frame, 1, &bb_bar);
            const FLOAT black[4] = {0, 0, 0, 1};
            rsx_rec_ClearRenderTargetView(g.frame, rtv_handle(bbi), black, 0, NULL);
        }
        if (!ld_transfer_image(srcimg, SRV_SURFACE_BASE + target,
            D3D12_RESOURCE_STATE_RENDER_TARGET, bb, rtv_handle(bbi),
            bb_state, DXGI_FORMAT_R8G8B8A8_UNORM,
            uv, x, 0, width, display_desc.Height)) {
            fprintf(stderr, "[image-transfer] presentation composition failed\n");
            g.ready = 0;
            return;
        }
        if (bb_state != D3D12_RESOURCE_STATE_PRESENT) {
            bb_bar.Transition.StateBefore = bb_state;
            bb_bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            rsx_rec_ResourceBarrier(g.frame, 1, &bb_bar);
        }
    } else {
    D3D12_RESOURCE_BARRIER bar[2] = {0};
    bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[0].Transition.pResource = srcimg;
    bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[1].Transition.pResource = bb;
    bar[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    rsx_rec_ResourceBarrier(g.frame, 2, bar);

    rsx_rec_CopyResource(g.frame, bb, srcimg);

    bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    rsx_rec_ResourceBarrier(g.frame, 2, bar);

    }

    ld_flush(LD_FLUSH_PRESENT);
    if(!g.ready)return;
    {
        const HRESULT present_hr = ld_present_swapchain();
        if (SUCCEEDED(present_hr))
            ld_present_measure_record(g_ld_frames + 1u);
        else {
            fprintf(stderr,
                    "[d3d-fail] Present hr=0x%08lX frame=%u "
                    "target=%u size=%ux%u window=%ux%u\n",
                    (unsigned long)present_hr, g_ld_frames,
                    target, g.surfaces[target].w, g.surfaces[target].h,
                    g.width, g.height);
            ld_dump_dred("Present", present_hr);
            g.ready = 0;
        }
    }
#if !defined(YZ_PERF_CLEAN)
    if (InterlockedCompareExchange(
            &g_ld_diag_post_movie_pending, 0, 0) != 0) {
        const LONG post_movie_present =
            InterlockedIncrement(&g_ld_diag_post_movie_presents);
        if (post_movie_present == 32 &&
            InterlockedCompareExchange(
                &g_ld_diag_post_movie_pending, 0, 1) == 1) {
            char path[MAX_PATH * 2];
            snprintf(
                path, sizeof(path),
                "%s\\semantic_checkpoint_surface.ppm",
                g.vertex_diag_dir);
            const u64 nonblack =
                ld_dump_surface_ppm(path, &g.surfaces[target]);
            fprintf(stderr,
                    "[rsx-vertex-surface] reason=post-movie-32 "
                    "mode=%s target=%u pixels=%llu "
                    "nonblack=%s%llu path=%s\n",
                    ld_vertex_mode_name(), target,
                    (unsigned long long)g.surfaces[target].w *
                        g.surfaces[target].h,
                    nonblack == UINT64_MAX ? "unknown:" : "",
                    (unsigned long long)(
                        nonblack == UINT64_MAX ? 0 : nonblack),
                    path);
            fflush(stderr);
        }
    }
#endif

    { static unsigned long long packets_at_last_frame = 0;
      g_ld_last_frame_draws = (u32)(g_ld_stats.packets_seen - packets_at_last_frame);
      packets_at_last_frame = g_ld_stats.packets_seen; }
    /* LD_ASPECT_DIAG=1: every 300 presents, how the last frame's draws were
     * placed (ld_aspect_place). Text follows the last panel of its frame only. */
    { static int diag = -1;
      if (diag < 0) diag = getenv("LD_ASPECT_DIAG") != NULL;
      if (diag && g_ld_aspect_k != 1.0f && g_ld_frames % 300 == 0) {
          const u32* c = g_ld_aspect_counts;
          const int s = g_ld_aspect_screen >= 0 ? g_ld_aspect_screen : g_ld_aspect_screen_prev;
          fprintf(stderr, "[aspect] frame=%u stretch=%u centre=%u left=%u right=%u cover=%u pillar=%u text=%u "
                  "menu=%d screen=%s\n", g_ld_frames, c[0], c[1], c[2], c[3], c[4], c[5], c[6],
                  ld_aspect_grid_half(), s >= 0 ? rsx_aspect_screens[s].name : "-");
      }
      memset(g_ld_aspect_counts, 0, sizeof(g_ld_aspect_counts));
      g_ld_aspect_menu = ld_aspect_grid_half();
      const u32 g_ld_aspect_3d_log = g_ld_aspect_3d;
      g_ld_aspect_native = 0; g_ld_aspect_3d = 0;
      memset(g_ld_aspect_grid, 0, sizeof(g_ld_aspect_grid));
      if (g_ld_aspect_screen >= 0) { g_ld_aspect_screen_prev = g_ld_aspect_screen; g_ld_aspect_sticky = 0; }
      else if (g_ld_aspect_newbg || ++g_ld_aspect_sticky > 30) g_ld_aspect_screen_prev = -1;
      g_ld_aspect_newbg = 0;
      memcpy(g_ld_aspect_cover_tex[1], g_ld_aspect_cover_tex[0], sizeof(g_ld_aspect_cover_tex[0]));
      memset(g_ld_aspect_cover_tex[0], 0, sizeof(g_ld_aspect_cover_tex[0]));
      memcpy(g_ld_fig_tex[1], g_ld_fig_tex[0], sizeof(g_ld_fig_tex[0]));
      memset(g_ld_fig_tex[0], 0, sizeof(g_ld_fig_tex[0]));
      if (g_ld_fig_hi >= g_ld_fig_lo) g_ld_fig_cx = (g_ld_fig_lo + g_ld_fig_hi) * 0.5f;
      if (g_ld_fig_anchored) g_ld_fig_cx0 = g_ld_fig_cx;
      { const ULONGLONG now_ms = GetTickCount64();
        const float dt = (float)(now_ms - g_ld_fig_tick);
        g_ld_fig_tick = now_ms;
        if (g_ld_fig_anchored) g_ld_fig_ease = 1.0f;
        else {
            const float span = g_ld_fig_cx0 - 640.0f;
            float target = span > 1.0f ? (g_ld_fig_cx - 640.0f) / span : 0.0f;
            target = target < 0.0f ? 0.0f : target;
            const float floor_now = g_ld_fig_ease - dt / 700.0f;   /* at most the whole zoom in 0.7 s */
            if (target < g_ld_fig_ease) g_ld_fig_ease = target > floor_now ? target : floor_now;
        } }
      if (!g_ld_fig_tex[1][0] && !g_ld_fig_anchored) g_ld_fig_place = -1;
      g_ld_fig_anchored = 0; g_ld_fig_lo = 1e30f; g_ld_fig_hi = -1e30f;
      { static int framelog = -1;
        if (framelog < 0) framelog = getenv("LD_ASPECT_FRAMELOG") != NULL;
        if (framelog && g_ld_aspect_k != 1.0f) {
            fprintf(stderr, "[aspect-frame] %u screen=%s first=%s early=%u menu=%d video=%d fig=%d 3d=%u cx=%.0f ease=%.3f", g_ld_frames,
                    g_ld_aspect_screen >= 0 ? rsx_aspect_screens[g_ld_aspect_screen].name : "-",
                    g_ld_aspect_first >= 0 ? rsx_aspect_screens[g_ld_aspect_first].name : "-",
                    g_ld_aspect_early, g_ld_aspect_menu, g_ld_aspect_video, g_ld_fig_place, g_ld_aspect_3d_log,
                    g_ld_fig_cx, g_ld_fig_ease);
            for (int i = 0; i < 6 && g_ld_aspect_early_tex[i]; i++)   /* the unrecognised textures */
                fprintf(stderr, " %016llx/%u", (unsigned long long)g_ld_aspect_early_tex[i], g_ld_aspect_early_h[i]);
            fputc('\n', stderr);
        }
        g_ld_aspect_early = 0; g_ld_aspect_first = -2;
        memset(g_ld_aspect_early_tex, 0, sizeof(g_ld_aspect_early_tex)); }
      g_ld_aspect_screen = -1;
      g_ld_aspect_video = 0;
      g_ld_aspect_npieces = 0;
      g_ld_aspect_container = LD_ASPECT_CENTRE;
      g_ld_aspect_run = -1;
      { static int gated = -1; static char gate[MAX_PATH];
        if (gated < 0) { const char* e = getenv("LD_ASPECT_OFF_GATE"); gated = e && e[0];
                         if (gated) snprintf(gate, sizeof gate, "%s", e); }
        g_ld_aspect_off = gated && GetFileAttributesA(gate) != INVALID_FILE_ATTRIBUTES; } }
    g_ld_frames++;
#if defined(YZ_PERF_PROFILE)
    ld_profile_present(g_ld_frames);
#endif
#if !defined(YZ_PERF_CLEAN)
    /* Autonomous parity capture.  Scalar records remain circular and cheap.
     * Readback begins only after two alternating display surfaces stop
     * receiving writes while FIFO progress continues, and again for the first
     * 16 held-movement presentations.  The latter continues counting to 120
     * without readback so validation does not materially alter movement. */
    {
        static int parity_enabled = -1;
        static char parity_dir[MAX_PATH * 2];
        static u32 observed_write[MAX_SURFACES];
        static u8 observed_valid[MAX_SURFACES];
        static u32 prior_target = LD_INVALID_SURFACE;
        static u32 prior_get;
        static u32 prior_put;
        static u32 dialogue_stable;
        static u64 dialogue_targets;
        static int dialogue_armed;
        static u32 dialogue_captures;
        static u32 movement_frames;
        static u32 movement_captures;
        if (parity_enabled < 0) {
            const char* enabled = getenv("RSX_PARITY_DIAG");
            const char* directory = getenv("RSX_VALIDATION_DIR");
            parity_enabled = enabled && *enabled && directory && *directory;
            if (parity_enabled) {
                strncpy(parity_dir, directory, sizeof(parity_dir) - 1u);
                parity_dir[sizeof(parity_dir) - 1u] = '\0';
            }
        }
        if (parity_enabled) {
            const LONG movement_phase = InterlockedCompareExchange(
                &g_yz_movement_proof_phase, 0, 0);
            const u64 elapsed = g_yz_auto_start_tick
                ? GetTickCount64() - g_yz_auto_start_tick : 0;
            surface_t* surface = &g.surfaces[target];
            const int fifo_progress =
                g_ld_fifo_get != prior_get || g_ld_fifo_put != prior_put;
            const int alternating =
                prior_target != LD_INVALID_SURFACE && prior_target != target;
            const int unchanged = observed_valid[target] &&
                observed_write[target] == surface->last_write_generation;

            if (!dialogue_armed && !dialogue_captures &&
                movement_phase == 0 &&
                ((g_ld_frame_vertex_constant_ring_recycles != 0 &&
                  elapsed >= 300000ull) || elapsed >= 420000ull)) {
                if (g_ld_frame_vertex_constant_ring_recycles != 0) {
                    dialogue_armed = 1;
                    fprintf(
                        stderr,
                        "[parity-diag] DIALOGUE HIGH-DRAW TRIGGER "
                        "elapsed_ms=%llu frame_recycles=%u total_recycles=%llu "
                        "drop_ring=%llu frame=%u flip=%llu\n",
                        (unsigned long long)elapsed,
                        g_ld_frame_vertex_constant_ring_recycles,
                        (unsigned long long)
                            g_ld_vertex_constant_ring_recycles,
                        (unsigned long long)g_ld_stats.group_drop_ring,
                        g_ld_frames,
                        (unsigned long long)g_ld_flip_requested);
                    fflush(stderr);
                } else if (fifo_progress && alternating && unchanged) {
                    dialogue_stable++;
                    if (target < 64u)
                        dialogue_targets |= 1ull << target;
                } else if (fifo_progress) {
                    dialogue_stable = 0;
                    dialogue_targets = target < 64u ? 1ull << target : 0;
                }
                if (dialogue_stable >= 12u &&
                    dialogue_targets &&
                    (dialogue_targets & (dialogue_targets - 1u)) != 0) {
                    dialogue_armed = 1;
                    fprintf(
                        stderr,
                        "[parity-diag] DIALOGUE TRIGGER elapsed_ms=%llu "
                        "stable=%u targets=%016llX drop_ring=%llu "
                        "frame=%u flip=%llu\n",
                        (unsigned long long)elapsed, dialogue_stable,
                        (unsigned long long)dialogue_targets,
                        (unsigned long long)g_ld_stats.group_drop_ring,
                        g_ld_frames,
                        (unsigned long long)g_ld_flip_requested);
                    fflush(stderr);
                }
            }
            if (dialogue_armed && dialogue_captures < 16u) {
                const u32 capture = ++dialogue_captures;
                ld_parity_capture_surface(
                    parity_dir, "dialogue", capture, buffer_id, target);
                if (capture == 16u) {
                    dialogue_armed = 0;
                    yz_frontier_trace_dump(5u);
                    fprintf(
                        stderr,
                        "[parity-diag] DIALOGUE COMPLETE captures=16 "
                        "frame=%u flip=%llu\n",
                        g_ld_frames,
                        (unsigned long long)g_ld_flip_requested);
                    fflush(stderr);
                }
            }

            if (movement_phase == 4) {
                movement_frames++;
                if (movement_captures < 16u) {
                    const u32 capture = ++movement_captures;
                    ld_parity_capture_surface(
                        parity_dir, "movement", capture, buffer_id, target);
                    if (capture == 16u)
                        yz_frontier_trace_dump(6u);
                }
                if (movement_frames == 120u) {
                    yz_frontier_trace_dump(7u);
                    fprintf(
                        stderr,
                        "[parity-diag] MOVEMENT 120 COMPLETE "
                        "captures=16 frame=%u flip=%llu groups=%llu "
                        "drop_ring=%llu\n",
                        g_ld_frames,
                        (unsigned long long)g_ld_flip_requested,
                        (unsigned long long)g_ld_stats.groups_executed,
                        (unsigned long long)g_ld_stats.group_drop_ring);
                    fflush(stderr);
                }
            }

            observed_write[target] = surface->last_write_generation;
            observed_valid[target] = 1;
            prior_target = target;
            prior_get = g_ld_fifo_get;
            prior_put = g_ld_fifo_put;
        }
    }
    /* Capture the actual presented surface at acknowledged input boundaries.
     * This is independent of desktop visibility/focus and is inert unless the
     * promotion controller explicitly enables RSX_MOVEMENT_PROOF. */
    {
        static int movement_capture = -1;
        static char movement_dir[MAX_PATH * 2];
        if (movement_capture < 0) {
            const char* enabled = getenv("RSX_MOVEMENT_PROOF");
            const char* directory = getenv("RSX_VALIDATION_DIR");
            movement_capture = enabled && directory && *directory;
            if (movement_capture) {
                strncpy(movement_dir, directory,
                        sizeof(movement_dir) - 1u);
                movement_dir[sizeof(movement_dir) - 1u] = '\0';
            }
        }
        if (movement_capture) {
            const LONG phase = InterlockedCompareExchange(
                &g_yz_movement_proof_phase, 0, 0);
            static u64 next_probe_tick;
            static u32 probe_serial;
            static u64 probe_delay_ms;
            static u64 probe_interval_ms;
            static int probe_configured;

            if (!probe_configured) {
                const char* delay = getenv("RSX_MOVEMENT_PROOF_DELAY_MS");
                const char* interval = getenv("RSX_MOVEMENT_PROBE_INTERVAL_MS");
                probe_delay_ms = delay && *delay
                    ? _strtoui64(delay, NULL, 10) : 780000ull;
                probe_interval_ms = interval && *interval
                    ? _strtoui64(interval, NULL, 10) : 0ull;
                if (probe_interval_ms && probe_interval_ms < 30000ull)
                    probe_interval_ms = 30000ull;
                probe_configured = 1;
            }

            /* A time threshold alone cannot distinguish authored camera work
             * from playable Akiyama.  In visual-arm mode, retain occasional
             * renderer-surface probes while dialogue input continues.  The
             * proof is armed only after one of these images visibly shows the
             * gameplay HUD, avoiding timing-dependent false positives. */
            if (phase == 0 && probe_interval_ms && g_yz_auto_start_tick) {
                const u64 now = GetTickCount64();
                const u64 elapsed = now - g_yz_auto_start_tick;
                if (elapsed >= probe_delay_ms &&
                    (!next_probe_tick || now >= next_probe_tick)) {
                    char path[MAX_PATH * 2];
                    const u32 serial = ++probe_serial;
                    snprintf(path, sizeof(path),
                             "%s\\frontier_probe_%03u.ppm",
                             movement_dir, serial);
                    {
                        const u64 nonblack =
                            ld_dump_surface_ppm(path, &g.surfaces[target]);
                        fprintf(stderr,
                                "[movement-proof] frontier-probe serial=%u "
                                "elapsed_ms=%llu frame=%u nonblack=%s%llu "
                                "path=%s\n",
                                serial, (unsigned long long)elapsed,
                                g_ld_frames,
                                nonblack == UINT64_MAX ? "unknown:" : "",
                                (unsigned long long)(
                                    nonblack == UINT64_MAX ? 0 : nonblack),
                                path);
                        fflush(stderr);
                    }
                    next_probe_tick = now + probe_interval_ms;
                }
            }
            const char* name = NULL;
            LONG next_phase = phase;
            if (phase == 1) {
                name = "movement_before.ppm";
                next_phase = 2;
            } else if (phase == 3) {
                name = "camera_after.ppm";
                next_phase = 4;
            } else if (phase == 5) {
                name = "movement_after_60s.ppm";
                next_phase = 6;
            }
            if (name) {
                char path[MAX_PATH * 2];
                snprintf(path, sizeof(path), "%s\\%s", movement_dir, name);
                ld_movement_camera_snapshot(phase);
                {
                    const u64 nonblack =
                        ld_dump_surface_ppm(path, &g.surfaces[target]);
                    fprintf(stderr,
                            "[movement-proof] capture phase=%ld frame=%u "
                            "flip=%llu nonblack=%s%llu path=%s\n",
                            phase, g_ld_frames,
                            (unsigned long long)g_ld_flip_requested,
                            nonblack == UINT64_MAX ? "unknown:" : "",
                            (unsigned long long)(
                                nonblack == UINT64_MAX ? 0 : nonblack),
                            path);
                    fflush(stderr);
                }
                InterlockedCompareExchange(
                    &g_yz_movement_proof_phase, next_phase, phase);
                if (next_phase == 6) {
                    fprintf(stderr,
                            "[movement-proof] COMPLETE sustained_ms=60000 "
                            "camera_ms=5000 frame=%u\n", g_ld_frames);
                    fflush(stderr);
                }
            }
        }
    }
    /* Narrow validation readback for the four user-visible orphanage shots.
     * This captures the renderer's actual presented surface and therefore is
     * independent of desktop focus, window visibility, and screenshot APIs.
     * It is inert unless the validation controller supplies both variables. */
    {
        static int validation_capture = -1;
        static u32 validation_dumped = 0;
        static u64 validation_origin = UINT64_MAX;
        static char validation_dir[MAX_PATH * 2];
        if (validation_capture < 0) {
            const char* enabled = getenv("RSX_VALIDATION_CAPTURE");
            const char* directory = getenv("RSX_VALIDATION_DIR");
            validation_capture = enabled && directory && *directory;
            if (validation_capture) {
                strncpy(validation_dir, directory,
                        sizeof(validation_dir) - 1u);
                validation_dir[sizeof(validation_dir) - 1u] = '\0';
            }
        }
        /* The a010 file-lifetime gate is the authoritative scene boundary in
         * clean production builds.  The older exact-mesh gate was useful for
         * a geometry experiment but is not guaranteed to occur in every
         * authored camera block, so using it for validation can miss the
         * entire scene even while valid draws are being presented. */
        if (validation_capture && validation_origin == UINT64_MAX &&
            InterlockedCompareExchange(&g_yz_a010_root_active, 0, 0) != 0) {
            validation_origin = g_ld_flip_requested;
            fprintf(stderr,
                    "[validation-capture] a010 origin flip=%llu frame=%u\n",
                    (unsigned long long)validation_origin, g_ld_frames);
        }
        if (validation_capture && validation_origin != UINT64_MAX) {
            /* Scene-open itself is a 43-flip black/fade interval.  Preserve
             * the original four-shot spacing after that measured lead-in so
             * the promotion run lands on the outdoor ground, children,
             * over-shoulder hair, and face close-up rather than the fade,
             * sink, and pre-pickup phone shots. */
            static const u64 offsets[] = {43, 128, 302, 316};
            static const char* labels[] = {
                "background-ground", "children-eyes", "hair", "face-eyes"
            };
            for (u32 i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
                const u64 requested = validation_origin + offsets[i];
                if (!(validation_dumped & (1u << i)) &&
                    g_ld_flip_requested >= requested) {
                    char path[MAX_PATH * 2];
                    snprintf(path, sizeof(path),
                             "%s\\orphanage_gpu_%s_flip_%llu.ppm",
                             validation_dir, labels[i],
                             (unsigned long long)requested);
                    const u64 nonblack =
                        ld_dump_surface_ppm(path, &g.surfaces[target]);
                    fprintf(stderr,
                            "[validation-capture] label=%s requested_flip=%llu "
                            "actual_flip=%llu frame=%u target=%u "
                            "nonblack=%s%llu path=%s\n",
                            labels[i], (unsigned long long)requested,
                            (unsigned long long)g_ld_flip_requested,
                            g_ld_frames, target,
                            nonblack == UINT64_MAX ? "unknown:" : "",
                            (unsigned long long)(
                                nonblack == UINT64_MAX ? 0 : nonblack),
                            path);
                    validation_dumped |= 1u << i;
                }
            }
        }
    }
#endif
    /* First 32 frames verbatim, then every 32nd: keeps the log bounded while
     * making the TRUE frame count measurable from the log. (The old hard cap
     * at 32 made "stalls at frame ~32" unfalsifiable from the .err alone.) */
    if (g_ld_frames <= 32 || (g_ld_frames & 31) == 0)
        fprintf(stderr,
                "[live-draw] frame %u packets[seen=%llu queued=%llu movie=%llu qfull=%llu] "
                "groups[seen=%llu exec=%llu empty=%llu drop{fetch=%llu degen=%llu prim=%llu "
                "alloc=%llu pso=%llu ring=%llu surface=%llu}] clears[guest=%llu badsurf=%llu implicitZ=%llu] "
                "textures[cached=%u/%u full=%llu decodefail=%llu depthSRV=%llu rejectZ=%llu] "
                "vtex[cached=%u enabled=%llu binds=%llu uploads=%llu refresh=%llu "
                "unsupported=%llu missingTXL=%llu] divider=%llu "
                "balance[p=%s g=%s] binds[white=%llu real=%llu surf=%llu] (cumulative)\n",
                g_ld_frames,
                g_ld_stats.packets_seen, g_ld_stats.packets_queued,
                g_ld_stats.packets_movie, g_ld_stats.packets_queue_full,
                g_ld_stats.groups_seen, g_ld_stats.groups_executed,
                g_ld_stats.groups_empty, g_ld_stats.group_drop_fetch,
                g_ld_stats.group_drop_degenerate, g_ld_stats.group_drop_primitive,
                g_ld_stats.group_drop_alloc, g_ld_stats.group_drop_pso,
                g_ld_stats.group_drop_ring, g_ld_stats.group_drop_surface,
                g_ld_stats.clears, g_ld_stats.clear_drop_surface,
                g_ld_stats.implicit_depth_clears,
                g.n_textures, MAX_TEXTURES,
                (unsigned long long)g_ld_texture_cache_full,
                (unsigned long long)g_ld_texture_decode_fail,
                (unsigned long long)g_ld_zdepth_srv_binds,
                (unsigned long long)g_ld_zdepth_srv_reject_no_write,
                g.n_vtex,
                (unsigned long long)g_ld_vtex_enabled,
                (unsigned long long)g_ld_vtex_binds,
                (unsigned long long)g_ld_vtex_uploads,
                (unsigned long long)g_ld_vtex_refreshes,
                (unsigned long long)g_ld_vtex_unsupported,
                (unsigned long long)g_ld_vtex_missing_for_txl,
                (unsigned long long)g_ld_divider_fetches,
                g_ld_stats.packets_seen == g_ld_stats.packets_queued +
                    g_ld_stats.packets_movie + g_ld_stats.packets_queue_full ? "ok" : "BAD",
                g_ld_stats.groups_seen == ld_groups_accounted() ? "ok" : "BAD",
                g_ld_bind_white, g_ld_bind_real, g_ld_bind_surf);
    /* [fps] heartbeat: direct frame-rate logging, one line per ~5s wall.
     * Exists because pace repeatedly had to be inferred from frame-counter
     * arithmetic and log ordering, and two such inferences were wrong in one
     * night (s42). Volume-bounded per LESSONS #6c. */
    { static ULONGLONG fps_t0 = 0; static u32 fps_f0 = 0;
      ULONGLONG now = GetTickCount64();
      if (fps_t0 == 0) { fps_t0 = now; fps_f0 = g_ld_frames; }
      else if (now - fps_t0 >= 5000) {
          /* PS1_PC=1: the R3000 state block (0x76C080) on the same heartbeat.
           *
           * READ +0x124 (instructions retired), NOT +0x108. The interpreter
           * loads PC from +0x108 on entry (lwz r26, 0x108(r23) at 0x001066CC)
           * and writes it back only in its epilogue (stw r26, 0x108(r23) at
           * 0x00106964) -- and it has not returned, because it is entered once
           * and loops internally. So +0x108 is stale from boot: 280,000 reads
           * across 14 samples gave 0xBFC00000 every time while +0x124 climbed
           * past a billion. Reading that as "stuck at the reset vector" would
           * be wrong twice over -- the epilogue does `ori r26, r26, 0x80`
           * first, so a real exit there could not even write 0xBFC00000.
           *
           * +0x124 is updated from inside the loop and is the honest progress
           * signal. Finding the LIVE pc needs the interpreter's register, not
           * this field. */
          { static int pc_on = -1;
            if (pc_on < 0) pc_on = getenv("PS1_PC") ? 1 : 0;
            if (pc_on) {
                extern uint32_t vm_read32(uint64_t);
                /* One read of +0x108 cannot tell a reset loop from a field that
                 * is simply not live between interpreter entries -- both look
                 * like a constant 0xBFC00000. So sample it hard: 20000 reads,
                 * and report how many DISTINCT values appear plus the lowest
                 * and highest. A live PC sweeps; a dead field does not. */
                uint32_t lo = 0xFFFFFFFFu, hi = 0, prev = 0, chg = 0;
                for (int k = 0; k < 20000; k++) {
                    uint32_t v = vm_read32(0x76C080u + 0x108u);
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                    if (k && v != prev) chg++;
                    prev = v;
                }
                /* The PS1 GPU command ring. func_0010F658 pushes one 0x100-byte
                 * packet per GP0 batch (header 3 at +0, payload from +0x10) and
                 * advances a write offset; the four GPU SPUs consume it. If this
                 * offset never moves, the R3000 is not producing draw commands
                 * at all, and no amount of SPU-side work will help.
                 *
                 *   ring_off_ptr = *(TOC-0x794C)   (TOC = 0x1C3D30)
                 *   ring_base    = *(TOC-0x7918)
                 * from func_0010F658's first two instructions. */
                uint32_t roff_p = vm_read32(0x1BC3E4u);
                uint32_t rbase  = vm_read32(0x1BC418u);
                uint32_t roff   = roff_p ? vm_read32(roff_p) : 0xFFFFFFFFu;
                /* Each GPU SPU polls its OWN local store at 0x15010 for that
                 * offset (it reports that address to the PPU through its
                 * outbound mailbox during init), and a raw SPU's local store is
                 * aliased into guest memory at 0xE0000000 + n*0x100000 -- so
                 * these four reads see exactly what the SPU sees. If they match
                 * `off`, the kick landed and the SPUs are simply not reacting;
                 * if they do not, the kick is being lost. Opposite problems. */
                /* +0x15010 is the offset the PPU publishes to this SPU;
                 * +0x15020 is how far the SPU has actually CONSUMED (its work
                 * loop at 0x3158 advances it by 0x100 per packet under the same
                 * 0x007FFFFF mask the PPU uses, and stores it back). The pair
                 * separates "the SPUs are not being told" from "the SPUs are
                 * told and not working" from "both are fine and the problem is
                 * downstream in compositing". */
                uint32_t ls[4], cons[4];
                for (int n = 0; n < 4; n++) {
                    uint32_t w = 0xE0000000u + (uint32_t)n * 0x100000u;
                    ls[n]   = vm_read32(w + 0x15010u);
                    cons[n] = vm_read32(w + 0x15020u);
                }
                /* The scheduler's own state. func_00105FA8 hands the
                 * interpreter a budget at +0x120; when that is permanently zero
                 * the interpreter spins without executing. These are the fields
                 * it works from, so a frozen run says WHICH state it froze in
                 * rather than just that it froze. */
                fprintf(stderr, "[ps1] pc=0x%08X(chg=%u) exited=%u total=%u"
                                " cost=%u budget=%d reason=%d evhead=0x%08X"
                                " ring[off=0x%08X] pub[%06X %06X %06X %06X]"
                                " done[%06X %06X %06X %06X]\n",

                        lo, chg,
                        vm_read32(0x76C080u + 0x110u),
                        vm_read32(0x76C080u + 0x124u),
                        vm_read32(0x76C080u + 0x114u),
                        (int32_t)vm_read32(0x76C080u + 0x120u),
                        (int32_t)vm_read32(0x76C080u + 0x138u),
                        vm_read32(0x76C080u + 0x540u),
                        roff, ls[0], ls[1], ls[2], ls[3],
                        cons[0], cons[1], cons[2], cons[3]);
                /* Does PS1 VRAM actually have pixels in it?
                 *
                 * The texture the PS3 side binds for the PS1 framebuffer is
                 * location 1 (RSX local), offset 0x400000, 1024x512 fmt 0xE2 --
                 * exactly PS1 VRAM. Everything upstream of here is now known
                 * good (the four GPU SPUs consume every packet the R3000
                 * produces), so this is the first place the picture can go
                 * missing: either the SPUs are not writing pixels, or they are
                 * and our compositing drops them. Count non-zero words rather
                 * than guessing from a window capture -- PrintWindow on a D3D12
                 * swapchain cannot tell "black" from "capture failed". */
                { const u8* fb = guest_ptr(1u, 0x400000u, 1024u * 512u * 2u);
                  /* The guest EA of PS1 VRAM, once. Watching 0xC0400000 --
                   * derived from cellGcmSys's localAddress -- caught no writes
                   * at all from either the PPU store path or SPU DMA, which is
                   * either a real finding or a wrong address. This says which,
                   * instead of assuming. */
                  { static int shown = 0;
                    extern uint8_t* vm_base;
                    if (!shown && fb && vm_base) { shown = 1;
                        fprintf(stderr, "[ps1] VRAM guest EA = 0x%08X\n",
                                (u32)(size_t)(fb - vm_base)); } }
                  if (!fb) fprintf(stderr, "[ps1] vram: not mapped\n");
                  else {
                      u32 nz = 0, first = 0xFFFFFFFFu, n = 1024u * 512u / 2u;
                      const u32* w32 = (const u32*)fb;
                      for (u32 i = 0; i < n; i++)
                          if (w32[i]) { nz++; if (first == 0xFFFFFFFFu) first = i * 4u; }
                      fprintf(stderr, "[ps1] vram nonzero=%u/%u first=+0x%X\n",
                              nz, n, first == 0xFFFFFFFFu ? 0u : first);
                      /* PS1_EVENTS=1: the three kernel events the BIOS polls.
                       *
                       * The BIOS parks at 0xBFC53840 calling TestEvent
                       * (B-table 0x0B, trampoline 0xBFC58B60) on three
                       * descriptors loaded from PS1 kernel RAM 0x0000B21C,
                       * 0x0000B224 and 0x0000B228. Reading their event CLASS
                       * says what it is waiting for; the PS1 CD class is
                       * 0xF0000003.
                       *
                       * PS1 RAM base is *(TOC-0x79D4) = 0x00770780, and the
                       * interpreter reads PS1 memory with lwbrx -- PS1 RAM is
                       * stored LITTLE-endian in guest memory, so every read
                       * here has to be byte-swapped. Reading it big-endian
                       * would produce plausible-looking nonsense. */
                      { static int ev = -1; static int ev_done = 0;
                        if (ev < 0) ev = getenv("PS1_EVENTS") ? 1 : 0;
                        static int ev_beats = 0;
                        if (ev && !ev_done && ++ev_beats >= 3) {
                            ev_done = 1;
                            const u32 ram = vm_read32(0x001BC35Cu);
                            #define PS1LE(a) __builtin_bswap32(vm_read32((ram) + (u32)(a)))
                            const u32 tot_evcb = PS1LE(0x0120);
                            const u32 tot_size = PS1LE(0x0124);
                            fprintf(stderr, "[ps1ev] ram=0x%08X  EvCB tbl=0x%08X size=%u\n",
                                    ram, tot_evcb, tot_size);
                            /* All FIVE slots CdInit fills, not just the three
                             * the poll loop reads: -0x4de8/-0x4de4/-0x4de0/
                             * -0x4ddc/-0x4dd8 = 0xB218..0xB228. All zero means
                             * CdInit never ran; a mix means it ran partway. */
                            static const u32 slot[5] = { 0xB218u, 0xB21Cu, 0xB220u,
                                                         0xB224u, 0xB228u };
                            for (int k = 0; k < 5; k++) {
                                const u32 h = PS1LE(slot[k]);
                                fprintf(stderr, "[ps1ev]   slot 0x%04X handle=0x%08X",
                                        slot[k], h);
                                if ((h >> 24) == 0xF1u && tot_evcb) {
                                    const u32 idx = h & 0xFFFFu;
                                    const u32 cb  = (tot_evcb & 0x1FFFFFu) + idx * 0x1Cu;
                                    fprintf(stderr, "  idx=%u class=0x%08X status=0x%08X"
                                                    " spec=0x%08X mode=0x%08X",
                                            idx, PS1LE(cb + 0), PS1LE(cb + 4),
                                            PS1LE(cb + 8), PS1LE(cb + 12));
                                }
                                fprintf(stderr, "\n");
                            }
                            /* Dump the whole EvCB table. The table pointer
                             * itself is valid (0xA000E028, 448 bytes = 16
                             * entries of 0x1C), so the PS1 kernel IS up and
                             * events CAN be opened. Whether any ARE distinguishes
                             * "CdInit specifically never ran" from "nothing ever
                             * opens an event", which are very different faults.
                             * EvCB layout: class +0, status +4, spec +8,
                             * mode +0xC, func +0x10. */
                            { const u32 tb = tot_evcb & 0x1FFFFFu;
                              const u32 n = tot_size / 0x1Cu;
                              u32 live = 0;
                              for (u32 i = 0; i < n && i < 32; i++) {
                                  const u32 cb = tb + i * 0x1Cu;
                                  const u32 cls = PS1LE(cb + 0);
                                  const u32 st  = PS1LE(cb + 4);
                                  if (!cls && !st) continue;
                                  live++;
                                  fprintf(stderr, "[ps1ev]   EvCB[%2u] class=0x%08X"
                                                  " status=0x%08X spec=0x%08X"
                                                  " mode=0x%08X func=0x%08X\n",
                                          i, cls, st, PS1LE(cb + 8),
                                          PS1LE(cb + 12), PS1LE(cb + 16));
                              }
                              fprintf(stderr, "[ps1ev]   %u of %u EvCB entries in use\n",
                                      live, n); }
                            #undef PS1LE
                        } }
                      /* PS1_GP0HIST=1: which GP0 PRIMITIVES the ring actually
                       * carries, as a histogram over the last 64 packets.
                       *
                       * "The menu draws but nothing 3D does" is a statement
                       * about primitive TYPES, and no counter here reported
                       * those -- packets, groups and VRAM pixels all count the
                       * same whether the PS1 is drawing a textured rectangle or
                       * a gouraud triangle. GP0 opcodes: 0x20-0x3F polygons,
                       * 0x40-0x5F lines, 0x60-0x7F rectangles, 0x80+ transfers,
                       * 0x00-0x1F misc/state. If the 3D is missing because the
                       * polygons never reach the ring, that is upstream in the
                       * R3000; if they reach it, it is the rasteriser or the
                       * composite. Opposite halves of the pipeline. */
                      { static int gh = -1;
                        if (gh < 0) gh = getenv("PS1_GP0HIST") ? 1 : 0;
                        if (gh && rbase && roff >= 0x4000u) {
                            unsigned cls[8] = {0}; unsigned pk_types[8] = {0};
                            for (u32 k = 1; k <= 64u; k++) {
                                const u32 pk = rbase + ((roff - k * 0x100u) & 0x007FFFFFu);
                                pk_types[vm_read32(pk) & 7u]++;
                                for (u32 q = 0; q < 60u; q++) {
                                    const u32 w = vm_read32(pk + 0x10u + q * 4u);
                                    if (!w) continue;
                                    cls[(w >> 24) >> 5]++;   /* opcode/0x20 */
                                }
                            }
                            /* PS1 VRAM comes out solid 0x83E0 -- R=0, G=31,
                             * B=0, mask bit set -- across the full 640-pixel
                             * display width, fully written (207 DMA writes per
                             * 64-byte block). Something paints the screen
                             * green. GP0 0x02 is Fill Rectangle and its colour
                             * word is right here in the ring, so print the
                             * fills: if the guest asks for green, the bug is
                             * upstream in the R3000; if it asks for something
                             * else, our SPU rasteriser is packing the colour
                             * wrong. Opposite halves of the pipeline. */
                            for (u32 k = 1; k <= 64u; k++) {
                                const u32 pk = rbase + ((roff - k * 0x100u) & 0x007FFFFFu);
                                for (u32 q = 0; q < 58u; q++) {
                                    const u32 w = vm_read32(pk + 0x10u + q * 4u);
                                    if ((w >> 24) != 0x02u) continue;
                                    fprintf(stderr, "[gp0fill] pkt+0x%X"
                                                    " cmd=%08X xy=%08X wh=%08X\n",
                                            0x10u + q * 4u, w,
                                            vm_read32(pk + 0x14u + q * 4u),
                                            vm_read32(pk + 0x18u + q * 4u));
                                    break;
                                }
                            }
                            fprintf(stderr, "[gp0hist] last 64 pkts types[");
                            for (int q = 0; q < 8; q++) fprintf(stderr, "%u ", pk_types[q]);
                            fprintf(stderr, "] words: misc=%u POLY=%u line=%u"
                                            " rect=%u xfer=%u %u %u %u\n",
                                    cls[0], cls[1], cls[2], cls[3],
                                    cls[4], cls[5], cls[6], cls[7]);
                        } }
                      /* PS1_RINGDUMP=1: the last few GP0 command packets.
                       *
                       * func_0010F658 writes one 0x100-byte packet per batch:
                       * a type word at +0 (it writes 3 in one variant and 2 in
                       * another) and the payload from +0x10. The four GPU SPUs
                       * consume every packet and never DMA a pixel to VRAM, so
                       * the question is what these packets actually contain --
                       * PS1 GP0 drawing commands, or only state and sync. */
                      { static int rd2 = -1; static int shown = 0;
                        if (rd2 < 0) rd2 = getenv("PS1_RINGDUMP") ? 1 : 0;
                        if (rd2 && shown < 3 && rbase && roff >= 0x300u) {
                            shown++;
                            for (int k = 3; k >= 1; k--) {
                                const u32 pk = rbase + ((roff - (u32)k * 0x100u) & 0x007FFFFFu);
                                fprintf(stderr, "[ring] pkt@0x%08X type=%u:", pk,
                                        vm_read32(pk));
                                for (u32 q = 0; q < 12; q++)
                                    fprintf(stderr, " %08X", vm_read32(pk + 0x10u + q * 4u));
                                fprintf(stderr, "\n");
                            }
                        } }
                      { static u32 rdy = 0;
                        if (!rdy) { const char* re = getenv("PS1_VRAM_READY");
                                    rdy = re ? (u32)strtoul(re, 0, 0) : 20000u; }
                        if (nz >= rdy) g_ld_ps1_vram_ready = 1; }
                      /* PS1_FBDUMP=<path>: write PS1 VRAM out as a PPM.
                       *
                       * This exists because "the window is black" was an
                       * ASSUMPTION for a long stretch of this port: PrintWindow
                       * on a D3D12 swapchain returns black whether the page is
                       * black or the capture simply failed, so every screenshot
                       * was unfalsifiable. This reads the pixels the PS1 itself
                       * produced, straight out of guest memory, with no D3D and
                       * no window involved. 1024x512, 16-bit 1-5-5-5, pitch
                       * 2048 -- the format the [tex-refresh] line reports. */
                      /* ONE dump, taken once the framebuffer has at least
                       * PS1_FBDUMP_MIN non-zero words (default 20000).
                       *
                       * It used to write a 1.5 MB PPM on every heartbeat, and
                       * that instrumentation was itself most of the
                       * "nondeterminism" this port was chasing: runs carrying
                       * the periodic dump reached 0-7,127 non-zero words, while
                       * clean runs reach 75,527 every time. Measuring the thing
                       * was changing it. One dump, gated on content. */
                      { const char* dp = getenv("PS1_FBDUMP");
                        static int dumped = 0;
                        u32 want = 20000;
                        { const char* mn = getenv("PS1_FBDUMP_MIN");
                          if (mn) want = (u32)strtoul(mn, 0, 0); }
                        if (dp && !dumped && nz >= want) {
                            dumped = 1;
                            char path[512];
                            snprintf(path, sizeof path, "%s", dp);
                            FILE* f = fopen(path, "wb");
                            if (f) {
                                fprintf(f, "P6\n1024 512\n255\n");
                                for (u32 y = 0; y < 512; y++) {
                                    const u8* row = fb + (size_t)y * 2048u;
                                    for (u32 x = 0; x < 1024; x++) {
                                        u16 p = (u16)(row[x * 2] | (row[x * 2 + 1] << 8));
                                        u8 rgb[3];
                                        rgb[0] = (u8)(((p >> 10) & 0x1F) * 255 / 31);
                                        rgb[1] = (u8)(((p >> 5)  & 0x1F) * 255 / 31);
                                        rgb[2] = (u8)(( p        & 0x1F) * 255 / 31);
                                        fwrite(rgb, 1, 3, f);
                                    }
                                }
                                fclose(f);
                            }
                        } }
                  } }
                (void)hi; (void)rbase;
            } }
          fprintf(stderr, "[fps] %.1f (frames %u..%u over %.1fs)\n",
                  (g_ld_frames - fps_f0) * 1000.0 / (double)(now - fps_t0),
                  fps_f0, g_ld_frames, (now - fps_t0) / 1000.0);
          fps_t0 = now; fps_f0 = g_ld_frames;
      } }
    /* RSX_DUMP_EVERY=N: also dump every Nth frame, not just the first 8 --
     * a title whose content starts after the boot clears is invisible otherwise. */
    static int ld_dump_every = -1;
    if (ld_dump_every < 0) { const char* e = getenv("RSX_DUMP_EVERY"); ld_dump_every = e ? atoi(e) : 0; }
    if (LD_DIAG_ENABLED("RSX_DUMP") &&
        (g_ld_frames <= 8 || (ld_dump_every > 0 && (g_ld_frames % (u32)ld_dump_every) == 0))) {
        /* Dump the presented color surface (RENDER_TARGET state -> safe). */
        const u32 cur = current_surface();
        if (cur != LD_INVALID_SURFACE) {
            char path[256];
            snprintf(path, sizeof(path), "scratch\\ld_frame_%02u.ppm", g_ld_frames);
            ld_dump_surface_ppm(path, &g.surfaces[cur]);
        }
    }
#if !defined(YZ_PERF_CLEAN)
    if (rsx_live_draw_a010_probe_active()) {
        const u32 elapsed = g_ld_frames - g_ld_a010_probe_start_frame;
        const int targeted = getenv("RSX_A010_SURFACE_DUMP") != NULL;
        u32 sample_every = 16u;
        const char *sample_every_env = getenv("RSX_A010_SURFACE_EVERY");
        if (sample_every_env) {
            const int parsed = atoi(sample_every_env);
            if (parsed > 0 && parsed <= 64)
                sample_every = (u32)parsed;
        }
        /* Loading consumes roughly 150 fast flips before a010. Sampling every
         * 16 frames spans the load and complete AUTH window while keeping the
         * synchronous readbacks from becoming the scene's clock. */
        const int world_ready =
            InterlockedCompareExchange(&g_ld_a010_world_ready, 0, 0) != 0;
        /* Targeted readbacks are intentionally expensive: each surface dump
         * submits and fences the open D3D12 list.  Do not perturb the producer
         * while it is still assembling the first a010 world command chain;
         * begin targeted capture only after a real scene mesh was observed. */
        const int capture_once =
            targeted && getenv("RSX_A010_CAPTURE_ONCE") != NULL;
        const int explicit_probe_fallback =
            capture_once && getenv("RSX_A010_PROBE") != NULL &&
            elapsed >= 192u;
        /* The exact healthy-mesh tuple is the strongest capture gate, but a
         * broken natural scene may never produce it. An explicitly requested
         * one-shot probe therefore gets one bounded fallback after 192 scene
         * flips, late enough to be past a010's loading/transition frames. */
        if ((!targeted || world_ready || explicit_probe_fallback) &&
            (elapsed % sample_every) == 0 &&
            (!targeted || elapsed <= 208u ||
             (capture_once && g_ld_a010_probe_sample == 0u))) {
            u64 mask = g_ld_a010_probe_touched;
            const u32 cur = current_surface();
            if (targeted) {
                mask = 0;
                for (u32 i = 0; i < g.n_surfaces && i < 64; i++) {
                    const u32 off = g.surfaces[i].offset;
                    /* Working a010 replay: 0xE40000 and 0x1800000 carry
                     * 1,481/1,727 scene draws; 0x2D10000 and 0x2710000 are
                     * the two depth-derived color passes; 0x1440000 is the
                     * final seven-draw composite. */
                    if (off == 0x00E40000u || off == 0x01800000u ||
                        off == 0x02D10000u || off == 0x02710000u ||
                        off == 0x01440000u ||
                        i == target)
                        mask |= 1ull << i;
                }
            } else if (cur < 64) {
                mask |= 1ull << cur;
            }
            fprintf(stderr,
                    "[a010-probe] SAMPLE n=%u live_frame=%u elapsed=%u "
                    "present=%u current=%u buffer=%u touched=0x%016llX "
                    "surfaces=%u targeted=%d\n",
                    g_ld_a010_probe_sample, g_ld_frames, elapsed, target, cur,
                    buffer_id, (unsigned long long)mask, g.n_surfaces,
                    targeted);
            for (u32 i = 0; i < g.n_surfaces && i < 64; i++) {
                if (!(mask & (1ull << i))) continue;
                char path[256];
                snprintf(path, sizeof(path),
                         "scratch\\a010_probe\\sample_%03u_frame_%05u_"
                         "surface_%02u_off_%08X.ppm",
                         g_ld_a010_probe_sample, g_ld_frames, i,
                         g.surfaces[i].offset);
                ld_dump_surface_ppm(path, &g.surfaces[i]);
                fprintf(stderr,
                        "[a010-probe] SURFACE sample=%u index=%u location=%u "
                        "offset=0x%08X size=%ux%u role=%s\n",
                        g_ld_a010_probe_sample, i, g.surfaces[i].location,
                        g.surfaces[i].offset, g.surfaces[i].w,
                        g.surfaces[i].h,
                        i == target ? "present" : "offscreen");
            }
            g_ld_a010_probe_touched = 0;
            g_ld_a010_probe_sample++;
            fflush(stderr);
            if (capture_once) {
                FILE* acceptance =
                    fopen("scratch\\a010_acceptance.txt", "a");
                if (acceptance) {
                    fprintf(acceptance,
                            "surface-capture sample=%u frame=%u mask=%016llX\n",
                            g_ld_a010_probe_sample - 1u, g_ld_frames,
                            (unsigned long long)mask);
                    fclose(acceptance);
                }
                InterlockedExchange(&g_ld_a010_probe_active, 0);
            }
        }
        if (elapsed >= 640 &&
            (!capture_once || g_ld_a010_probe_sample != 0u)) {
            fprintf(stderr,
                    "[a010-probe] END frame-cap live_frame=%u samples=%u\n",
                    g_ld_frames, g_ld_a010_probe_sample);
            fflush(stderr);
            InterlockedExchange(&g_ld_a010_probe_active, 0);
        }
    }
#endif

    /* new frame: reset per-frame ring cursors */
    g.frame->vb_used = 0; g.frame->ib_used = 0; g.frame->cb_used = 0; g.frame->ps_cb_used = 0;
    g.frame->srv_ring_used = 0; g.frame->smp_ring_used = 0;
#if !defined(YZ_PERF_CLEAN)
    g_ld_frame_vertex_constant_ring_recycles = 0;
#endif
    g.depth_cleared = 0;
    /* Per-zeta resources model persistent RSX memory.  Do not mark them
     * uncleared at a host-frame boundary: the implicit-clear branch would
     * erase depth rendered in an earlier frame and demote had_write before a
     * later pass can sample that zeta.  Only an actual guest depth clear may
     * clear/demote a tracked zeta (sink_clear does that above). */
}

void rsx_live_draw_shutdown(void)
{
    if (!g.ready) { rsx_rec_shutdown(); return; }
    ld_vertex_diag_emit("shutdown", 1);
    ld_native_graphics_report();
    g_native_binding_emitted=g_native_binding_elided=0;
    if (ld_native_programs()) {
        rsx_np_destroy(&g_native_vp); rsx_np_destroy(&g_native_fp);
        g_native_current_vp=NULL; g_native_program_verified=0;
    }
    ld_present_measure_dump();
    /* let the GPU drain, then release. (Best-effort; process teardown also
     * reclaims.) */
    ld_flush(LD_FLUSH_SHUTDOWN);
    rsx_rec_shutdown();
#if defined(YZ_PERF_PROFILE)
    fprintf(stderr,
            "[rsx-perf-summary] frames=%u "
            "pso{look=%llu hit=%llu miss=%llu probes=%llu full=%llu "
            "cached=%u capacity=%u mem_kb=%.1f} "
            "reject{requests=%llu unique=%u "
            "world=%llu/%u character=%llu/%u "
            "ui=%llu/%u other=%llu/%u} "
            "hlsl{vs_lookup=%llu vs_hit=%llu vs_compile=%llu vs_unique=%u "
            "ps_lookup=%llu ps_hit=%llu ps_compile=%llu ps_unique=%u} "
            "time_ms{decompile=%.3f vs_compile=%.3f ps_compile=%.3f "
            "create_pso=%.3f flush=%.3f fence=%.3f draw_cpu=%.3f} "
            "vertex{path=%s used_avg=%.3f used_max=%u "
            "legacy_mb=%.3f actual_mb=%.3f fetch_pack_ms=%.3f} "
            "vb_sync{flushes=%llu flush_ms=%.3f wait_ms=%.3f} "
            "frame{count=%llu avg_ms=%.3f compile_free=%llu "
            "compile_free_avg_ms=%.3f} "
            "draw{input_v=%llu expanded_v=%llu vb_mb=%.3f} "
            "tex{cached=%u evictions=%llu upload_mb=%.3f "
            "decode=%llu decode_ms=%.3f} "
            "high{upload_mb=%.3f vb_mb=%.3f retired=%u}\n",
            g_ld_frames,
            (unsigned long long)g_ld_profile.total.pso_lookups,
            (unsigned long long)g_ld_profile.total.pso_hits,
            (unsigned long long)g_ld_profile.total.pso_misses,
            (unsigned long long)g_ld_profile.total.pso_probes,
            (unsigned long long)g_ld_profile.total.pso_full,
            g.n_psos,
            MAX_PSOS,
            (double)sizeof(g.psos) / 1024.0,
            (unsigned long long)g_ld_profile.total.pso_full,
            g_ld_profile.n_rejected_pso_keys,
            (unsigned long long)
                g_ld_profile.rejected_pso_requests[LD_REJECT_WORLD],
            g_ld_profile.rejected_pso_unique[LD_REJECT_WORLD],
            (unsigned long long)
                g_ld_profile.rejected_pso_requests[LD_REJECT_CHARACTER],
            g_ld_profile.rejected_pso_unique[LD_REJECT_CHARACTER],
            (unsigned long long)
                g_ld_profile.rejected_pso_requests[LD_REJECT_UI],
            g_ld_profile.rejected_pso_unique[LD_REJECT_UI],
            (unsigned long long)
                g_ld_profile.rejected_pso_requests[LD_REJECT_OTHER],
            g_ld_profile.rejected_pso_unique[LD_REJECT_OTHER],
            (unsigned long long)g_ld_profile.total.vs_blob_lookups,
            (unsigned long long)g_ld_profile.total.vs_blob_hits,
            (unsigned long long)g_ld_profile.total.vs_compile_calls,
            g_ld_profile.n_vs_hashes,
            (unsigned long long)g_ld_profile.total.ps_blob_lookups,
            (unsigned long long)g_ld_profile.total.ps_blob_hits,
            (unsigned long long)g_ld_profile.total.ps_compile_calls,
            g_ld_profile.n_ps_hashes,
            ld_profile_ticks_ms(g_ld_profile.total.decompile_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.vs_compile_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.ps_compile_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.create_pso_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.flush_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.fence_wait_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.sink_end_qpc),
            ld_vertex_mode_name(),
            g_ld_profile.total.used_attribute_draws
                ? (double)g_ld_profile.total.used_attribute_sum /
                      (double)g_ld_profile.total.used_attribute_draws
                : 0.0,
            g_ld_profile.total_used_attribute_max,
            (double)g_ld_profile.total.legacy_vertex_upload_bytes /
                (1024.0 * 1024.0),
            (double)g_ld_profile.total.vertex_upload_bytes /
                (1024.0 * 1024.0),
            ld_profile_ticks_ms(
                g_ld_profile.total.vertex_fetch_pack_qpc),
            (unsigned long long)
                g_ld_profile.total.flush_reason[LD_FLUSH_VERTEX_RING],
            ld_profile_ticks_ms(
                g_ld_profile.total
                    .flush_reason_qpc[LD_FLUSH_VERTEX_RING]),
            ld_profile_ticks_ms(
                g_ld_profile.total
                    .fence_reason_qpc[LD_FLUSH_VERTEX_RING]),
            (unsigned long long)g_ld_profile.measured_frames,
            g_ld_profile.measured_frames
                ? g_ld_profile.total_frame_ms /
                      (double)g_ld_profile.measured_frames
                : 0.0,
            (unsigned long long)g_ld_profile.compile_free_frames,
            g_ld_profile.compile_free_frames
                ? g_ld_profile.compile_free_frame_ms /
                      (double)g_ld_profile.compile_free_frames
                : 0.0,
            (unsigned long long)g_ld_profile.total.input_vertices,
            (unsigned long long)g_ld_profile.total.expanded_vertices,
            (double)g_ld_profile.total.vertex_upload_bytes /
                (1024.0 * 1024.0),
            g.n_textures,
            (unsigned long long)g_ld_texture_cache_evictions,
            (double)g_ld_profile.total.texture_upload_bytes /
                (1024.0 * 1024.0),
            (unsigned long long)
                g_ld_profile.total.texture_decode_calls,
            ld_profile_ticks_ms(
                g_ld_profile.total.texture_decode_qpc),
            (double)g_ld_profile.total_upload_high / (1024.0 * 1024.0),
            (double)g_ld_profile.total_vb_high / (1024.0 * 1024.0),
            g_ld_profile.total_retired_high);
    fprintf(stderr,
            "[shader-cache-summary] "
            "vs{count=%u capacity=%u lookups=%llu hits=%llu "
            "misses=%llu inserts=%llu full_rejects=%llu "
            "compile_calls=%llu lookup_ms=%.3f compile_ms=%.3f "
            "post2048_distinct=%llu post2048_repeats=%llu "
            "source_bytes=%llu blob_bytes=%llu} "
            "ps{count=%u capacity=%u lookups=%llu hits=%llu "
            "misses=%llu inserts=%llu full_rejects=%llu "
            "compile_calls=%llu lookup_ms=%.3f compile_ms=%.3f "
            "post2048_distinct=%llu post2048_repeats=%llu "
            "source_bytes=%llu blob_bytes=%llu}\n",
            g.vs_blobs.count, MAX_SHADER_BLOBS,
            (unsigned long long)g_ld_profile.total.vs_blob_lookups,
            (unsigned long long)g_ld_profile.total.vs_blob_hits,
            (unsigned long long)g_ld_profile.total.vs_blob_misses,
            (unsigned long long)g_ld_profile.total.vs_blob_inserts,
            (unsigned long long)g_ld_profile.total.vs_blob_full_rejects,
            (unsigned long long)g_ld_profile.total.vs_compile_calls,
            ld_profile_ticks_ms(g_ld_profile.total.vs_blob_lookup_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.vs_compile_qpc),
            (unsigned long long)
                g_ld_profile.total.vs_post_boundary_distinct,
            (unsigned long long)
                g_ld_profile.total.vs_post_boundary_repeats,
            (unsigned long long)g.vs_blobs.retained_source_bytes,
            (unsigned long long)g.vs_blobs.retained_blob_bytes,
            g.ps_blobs.count, MAX_SHADER_BLOBS,
            (unsigned long long)g_ld_profile.total.ps_blob_lookups,
            (unsigned long long)g_ld_profile.total.ps_blob_hits,
            (unsigned long long)g_ld_profile.total.ps_blob_misses,
            (unsigned long long)g_ld_profile.total.ps_blob_inserts,
            (unsigned long long)g_ld_profile.total.ps_blob_full_rejects,
            (unsigned long long)g_ld_profile.total.ps_compile_calls,
            ld_profile_ticks_ms(g_ld_profile.total.ps_blob_lookup_qpc),
            ld_profile_ticks_ms(g_ld_profile.total.ps_compile_qpc),
            (unsigned long long)
                g_ld_profile.total.ps_post_boundary_distinct,
            (unsigned long long)
                g_ld_profile.total.ps_post_boundary_repeats,
            (unsigned long long)g.ps_blobs.retained_source_bytes,
            (unsigned long long)g.ps_blobs.retained_blob_bytes);
    fprintf(
        stderr,
        "[fp-constant-summary] mode=%s "
        "variants{exact_source=%u constant_specialized=%u canonical=%u "
        "collapsed_constants=%u collapsed_alpha=%u "
        "tracking_overflow=%llu} "
        "cb{allocations=%llu upload_bytes=%llu upload_ms=%.3f "
        "ring_capacity=%u capacity_failures=%llu "
        "flushes=%llu flush_ms=%.3f wait_ms=%.3f}\n",
        ld_fp_constant_mode_name(),
        g_ld_profile.n_ps_exact_source_hashes,
        g_ld_profile.n_ps_constant_specialized_hashes,
        g_ld_profile.n_ps_canonical_hashes,
        g_ld_profile.n_ps_constant_specialized_hashes >=
                g_ld_profile.n_ps_canonical_hashes
            ? g_ld_profile.n_ps_constant_specialized_hashes -
                  g_ld_profile.n_ps_canonical_hashes
            : 0,
        g_ld_profile.n_ps_exact_source_hashes >=
                g_ld_profile.n_ps_constant_specialized_hashes
            ? g_ld_profile.n_ps_exact_source_hashes -
                  g_ld_profile.n_ps_constant_specialized_hashes
            : 0,
        (unsigned long long)
            g_ld_profile.ps_variant_tracking_overflow,
        (unsigned long long)
            g_ld_profile.total.ps_constant_allocations,
        (unsigned long long)
            g_ld_profile.total.ps_constant_upload_bytes,
        ld_profile_ticks_ms(
            g_ld_profile.total.ps_constant_upload_qpc),
        PS_CB_RING_BYTES,
        (unsigned long long)
            g_ld_profile.total.ps_constant_capacity_failures,
        (unsigned long long)g_ld_profile.total.flush_reason[
            LD_FLUSH_PIXEL_CONSTANT_RING],
        ld_profile_ticks_ms(g_ld_profile.total.flush_reason_qpc[
            LD_FLUSH_PIXEL_CONSTANT_RING]),
        ld_profile_ticks_ms(g_ld_profile.total.fence_reason_qpc[
            LD_FLUSH_PIXEL_CONSTANT_RING]));
    fflush(stderr);
#endif
    if (g_ld_shader_disk_ready > 0) {
        fprintf(stderr,
                "[shader-disk-cache-summary] hits{vs=%llu ps=%llu} "
                "misses{vs=%llu ps=%llu} writes{vs=%llu ps=%llu} "
                "rejects=%llu\n",
                (unsigned long long)g_ld_shader_disk_hits[0],
                (unsigned long long)g_ld_shader_disk_hits[1],
                (unsigned long long)g_ld_shader_disk_misses[0],
                (unsigned long long)g_ld_shader_disk_misses[1],
                (unsigned long long)g_ld_shader_disk_writes[0],
                (unsigned long long)g_ld_shader_disk_writes[1],
                (unsigned long long)g_ld_shader_disk_rejects);
    }
    for (u32 i = 0; i < g.n_psos; i++) if (g.psos[i].pso) g.psos[i].pso->lpVtbl->Release(g.psos[i].pso);
    shader_blob_cache_release(&g.vs_blobs);
    shader_blob_cache_release(&g.ps_blobs);
    for (u32 i = 0; i < g.n_textures; i++) {
        rsx_source_release(&g.textures[i].source, &g_ld_source_bytes);
        if (g.textures[i].tex) g.textures[i].tex->lpVtbl->Release(g.textures[i].tex);
    }
    for (u32 i = 0; i < g.n_vtex; i++) if (g.vtex[i].tex) g.vtex[i].tex->lpVtbl->Release(g.vtex[i].tex);
    surface_views_shutdown();
    for (u32 i = 0; i < g.n_surfaces; i++) {
        if (g.surfaces[i].tex) g.surfaces[i].tex->lpVtbl->Release(g.surfaces[i].tex);
        if (g.surfaces[i].fb_tex) g.surfaces[i].fb_tex->lpVtbl->Release(g.surfaces[i].fb_tex);
        for(u32 j=0;j<LD_SWIN_PER;j++)if(g.surfaces[i].swin[j].tex)
            g.surfaces[i].swin[j].tex->lpVtbl->Release(g.surfaces[i].swin[j].tex);
        if (g.surfaces[i].lview) g.surfaces[i].lview->lpVtbl->Release(g.surfaces[i].lview);
    }
    for (u32 i = 0; i < g.n_zdepths; i++) {
        if (g.zdepths[i].packed) g.zdepths[i].packed->lpVtbl->Release(g.zdepths[i].packed);
        if (g.zdepths[i].tex)
            g.zdepths[i].tex->lpVtbl->Release(g.zdepths[i].tex);
        if (g.zdepths[i].snapshot)
            g.zdepths[i].snapshot->lpVtbl->Release(
                g.zdepths[i].snapshot);
    }
    for (u32 i=0; i<2; ++i) if (g.host_image[i].tex)
        g.host_image[i].tex->lpVtbl->Release(g.host_image[i].tex);
    if (g.movie_upload) g.movie_upload->lpVtbl->Release(g.movie_upload);
    if (g.movie_overlay_readback) g.movie_overlay_readback->lpVtbl->Release(g.movie_overlay_readback);
    if (g.movie_overlay_rgba) free(g.movie_overlay_rgba);
    if (g.movie_overlay_mask) free(g.movie_overlay_mask);
    if (g.white_tex) g.white_tex->lpVtbl->Release(g.white_tex);
    if (g.depth) g.depth->lpVtbl->Release(g.depth);
    if (g.packed_depth_pso) g.packed_depth_pso->lpVtbl->Release(g.packed_depth_pso);
    if (g.depth_snapshot_pso)
        g.depth_snapshot_pso->lpVtbl->Release(g.depth_snapshot_pso);
    for (u32 i = 0; i < 4; i++)
        if (g.downsample_pso[i]) g.downsample_pso[i]->lpVtbl->Release(g.downsample_pso[i]);
    if (g.downsample_rootsig)
        g.downsample_rootsig->lpVtbl->Release(g.downsample_rootsig);
    for (u32 i = 0; i < 4; ++i) {
        if (g_sblit_pso[i].pso) g_sblit_pso[i].pso->lpVtbl->Release(g_sblit_pso[i].pso);
        g_sblit_pso[i].pso = NULL;
        g_sblit_pso[i].fmt = DXGI_FORMAT_UNKNOWN;
    }
    if (g_sblit_rs) { g_sblit_rs->lpVtbl->Release(g_sblit_rs); g_sblit_rs = NULL; }
    if (g.depth_snapshot_rootsig)
        g.depth_snapshot_rootsig->lpVtbl->Release(
            g.depth_snapshot_rootsig);
    if (g.rootsig_x) g.rootsig_x->lpVtbl->Release(g.rootsig_x);
    if (g.swap) g.swap->lpVtbl->Release(g.swap);
    if (g_ld_info_queue) {
        g_ld_info_queue->lpVtbl->Release(g_ld_info_queue);
        g_ld_info_queue = NULL;
    }
#if defined(YZ_PERF_PROFILE)
    if (g_ld_profile.adapter) {
        g_ld_profile.adapter->lpVtbl->Release(g_ld_profile.adapter);
        g_ld_profile.adapter = NULL;
    }
#endif
    for (u32 slot = 0; slot < g.frame_count; ++slot)
        ld_frame_resources_destroy(&g.frames[slot]);
    if(geometry_pipeline)geometry_pipeline->lpVtbl->Release(geometry_pipeline);
    if(geometry_root)geometry_root->lpVtbl->Release(geometry_root);
    geometry_pipeline=NULL;geometry_root=NULL;geometry_failed=0;
    if (g.dev) g.dev->lpVtbl->Release(g.dev);
    memset(&g, 0, sizeof(g));
    if (dc.verts) { free(dc.verts); dc.verts = NULL; dc.cap_verts = 0; }
    if (dc.refs) { free(dc.refs); dc.refs = NULL; dc.cap_refs = 0; }
    if (dc.compact_verts) {
        free(dc.compact_verts);
        dc.compact_verts = NULL;
        dc.compact_capacity = 0;
    }
    if (dc.cuts) { free(dc.cuts); dc.cuts = NULL; dc.cap_cuts = 0; }
}

#endif /* _WIN32 */
