/* Reuse two production GPU slots while replacing vertex data and surface
 * views. Read back only after 32 presentations, so the oracle cannot silently
 * turn each presentation into a blocking submission. No title assets. */
#include "live_draw_test_harness.h"
#define FMT (0x8u | (0x2u<<5) | (0x1u<<8))
#define SOURCE 0x20000u
#define OUTPUT 0x40000u
#define Z 0x180000u
#define COLOR 0x1100u
#define TEX 0x1000u
static unsigned char rgb[256*256*3];
static unsigned component(unsigned frame,unsigned channel)
{ return 24+(frame*29+channel*67)%208; }
int main(void)
{
    ldh_check("reject invalid slot count",!rsx_live_draw_set_frames_in_flight(3));
    ldh_check("select two submission slots",rsx_live_draw_set_frames_in_flight(2));
    if(ldh_init("frame submissions"))return 2;
    ldh_check("two slots active",rsx_live_draw_frames_in_flight()==2);
    ldh_check("reject slot changes after init",!rsx_live_draw_set_frames_in_flight(1));
    rsx_test_fp_tex_r0_tc0(ldh_main+TEX,0);
    rsx_test_fp_mov_r0_col0(ldh_main+COLOR,RSX_TEST_FP_SWZ_IDENT);
    u64 previous=0;unsigned bad_fences=0;
    for(unsigned frame=0;frame<32;frame++) {
        ldh_m(NV4097_SET_TEXTURE_CONTROL0,0);
        ldh_surface(FMT,SOURCE,256,Z,256,64,64);
        ldh_setup_quad(0,1,1,0,0);
        if(frame&1) {
            u32 texture=0x200000+frame*0x4000;
            for(unsigned i=0;i<64*64;i++) {
                ldh_local[texture+i*4]=255;
                for(unsigned c=0;c<3;c++)ldh_local[texture+i*4+1+c]=(u8)component(frame,c);
            }
            ldh_texture0(texture,0xa5,64,64,256);
            ldh_draw(TEX,0x40,0,0,0x207);
        } else {
            for(unsigned i=0;i<4;i++)for(unsigned c=0;c<3;c++)
                ldh_wf(i*48+16+c*4,component(frame,c)/255.0f);
            ldh_draw(COLOR,0x40,0,0,0x207);
        }
        u32 output=OUTPUT+(frame%8)*0x10000;
        ldh_surface(FMT,output,256,Z+0x20000,256,64,64);
        ldh_setup_quad(0,1,1,0,0);
        ldh_texture0(SOURCE,0xa5,61-frame%3,59-frame%5,256);
        ldh_draw(TEX,0x40,0,0,0x207);
        rsx_live_draw_set_display_buffer(0,0,output,256,64,64);
        rsx_live_draw_present(0);
        u64 submitted=rsx_live_draw_submitted_fence();
        if(submitted<=previous || rsx_live_draw_completed_fence()>submitted)bad_fences++;
        previous=submitted;
    }
    ldh_check("submission fences advance monotonically",!bad_fences);
    rsx_live_draw_flush();
    ldh_check("explicit flush completes every older submission",
        rsx_live_draw_completed_fence()==rsx_live_draw_submitted_fence());
    for(unsigned slot=0;slot<8;slot++) {
        u32 w=0,h=0;char name[80];unsigned bad=0;
        int rc=ldh_read_rgb(OUTPUT+slot*0x10000,"submission-output.ppm",rgb,256,256,&w,&h);
        if(!rc)for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)for(unsigned c=0;c<3;c++)
            if(abs((int)rgb[(y*w+x)*3+c]-(int)component(24+slot,c))>1)bad++;
        snprintf(name,sizeof(name),"frame %u retains its own geometry and sampled content",24+slot);
        printf("frame=%u dimensions=%ux%u bad_channels=%u\n",24+slot,w,h,bad);
        ldh_check(name,!rc && w && h && !bad);
    }
    int result=ldh_done("frame submissions");
    rsx_live_draw_shutdown();return result;
}
