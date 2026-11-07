/* Exercise production D3D12 view ownership, content versions and eviction.
 * No title data: a gradient surface is sampled through 64 rectangular aliases,
 * queued before a fence, then reused and rewritten. CPU readback is the oracle. */
#include "live_draw_test_harness.h"
#define FMT (0x8u | (0x2u<<5) | (0x1u<<8))
#define SOURCE 0x20000u
#define OUTPUT 0x40000u
#define Z 0x100000u
#define COLOR 0x1100u
#define TEX 0x1000u
static unsigned char source_rgb[256*256*3],output_rgb[256*256*3];
static u32 source_w,source_h;

static void draw_source(unsigned variant)
{
    ldh_surface(FMT,SOURCE,256,Z,256,64,64);
    ldh_setup_quad(0,1,1,0,0);
    for(unsigned i=0;i<4;i++) {
        float x=(i==1||i==2)?1.0f:0.0f,y=i>=2?1.0f:0.0f;
        ldh_wf(i*48+16,variant?1-x:x);
        ldh_wf(i*48+20,variant?1-y:y);
        ldh_wf(i*48+24,variant?0.75f:0.25f);
    }
    ldh_draw(COLOR,0x40,0,0,0x207);
    ldh_check("read source",!ldh_read_rgb(SOURCE,"views-source.ppm",source_rgb,256,256,&source_w,&source_h));
}
static void draw_views(void)
{
    ldh_surface(FMT,OUTPUT,256,Z+0x20000,256,64,64);
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
        ldh_setup_quad(0,1,1,0,0);
        for(unsigned i=0;i<4;i++) {
            ldh_wf(i*48, -1.0f+(x+(i==1||i==2))*0.25f);
            ldh_wf(i*48+4,1.0f-(y+(i>=2))*0.25f);
        }
        /* Odd extents keep pixel centres off exact point-sampling boundaries. */
        ldh_texture0(SOURCE,0xa5,3+x*6,5+y*6,256);
        ldh_draw(TEX,0x40,0,0,0x207);
    }
    u32 w,h;
    if(ldh_read_rgb(OUTPUT,"views-output.ppm",output_rgb,256,256,&w,&h)) {
        ldh_check("read output",0);return;
    }
    unsigned scale=source_w/64,bad=0,max_error=0;
    ldh_check("native dimensions",w==source_w&&h==source_h&&scale>0);
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++) {
        unsigned tx=x/(8*scale),ty=y/(8*scale);
        unsigned sx=((x%(8*scale))*2+1)*(3+tx*6)/16;
        unsigned sy=((y%(8*scale))*2+1)*(5+ty*6)/16;
        for(unsigned c=0;c<3;c++) {
            unsigned expected=source_rgb[(sy*source_w+sx)*3+c];
            unsigned actual=output_rgb[(y*w+x)*3+c];
            unsigned error=abs((int)actual-(int)expected);
            if(error>max_error)max_error=error;
            if(error>1)bad++;
        }
    }
    printf("pixels=%u mismatched_channels=%u max_error=%u\n",w*h,bad,max_error);
    ldh_check("all rectangular aliases match independent crop oracle",bad==0);
}
static rsx_surface_view_stats stats(const char* label)
{
    rsx_surface_view_stats s;rsx_live_draw_surface_view_stats(&s);
    printf("%s allocations=%llu copies=%llu evictions=%llu bytes=%llu budget=%llu legacy_allocations=%llu\n",
        label,(unsigned long long)s.allocations,(unsigned long long)s.copies,
        (unsigned long long)s.evictions,(unsigned long long)s.resident_bytes,
        (unsigned long long)s.budget_bytes,(unsigned long long)s.legacy_allocations);
    return s;
}
int main(void)
{
    if(ldh_init("surface views"))return 2;
    rsx_test_fp_tex_r0_tc0(ldh_main+TEX,0);
    rsx_test_fp_mov_r0_col0(ldh_main+COLOR,RSX_TEST_FP_SWZ_IDENT);
    draw_source(0);draw_views();rsx_surface_view_stats first=stats("warmup");
    draw_views();rsx_surface_view_stats second=stats("reuse");
    draw_source(1);draw_views();rsx_surface_view_stats third=stats("rewrite");
    if(first.allocations && !first.evictions) {
        ldh_check("reuse allocates no additional GPU images",second.allocations==first.allocations);
        ldh_check("unchanged content queues no additional GPU copies",second.copies==first.copies);
        ldh_check("changed source reuses every image",third.allocations==first.allocations);
        ldh_check("changed source recopies all views",third.copies==second.copies+64);
    } else if(first.evictions) {
        ldh_check("eviction exercised",third.evictions>first.evictions);
        ldh_check("resident memory respects configured budget",third.resident_bytes<=third.budget_bytes);
    } else {
        ldh_check("legacy churn control exercised",third.legacy_allocations>=192);
    }
    int result=ldh_done("surface views");rsx_live_draw_shutdown();return result;
}
