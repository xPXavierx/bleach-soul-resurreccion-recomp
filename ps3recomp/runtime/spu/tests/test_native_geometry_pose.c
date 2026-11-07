#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "native_geometry_pose.h"
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;} } while(0)
static unsigned char* boundary(size_t bytes,void** allocation) {
    SYSTEM_INFO s;GetSystemInfo(&s);
    unsigned char* p=(unsigned char*)VirtualAlloc(0,2*s.dwPageSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!p)return 0;DWORD old;
    if(!VirtualProtect(p+s.dwPageSize,s.dwPageSize,PAGE_NOACCESS,&old)){VirtualFree(p,0,MEM_RELEASE);return 0;}
    *allocation=p;return p+s.dwPageSize-bytes;
}
int main(void) {
    void* owners[5]={0};const size_t sizes[5]={48,32,64,192,128};
    unsigned char* data[5];
    for(unsigned i=0;i<5;i++){data[i]=boundary(sizes[i],&owners[i]);CHECK(data[i]);memset(data[i],0,sizes[i]);}
    // Independently chosen identity matrix with translation2,3,4; original
    // u8 weight255*0x3b808080 rounds toward zero to0x3f7fffff, not1.0.
    const uint32_t matrix[12]={0x3f800000,0,0,0x40000000,0,0x3f800000,0,0x40400000,0,0,0x3f800000,0x40800000};
    for(unsigned i=0;i<12;i++)ps3_geo_write_be32(data[0]+i*4,matrix[i]);
    const uint32_t position[4]={0x3f800000,0x40000000,0x40400000,0x3f800000};
    for(unsigned v=0;v<4;v++) {
        data[1][v*8]=255;
        for(unsigned k=0;k<4;k++)ps3_geo_write_be32(data[2]+v*16+k*4,position[k]);
    }
    for(unsigned v=0;v<8;v++) {
        ps3_geo_write_be32(data[4]+v*16,0x3f800000);
        ps3_geo_write_be32(data[4]+v*16+12,0x3f800000);
    }
    ps3_geometry_pose_job job={0};job.palette=(ps3_geometry_output){data[0],48};
    job.influences=(ps3_geometry_input){data[1],32};job.positions=(ps3_geometry_output){data[2],64};
    job.frames=(ps3_geometry_output){data[3],192};job.directions[0]=(ps3_geometry_output){data[4],128};
    job.palette_entries=1;job.vertices=4;job.normalize_vertices=8;job.chunk_vertices=4;
    job.direction_count=1;job.tail_start=4;
    _mm_setcsr((_mm_getcsr()&~0x6000)|0xffc0);
    unsigned char saved[464];size_t offset=0;
    for(unsigned i=0;i<5;i++){memcpy(saved+offset,data[i],sizes[i]);offset+=sizes[i];}
    for(unsigned bad=0;bad<9;bad++) {
        ps3_geometry_pose_job b=job;
        switch(bad) {
            case 0:b.vertices=0;break;
            case 1:b.palette.bytes=47;break;
            case 2:b.influences.bytes=31;break;
            case 3:b.frames.bytes=191;break;
            case 4:b.frames.data=data[2];break;
            case 5:b.directions[0].bytes=127;break;
            case 6:b.normalize_vertices=4;break;
            case 7:b.tail_start=0;break; // Unspecified explicit matrix sources.
            case 8:b.chunk_vertices=3;break;
        }
        CHECK(!ps3_geometry_pose(&b));offset=0;
        for(unsigned i=0;i<5;i++){CHECK(!memcmp(saved+offset,data[i],sizes[i]));offset+=sizes[i];}
    }
    data[1][1]=1;CHECK(!ps3_geometry_pose(&job));data[1][1]=0;
    offset=0;for(unsigned i=0;i<5;i++){CHECK(!memcmp(saved+offset,data[i],sizes[i]));offset+=sizes[i];}
    CHECK(ps3_geometry_pose(&job));
    const uint32_t expected[4]={0x403fffff,0x409fffff,0x40dfffff,0x3f800000};
    for(unsigned v=0;v<4;v++)for(unsigned k=0;k<4;k++)CHECK(ps3_geo_read_be32(data[2]+v*16+k*4)==expected[k]);
    for(unsigned v=0;v<8;v++) {
        u128 n=ps3_skin4_load(data[4]+v*16);
        CHECK(n._f32[0]>0.999f && n._f32[0]<1.001f);
        CHECK(n._u32[1]==0 && n._u32[2]==0 && n._u32[3]==0x3f800000);
    }
    for(unsigned i=0;i<5;i++)VirtualFree(owners[i],0,MEM_RELEASE);
    puts("PASS pose: guard-page spans, independent position golden, direction length/w, ten atomic rejection cases");
    return 0;
}
