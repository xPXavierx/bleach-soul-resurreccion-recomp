/* A clear is a depth-content write even when no geometry is submitted.
 * Verify the production GPU result; no title data or register heuristics. */
#include "live_draw_test_harness.h"
#define FMT (0x8u | (0x2u<<5) | (0x1u<<8))
#define Z 0x100000u
#define OUT 0x4000u
#define COLOR 0x1100u
#define TEX 0x1000u
static void source(void){ldh_surface(FMT,0,64,Z,64,16,16);}
static void expect(const char* label,unsigned value)
{
    ldh_surface(FMT,OUT,64,Z+0x2000,64,16,16);
    ldh_setup_quad(0,1,1,.5f,.5f);
    ldh_texture0(Z,0xb0,16,16,64);
    ldh_m(NV4097_SET_TEXTURE_ADDRESS,0x40030303); /* stored > reference */
    ldh_draw(TEX,0x40,0,0,0x207);
    unsigned char rgb[64*64*3];u32 w,h;
    int read=ldh_read_rgb(OUT,"depth-content.ppm",rgb,64,64,&w,&h);
    unsigned bad=0;
    if(!read)for(u32 i=0;i<w*h;i++)if(rgb[i*3]!=value)bad++;
    printf("expected=%u actual=%u mismatched=%u\n",value,read?999:rgb[0],bad);
    ldh_check(label,!read&&!bad);
}
int main(void)
{
    if(ldh_init("depth content"))return 2;
    rsx_test_fp_tex_r0_tc0(ldh_main+TEX,0);
    rsx_test_fp_mov_r0_col0(ldh_main+COLOR,RSX_TEST_FP_SWZ_IDENT);
    source();ldh_m(NV4097_SET_ZSTENCIL_CLEAR_VALUE,0xffffff00u);
    ldh_m(NV4097_CLEAR_SURFACE,1);
    expect("clear-only depth is authoritative GPU content",255);
    source();ldh_m(NV4097_SET_ZSTENCIL_CLEAR_VALUE,0);
    ldh_m(NV4097_CLEAR_SURFACE,1);
    /* NEVER fails every fragment. Its presence cannot alter a clear's value.
     * This also makes the legacy draw-only validity heuristic take a snapshot,
     * isolating zero-value decoding from the first regression above. */
    ldh_setup_quad(0,1,1,0,0);ldh_draw(COLOR,0x40,1,1,0x200);
    expect("explicit zero depth clear remains zero",0);
    source();ldh_m(NV4097_SET_ZSTENCIL_CLEAR_VALUE,0xffffff00u);
    ldh_m(NV4097_CLEAR_SURFACE,1);
    expect("clear invalidates previous published depth contents",255);
    source();ldh_m(NV4097_SET_ZSTENCIL_CLEAR_VALUE,0x41);
    ldh_m(NV4097_CLEAR_SURFACE,2);
    expect("stencil-only clear preserves depth contents",255);
    int result=ldh_done("depth content");rsx_live_draw_shutdown();return result;
}
