/* Real D3D12 test: Z24S8 packed as ARGB with exact bytes, stencil, remap,
 * same-frame invalidation, growing view and colour/depth ownership. */
#include "live_draw_test_harness.h"
#define T "test_live_packed_depth"
#define ZA 0x100000u
#define OUT 0x40000u
#define FP 0x1000u
#define FMT (8u | (2u<<5) | (1u<<8))
static int check_pixel(u32 out, const char* name, u32 w, u32 h, int r, int g, int b)
{
    uint8_t rgb[64*64*3]; u32 rw=0,rh=0;
    int ok=ldh_read_rgb(out,"packed_depth.ppm",rgb,64,64,&rw,&rh)==0 && rw==w && rh==h;
    if(ok) for(u32 i=0;i<w*h;i++)
        if(rgb[3*i]!=r || rgb[3*i+1]!=g || rgb[3*i+2]!=b){
            printf("pixel %u got %u,%u,%u want %d,%d,%d\n",i,rgb[3*i],rgb[3*i+1],rgb[3*i+2],r,g,b);ok=0;break;
        }
    ldh_check(name,ok); return ok;
}
static void clear_depth(u32 w, u32 h, u32 value, u32 mask)
{
    ldh_surface(FMT,0,64,ZA,64,w,h);
    ldh_m(NV4097_SET_ZSTENCIL_CLEAR_VALUE,value);
    ldh_m(NV4097_CLEAR_SURFACE,mask);
}
static void sample(u32 out,u32 w,u32 h,u32 remap)
{
    ldh_surface(FMT,out,64,ZA,64,w,h);
    ldh_texture0(ZA,0xA5, w,h,64);
    ldh_m(NV4097_SET_TEXTURE_CONTROL1,remap);
    ldh_draw(FP,0x40,1,0,0x207);
}
int main(void)
{
    if(ldh_init(T)) return 2;
    rsx_test_fp_tex_r0_tc0(ldh_main+FP,0);
    ldh_setup_quad(0,1,1,0,0);
    clear_depth(4,4,0x123456A7,3);
    sample(OUT,4,4,0xAAE4);
    check_pixel(OUT,"clear-only depth, ARGB identity and nonzero stencil",4,4,0x34,0x56,0xA7);
    sample(OUT,4,4,0xAA93);
    check_pixel(OUT,"game remap reconstructs high/mid/low depth bytes",4,4,0x12,0x34,0x56);
    rsx_depth_view_stats before,after;
    rsx_live_draw_depth_view_stats(&before);
    ldh_m(NV4097_SET_STENCIL_TEST_ENABLE,1);
    ldh_m(NV4097_SET_STENCIL_MASK,255);
    ldh_m(NV4097_SET_STENCIL_FUNC,0x207);
    ldh_m(NV4097_SET_STENCIL_FUNC_MASK,255);
    ldh_m(NV4097_SET_STENCIL_OP_FAIL,0x1E00);
    ldh_m(NV4097_SET_STENCIL_OP_ZFAIL,0x1E00);
    ldh_m(NV4097_SET_STENCIL_OP_ZPASS,0x1E00);
    for(int i=0;i<8;i++) sample(OUT,4,4,i&1?0xAA93:0xAAE4);
    rsx_live_draw_depth_view_stats(&after);
    ldh_check("eight remapped reads with KEEP stencil need no extra resolve",
        after.resolves==before.resolves && after.binds==before.binds+8 && after.resident_bytes==64);
    ldh_m(NV4097_SET_STENCIL_FUNC_REF,0xE1);
    ldh_m(NV4097_SET_STENCIL_OP_ZPASS,0x1E01); /* REPLACE */
    sample(OUT,4,4,0xAAE4); /* reads previous version, then writes stencil */
    ldh_m(NV4097_SET_STENCIL_TEST_ENABLE,0);
    sample(OUT,4,4,0xAAE4);
    check_pixel(OUT,"stencil geometry write invalidates and publishes new stencil",4,4,0x34,0x56,0xE1);
    clear_depth(4,4,0xABCD12C5,3);
    sample(OUT,4,4,0xAAE4);
    check_pixel(OUT,"same-frame depth and stencil clear invalidates cached view",4,4,0xCD,0x12,0xC5);
    clear_depth(4,4,0x00000039,2);
    sample(OUT,4,4,0xAAE4);
    check_pixel(OUT,"stencil-only clear invalidates packed view and preserves depth",4,4,0xCD,0x12,0x39);
    clear_depth(16,16,0xFFFFFE81,3);
    sample(OUT+0x1000,16,16,0xAA93);
    check_pixel(OUT+0x1000,"grown packed snapshot and far-plane predecessor",16,16,0xFF,0xFF,0xFE);
    /* A geometry depth write, including read-own-zeta in the next draw. */
    for(u32 i=0;i<4;i++) ldh_wf(i*48+8,0.5f); /* viewport depth .75 */
    ldh_m(NV4097_SET_TEXTURE_CONTROL0,0);
    rsx_test_fp_mov_r0_col0(ldh_main+FP+0x100,RSX_TEST_FP_SWZ_IDENT);
    ldh_m(NV4097_SET_COLOR_MASK,0);
    ldh_draw(FP+0x100,0x40,1,1,0x207);
    ldh_m(NV4097_SET_COLOR_MASK,0x01010101);
    sample(OUT+0x1000,16,16,0xAA93);
    check_pixel(OUT+0x1000,"geometry updates exact UNORM24 depth .75",16,16,0xBF,0xFF,0xFF);
    /* A newer colour write must take ownership of the same address. */
    ldh_surface(FMT,ZA,64,ZA+0x10000,64,16,16);
    ldh_m(NV4097_SET_COLOR_CLEAR_VALUE,0xFF2468AC);
    ldh_m(NV4097_CLEAR_SURFACE,0xF0);
    sample(OUT+0x1000,16,16,0xAAE4);
    check_pixel(OUT+0x1000,"newer colour writer supersedes older packed depth",16,16,0x24,0x68,0xAC);
    rsx_live_draw_shutdown();
    return ldh_done(T);
}
