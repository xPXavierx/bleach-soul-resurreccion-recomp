/* Real GPU oracle for dynamic constants and code replacement at the same EA,
 * scissor transitions, target switches and two recording-slot lifetimes. */
#include "live_draw_test_harness.h"
#define FMT (0x8u | (0x2u<<5) | (0x1u<<8))
static unsigned char rgb[64*64*3];
static unsigned value(u32 frame,u32 half,u32 channel)
{ return 20+(frame*19+half*87+channel*41)%210; }
static void program(u32 frame,u32 half)
{
    u8* p=ldh_main+0x1000;
    if(frame==2) rsx_test_fp_mov_r0_col0(p,RSX_TEST_FP_SWZ_IDENT);
    else {
        rsx_test_fp_instr(p,RSX_TEST_FP_OPC(1)|RSX_TEST_FP_MASK_XYZW|
            (frame==3 ? RSX_TEST_FP_OUTREG(1) : 1u),2|RSX_TEST_FP_SWZ_IDENT);
        for(u32 c=0;c<4;c++) {
            float f=c==3 ? 1 : value(frame,half,c)/255.0f; u32 w;memcpy(&w,&f,4);
            rsx_test_fp_put_word(p+16+c*4,w);
        }
        if(frame==3)rsx_test_fp_instr(p+32,RSX_TEST_FP_OPC(1)|RSX_TEST_FP_MASK_XYZW|1,
            4|RSX_TEST_FP_SWZ_IDENT); /* MOV r0,r1 */
    }
    for(u32 i=0;i<4;i++)for(u32 c=0;c<3;c++)ldh_wf(i*48+16+c*4,value(frame,half,c)/255.0f);
}
int main(void)
{
    if(!rsx_live_draw_set_frames_in_flight(2) || ldh_init("native program draws"))return 2;
    for(u32 frame=0;frame<8;frame++) {
        u32 output=0x40000+frame*0x10000;
        ldh_surface(FMT,output,256,0x180000,256,64,64);
        ldh_setup_quad(0,1,1,0,0);
        for(u32 half=0;half<2;half++) {
            program(frame,half);
            ldh_m(NV4097_SET_SCISSOR_HORIZONTAL,(32u<<16)|(half*32));
            ldh_draw(0x1000,0x40,0,0,0x207);
        }
        rsx_live_draw_set_display_buffer(0,0,output,256,64,64);
        rsx_live_draw_present(0);
    }
    rsx_live_draw_flush();
    for(u32 frame=0;frame<8;frame++) {
        u32 w=0,h=0,bad=0; char label[100];
        int rc=ldh_read_rgb(0x40000+frame*0x10000,"native-program-output.ppm",rgb,64,64,&w,&h);
        if(!rc)for(u32 y=0;y<h;y++)for(u32 x=0;x<w;x++)for(u32 c=0;c<3;c++)
            if(abs((int)rgb[(y*w+x)*3+c]-(int)value(frame,x>=32,c))>1)bad++;
        snprintf(label,sizeof(label),"frame %u code/constants/scissor retained (%u bad channels)",frame,bad);
        ldh_check(label,!rc && w==64 && h==64 && !bad);
    }
    int result=ldh_done("native program draws");rsx_live_draw_shutdown();return result;
}
