#ifndef RSX_RESOLUTION_H
#define RSX_RESOLUTION_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Guest addresses, row pitches, texture coordinates and cache keys stay in
 * logical texels. Only GPU image allocations and raster/copy rectangles cross
 * this boundary. Asset textures keep their original resolution.
 *
 * Half-step scales also cover 1080p from a 720p logical canvas. Map edges
 * consistently with ceil, including negative clips and odd-size targets.
 * Copies between different fractional phases require a filtered transfer.
 * The domain is immutable for a device's lifetime. */
typedef struct { float scale; } rsx_resolution_domain;

/* Up to 8: the display scale itself is 1..4 (BLEACH_RENDER_SCALE), and glyph
 * scratches are drawn at twice it (rsx_live_draw.c, ld_logic_target_scale). */
static inline int rsx_resolution_valid(rsx_resolution_domain d)
{
    return d.scale >= 1 && d.scale <= 8 &&
        d.scale * 2 == (uint32_t)(d.scale * 2);
}

static inline uint32_t rsx_resolution_extent(rsx_resolution_domain d, uint32_t logical)
{
    if (!rsx_resolution_valid(d) || !logical)
        return 0;
    uint64_t physical = ((uint64_t)logical * (uint32_t)(d.scale * 2) + 1) / 2;
    return physical <= 16384 ? (uint32_t)physical : 0;
}

static inline int32_t rsx_resolution_edge(rsx_resolution_domain d, int32_t logical)
{
    if (!rsx_resolution_valid(d)) return 0;
    int64_t n = (int64_t)logical * (int32_t)(d.scale * 2);
    n = (n + (n > 0)) / 2;
    return n < INT32_MIN ? INT32_MIN : n > INT32_MAX ? INT32_MAX : (int32_t)n;
}

static inline int32_t rsx_resolution_span(rsx_resolution_domain d,
                                         int32_t begin, int32_t end)
{
    return rsx_resolution_edge(d, end) - rsx_resolution_edge(d, begin);
}

static inline int rsx_resolution_copy_aligned(rsx_resolution_domain d,
                                              int32_t source, int32_t destination)
{
    return rsx_resolution_valid(d) &&
        (((int64_t)source - destination) * (int32_t)(d.scale * 2)) % 2 == 0;
}

/* SV_POSITION is physical, but the original fragment program's WPOS input
 * is expressed in logical texels. Correct only xy in the original entry;
 * depth, reciprocal w, interpolants and later export wrappers are unchanged.
 * Generated HLSL is also the disk-cache identity, so scales cannot collide. */
static inline int rsx_resolution_fragment(char* hlsl, size_t capacity,
                                          rsx_resolution_domain d)
{
    if (d.scale == 1) return 0;
    char* entry = strstr(hlsl, " main(PSInput input)");
    if (!entry || !rsx_resolution_valid(d)) return -1;
    char* body = strchr(entry, '{');
    if (!body) return -1;
    ++body;
    char prefix[96];
    int n = snprintf(prefix, sizeof(prefix),
        "\n    input.position.xy *= (1.0 / %.1f);\n", (double)d.scale);
    size_t length = strlen(hlsl);
    if (n < 0 || (size_t)n >= sizeof(prefix) || length + (size_t)n + 1 > capacity)
        return -1;
    memmove(body + n, body, length - (size_t)(body - hlsl) + 1);
    memcpy(body, prefix, (size_t)n);
    return 1;
}

#endif
