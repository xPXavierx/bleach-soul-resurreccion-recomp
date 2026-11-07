/* Exercise clipping independently of depth testing and PSO reuse. */
#include "live_draw_test_harness.h"
#define FMT (0x8u | (0x2u<<5) | (0x1u<<8))
#define ZCONTROL 0x1D78u
static void probe(const char* label, float z, int control, unsigned expected)
{
    ldh_surface(FMT,0,64,0x100000,64,16,16);
    ldh_m(NV4097_SET_COLOR_CLEAR_VALUE,0xff000000);
    ldh_m(NV4097_CLEAR_SURFACE,0xf0);
    ldh_setup_quad(0,1,1,0,0);
    for(unsigned i=0;i<4;i++)ldh_wf(i*48+8,z);
    if(control>=0)ldh_m(ZCONTROL,(unsigned)control);
    ldh_draw(0x1000,0x40,0,0,0x207);
    unsigned char rgb[64*64*3];u32 w,h;
    int read=ldh_read_rgb(0,"depth-clip.ppm",rgb,64,64,&w,&h);
    unsigned bad=0;
    if(!read)for(u32 i=0;i<w*h;i++)if(rgb[i*3]!=expected)bad++;
    printf("z=%g control=%d expected=%u actual=%u mismatched=%u\n",z,control,expected,read?999:rgb[0],bad);
    ldh_check(label,!read&&!bad);
}
int main(void)
{
    if(ldh_init("depth clip"))return 2;
    rsx_test_fp_mov_r0_col0(ldh_main+0x1000,RSX_TEST_FP_SWZ_IDENT);
    probe("reset state clips beyond far plane",2,-1,0);
    probe("inside clip volume remains visible",0,1,255);
    probe("near clipping works without depth test",-2,1,0);
    probe("far clipping works without depth test",2,1,0);
    probe("disabled clip preserves out of range geometry",2,0,255);
    probe("explicit clamp overrides clip",-2,0x11,255);
    probe("reenabling clip selects correct cached pipeline",-2,1,0);
    int result=ldh_done("depth clip");rsx_live_draw_shutdown();return result;
}
