/* Validate source modifiers on the production GPU with independent expected
 * values. All three operands are exercised, including ABS/NEG ordering. */
#include "live_draw_test_harness.h"
#define FMT (0x8u | (0x2u<<5) | (0x1u<<8))
#define SW RSX_TEST_FP_SWZ_IDENT
static u32 src(u32 r){return r*4u|SW;}
static void ins(u8*p,u32 op,u32 dst,u32 s0,u32 s1,u32 s2,int end)
{
    rsx_test_fp_instr(p,(op<<24)|(dst<<1)|(15u<<9)|(end?1u:0u),s0);
    rsx_test_fp_put_word(p+8,s1);rsx_test_fp_put_word(p+12,s2);
}
static void constant(u8*p,const float v[4])
{
    for(u32 i=0;i<4;i++){u32 w;memcpy(&w,v+i,4);rsx_test_fp_put_word(p+i*4,w);}
}
static void probe(unsigned operand,unsigned modifier,int absolute,int negate,
                  const float input[4],const float expected[4],const char*label)
{
    u8*p=ldh_main+0x1000;
    ins(p,1,10,2u|SW,0,0,0);constant(p+16,input);p+=32;
    u32 args[3]={src(9),src(9),src(9)}; /* r9 remains zero. */
    args[operand]=src(10)|(negate?(1u<<17):0);
    if(absolute)args[operand]|=1u<<(operand?18:29);
    args[1]|=modifier<<(19+3*operand);
    ins(p,operand==0?1:operand==1?3:4,11,args[0],args[1],args[2],0);p+=16;
    ins(p,0x0e,0,src(11),2u|SW,0,1);constant(p+16,expected);
    ldh_surface(FMT,0,64,0x100000,64,16,16);
    ldh_setup_quad(0,1,1,0,0);ldh_draw(0x1000,0x40,0,0,0x207);
    unsigned char rgb[64*64*3];u32 w,h;
    int read=ldh_read_rgb(0,"fp-precision.ppm",rgb,64,64,&w,&h);unsigned bad=0;
    if(!read)for(u32 i=0;i<w*h*3;i++)if(rgb[i])bad++;
    printf("operand=%u mode=%u abs=%d neg=%d mismatched_channels=%u\n",operand,modifier,absolute,negate,bad);
    ldh_check(label,!read&&!bad);
}
int main(void)
{
    if(ldh_init("FP source precision"))return 2;
    const float input[4]={-3,-.25f,1.25f,.75f};
    const float sat[4]={0,0,1,.75f},sn[4]={0,0,-1,-.75f};
    const float fixed12[4]={-2,-.25f,1.25f,.75f};
    const float fixed9[4]={-1,-.25f,1,.75f};
    for(unsigned s=0;s<3;s++) {
        probe(s,0,0,0,input,input,"unmodified operand");
        probe(s,4,0,0,input,sat,"saturate source before arithmetic");
        probe(s,4,1,0,input,sat,"saturate precedes absolute value");
        probe(s,4,1,1,input,sn,"negation follows saturation and absolute value");
        probe(s,2,0,0,input,fixed12,"fixed12 source range");
        probe(s,3,0,0,input,fixed9,"fixed9 source range");
    }
    int result=ldh_done("FP source precision");rsx_live_draw_shutdown();return result;
}
