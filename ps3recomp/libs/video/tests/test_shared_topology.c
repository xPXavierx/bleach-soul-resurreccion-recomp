/* Contract tests for the expansion used by both live D3D12 and the draw engine. */
#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void golden(u32 prim, u32 n, const u32* cuts, u32 nc,
                   const u32* expected, u32 count)
{
    u32 storage[512], remap[128], direct[512];
    int indexed = -1;
    CHECK(rsx_vertex_topology_plan(prim, 1, &indexed) && indexed);
    CHECK(rsx_vertex_topology_index_count(prim, n, cuts, nc) == count);
    memset(storage, 0xCC, sizeof storage);
    rsx_vertex_write_topology_indices(prim, n, cuts, nc, NULL, storage + 1);
    CHECK(storage[0] == 0xCCCCCCCC && storage[count + 1] == 0xCCCCCCCC);
    CHECK(memcmp(storage + 1, expected, count * sizeof(u32)) == 0);
    memcpy(direct, storage + 1, count * sizeof(u32));
    for (u32 i = 0; i < n; ++i) remap[i] = (i * 7u) % 13u;
    rsx_vertex_write_topology_indices(prim, n, cuts, nc, remap, storage + 1);
    for (u32 i = 0; i < count; ++i) CHECK(storage[i + 1] == remap[direct[i]]);
    CHECK(storage[0] == 0xCCCCCCCC && storage[count + 1] == 0xCCCCCCCC);
}

int main(void)
{
    const u32 quad[] = {0,1,2,1,3,2,2,3,4,3,5,4};
    const u32 odd_cut[] = {3,8}, odd_expected[] = {3,4,5,4,6,5,8,9,10,9,11,10};
    const u32 cut[] = {4}, cut_expected[] = {0,1,2,1,3,2,4,5,6,5,7,6};
    const u32 fan[] = {0,1,2,0,2,3,0,3,4};
    const u32 strip[] = {0,1,2,2,1,3,2,3,4};
    const u32 quads[] = {0,1,2,2,3,0,4,5,6,6,7,4};
    const u32 lines[] = {0,1,1,2,2,3,4,5,5,6,6,7};
    const u32 tris[] = {0,1,2,3,4,5};
    golden(RSX_PRIMITIVE_QUAD_STRIP, 6, NULL, 0, quad, 12);
    golden(RSX_PRIMITIVE_QUAD_STRIP, 7, NULL, 0, quad, 12);
    golden(RSX_PRIMITIVE_QUAD_STRIP, 8, cut, 1, cut_expected, 12);
    golden(RSX_PRIMITIVE_QUAD_STRIP, 12, odd_cut, 2, odd_expected, 12);
    golden(RSX_PRIMITIVE_TRIANGLE_FAN, 5, NULL, 0, fan, 9);
    golden(RSX_PRIMITIVE_POLYGON, 5, NULL, 0, fan, 9);
    golden(RSX_PRIMITIVE_TRIANGLE_STRIP, 5, NULL, 0, strip, 9);
    golden(RSX_PRIMITIVE_QUADS, 8, NULL, 0, quads, 12);
    golden(RSX_PRIMITIVE_LINE_STRIP, 8, cut, 1, lines, 12);
    golden(RSX_PRIMITIVE_TRIANGLES, 8, NULL, 0, tris, 6);
    int indexed = -1;
    CHECK(rsx_vertex_topology_plan(RSX_PRIMITIVE_TRIANGLES, 0, &indexed) && !indexed);
    CHECK(!rsx_vertex_topology_plan(99, 0, &indexed) && !indexed);
    CHECK(!rsx_vertex_topology_plan(RSX_PRIMITIVE_QUAD_STRIP, 0, NULL));
    /* Exhaust trailing/short/empty restart segments. The expected count is
     * expressed as complete vertex pairs; no triangle may cross the cut. */
    unsigned cases = 0;
    for (u32 n = 0; n <= 128; ++n) for (u32 split = 0; split <= n; ++split) {
        u32 total = 0, lengths[] = {split, n - split};
        for (u32 s = 0; s < 2; ++s) {
            u32 pairs = lengths[s] / 2;
            total += pairs > 1 ? (pairs - 1) * 6 : 0;
        }
        CHECK(rsx_vertex_topology_index_count(RSX_PRIMITIVE_QUAD_STRIP, n, &split, 1) == total);
        u32 out[400]; memset(out, 0xCC, sizeof out);
        rsx_vertex_write_topology_indices(RSX_PRIMITIVE_QUAD_STRIP, n, &split, 1, NULL, out + 1);
        CHECK(out[0] == 0xCCCCCCCC && out[total + 1] == 0xCCCCCCCC);
        for (u32 t = 0; t < total; t += 3) {
            u32 a = out[t+1], b = out[t+2], c = out[t+3];
            CHECK(a < n && b < n && c < n);
            CHECK((a < split) == (b < split) && (a < split) == (c < split));
            u32 begin = a < split ? 0 : split;
            /* Paired vertices lie at x=pair, y=pair-side. Every triangle
             * must keep the same negative signed area after a restart. */
            int ax=(a-begin)/2, ay=(a-begin)%2;
            int bx=(b-begin)/2, by=(b-begin)%2;
            int cx=(c-begin)/2, cy=(c-begin)%2;
            CHECK((bx-ax)*(cy-ay)-(by-ay)*(cx-ax) == -1);
        }
        ++cases;
    }
    CHECK(rsx_vertex_topology_index_count(RSX_PRIMITIVE_QUAD_STRIP, 32, NULL, 0) == 90);
    printf("PASS shared topology: %u restart/bounds/winding cases; golden/remap tests for all accepted classes.\n", cases);
    return 0;
}
