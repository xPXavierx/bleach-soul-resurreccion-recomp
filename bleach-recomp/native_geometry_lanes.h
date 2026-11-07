#ifndef BLEACH_NATIVE_GEOMETRY_LANES_H
#define BLEACH_NATIVE_GEOMETRY_LANES_H
/* Extra concurrent lanes for the EDGE geometry job list.
 *
 * The title creates "geom-jobchain" with maxContention=4 and gives each SPU
 * number its own EdgeGeomRingBufferInfo line (table at 0x00843700, 128 bytes
 * per SPU), a 4 MiB output ring mapped for the RSX and one RSX label
 * (0x80 + SPU). Measured: geometry is the frame's critical path and saturates
 * those four lanes. BLEACH_GEOMETRY_LANES=N (5..16) lets the job pool use N
 * lanes; every SPU number from 4 upward receives a runtime-owned ring at
 * 0x38000000 + (spu - 4) * 4 MiB, an RSX IO mapping, label 0x80 + spu and a
 * private copy of its ring-info line (redirected lock-line atomics), so no
 * guest memory beyond the original four-entry table is touched.
 *
 * Off by default. Any mismatch with the observed table layout disables the
 * extension before a single extra lane is granted. */
#include "spurs_native_batch.h"
#include "spu_context.h"
#include "spu_image_cache.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
extern "C" unsigned char* vm_base;
extern "C" uint32_t cellGcm_host_map_main_memory(uint32_t ea, uint32_t size);
namespace bleach_geometry_lanes {
constexpr uint32_t kTable = 0x00843700u, kLine = 0x80u, kRingSize = 0x400000u;
constexpr uint32_t kLabelBase = 0x20000800u, kLabelStride = 0x10u;
constexpr uint32_t kRingBase = 0x38000000u, kShadowBase = 0x37FF0000u;
constexpr uint32_t kOriginalLanes = 4, kMaxLanes = 16, kGeometryImage = 1014;
static unsigned lanes = 0;          // requested total lanes, 0 = disabled
static int state = 0;               // 0 untried, 1 ready, -1 refused
static std::mutex setup_lock;

static uint32_t rd(uint32_t ea) {
    const unsigned char* p = vm_base + ea;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
static void wr(uint32_t ea, uint32_t v) {
    unsigned char* p = vm_base + ea;
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
// The same independent 256-byte EDGE list the native geometry pool accepts.
static bool geometry_list(const spurs_native_batch* b) {
    if (b->count < 2 || b->descriptor_size != 256 || b->stride != 256 ||
        uint64_t(b->array) + uint64_t(b->count) * 256 > 0x10000000ull) return false;
    const uint32_t binary = rd(b->array + 4) & ~7u;
    if (!binary || binary + 48272u > 0x10000000u) return false;
    static thread_local spu_image_cache cache{};
    return spu_image_cache_fingerprint(&cache, vm_base + binary, 48272) == 0xde1065fcb7151ac3ull;
}
// Lines 0..3 must look exactly like the rings the title built.
static bool table_matches() {
    for (uint32_t spu = 0; spu < kOriginalLanes; spu++) {
        const uint32_t line = kTable + spu * kLine;
        const uint32_t start = rd(line), end = rd(line + 8), current = rd(line + 12);
        if (!start || end != start + kRingSize || current < start || current > end ||
            rd(line + 16) != 1 || rd(line + 20) != kLabelBase + spu * kLabelStride) {
            fprintf(stderr, "[geometry-lanes] table line %u does not match the observed EDGE layout\n", spu);
            return false;
        }
    }
    return true;
}
static bool setup() {
    if (!table_matches()) return false;
    const uint32_t extra = kMaxLanes - kOriginalLanes;
    const uint32_t io = cellGcm_host_map_main_memory(kRingBase, extra * kRingSize);
    if (io == 0xFFFFFFFFu) {
        fprintf(stderr, "[geometry-lanes] RSX IO mapping refused\n");
        return false;
    }
    for (uint32_t spu = kOriginalLanes; spu < kMaxLanes; spu++) {
        const uint32_t line = kShadowBase + spu * kLine;
        const uint32_t start = kRingBase + (spu - kOriginalLanes) * kRingSize;
        const uint32_t label = kLabelBase + spu * kLabelStride;
        // Same initial state the title gives its own rings (observed at the
        // first GETLLAR): current = start, cached free = 0, label = end.
        for (uint32_t w = 0; w < kLine; w += 4) wr(line + w, 0);
        wr(line, start); wr(line + 4, io + (spu - kOriginalLanes) * kRingSize);
        wr(line + 8, start + kRingSize); wr(line + 12, start);
        wr(line + 16, 1); wr(line + 20, label);
        wr(label, start + kRingSize);
    }
    fprintf(stderr, "[geometry-lanes] enabled lanes=%u extra rings at 0x%08X io=0x%08X labels 0x%02X..0x%02X\n",
        lanes, kRingBase, io, 0x80 + kOriginalLanes, 0x80 + kMaxLanes - 1);
    return true;
}
static unsigned extension(const spurs_native_batch* batch) {
    if (!lanes || state < 0 || !geometry_list(batch)) return 0;
    if (!state) {
        std::lock_guard<std::mutex> guard(setup_lock);
        if (!state) state = setup() ? 1 : -1;
        if (state < 0) return 0;
    }
    return lanes;
}
// Redirect the ring-info line of SPU numbers 4.. to the runtime-owned copy.
static uint32_t remap(spu_context* ctx, uint32_t ea) {
    if (state <= 0 || ctx->image_id != (int)kGeometryImage) return ea;
    if (ea < kTable + kOriginalLanes * kLine || ea >= kTable + kMaxLanes * kLine) return ea;
    const uint32_t spu = (uint32_t(ctx->ls[0x1C8]) << 24 | uint32_t(ctx->ls[0x1C9]) << 16 |
                          uint32_t(ctx->ls[0x1CA]) << 8 | ctx->ls[0x1CB]);
    const uint32_t index = (ea - kTable) / kLine;
    if (index != spu) {
        fprintf(stderr, "[geometry-lanes] STOP: SPU %u addressed ring line %u\n", spu, index);
        abort();
    }
    return kShadowBase + index * kLine;
}
static void initialize() {
    const char* env = getenv("BLEACH_GEOMETRY_LANES");
    if (!env || !*env || !strcmp(env, "0")) return;
    char* end = nullptr;
    const unsigned long n = strtoul(env, &end, 10);
    if (!end || *end || n <= kOriginalLanes || n > kMaxLanes) {
        fprintf(stderr, "[geometry-lanes] BLEACH_GEOMETRY_LANES must be %u..%u\n", kOriginalLanes + 1, kMaxLanes);
        abort();
    }
    lanes = unsigned(n);
    g_spurs_lane_extension = extension;
    g_spu_atomic_ea_remap = remap;
}
}
#endif
