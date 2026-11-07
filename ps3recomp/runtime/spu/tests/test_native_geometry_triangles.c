#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "native_geometry_triangles.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
static unsigned char* boundary(size_t bytes,void** allocation) {
    SYSTEM_INFO s;GetSystemInfo(&s);
    unsigned char* p=VirtualAlloc(0,2*s.dwPageSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!p)return 0;DWORD old;
    if(!VirtualProtect(p+s.dwPageSize,s.dwPageSize,PAGE_NOACCESS,&old))return 0;
    *allocation=p;return p+s.dwPageSize-bytes;
}
int main(void) {
    void* owners[3]={0};unsigned char* data[3];const unsigned sizes[3]={64,64,32};
    for(unsigned k=0;k<3;k++){data[k]=boundary(sizes[k],&owners[k]);CHECK(data[k]);memset(data[k],0,sizes[k]);}
    const float vertices[4][4]={{0,0,0,1},{0,1,0,1},{1,0,0,1},{10,10,0,1}};
    for(unsigned k=0;k<4;k++){u128 v;memcpy(&v,vertices[k],16);ps3_skin4_store(data[0]+k*16,v);}
    const unsigned char input[32]={0,0,0,1,0,2, 0,0,0,2,0,1, 0,0,0,0,0,1, 0,3,0,3,0,3};
    memcpy(data[2],input,32);
    ps3_geometry_triangles_job j={0};j.projection.vertices=4;
    j.projection.positions=(ps3_geometry_input){data[0],64};j.projection.projected=(ps3_geometry_output){data[1],64};
    j.indices=(ps3_geometry_output){data[2],32};j.index_count=12;j.cull=1;j.quantization_scale=1;
    for(unsigned k=0;k<3;k++)j.projection.world_view[k]._f32[k]=1;
    for(unsigned k=0;k<4;k++)j.projection.projection[k]._f32[k]=1;
    for(unsigned k=0;k<3;k++){j.projection.viewport_scale._f32[k]=1;j.projection.minimum._f32[k]=-2;j.projection.maximum._f32[k]=2;}
    _mm_setcsr((_mm_getcsr()&~0x6000)|0xffc0);
    unsigned char saved[160];memcpy(saved,data[0],64);memcpy(saved+64,data[1],64);memcpy(saved+128,data[2],32);
    for(unsigned bad=0;bad<10;bad++) {
        ps3_geometry_triangles_job b=j;ps3_geometry_triangles_result result={777,888,999};
        switch(bad) {
            case 0:b.projection.vertices=3;break;
            case 1:b.projection.positions.bytes=63;break;
            case 2:b.projection.projected.bytes=63;break;
            case 3:b.indices.bytes=31;break;
            case 4:b.indices.data=data[0];break;
            case 5:b.projection.projected.data=data[0];break;
            case 6:b.index_count=11;break;
            case 7:b.cull=3;break;
            case 8:b.quantization_scale=3;break;
            case 9:data[2][1]=4;break;
        }
        CHECK(!ps3_geometry_triangles(&b,&result));if(bad==9)data[2][1]=0;
        CHECK(result.index_count==777 && result.aligned_index_bytes==888 && result.written_index_bytes==999);
        CHECK(!memcmp(saved,data[0],64) && !memcmp(saved+64,data[1],64) && !memcmp(saved+128,data[2],32));
    }
    for(unsigned cull=0;cull<3;cull++)for(unsigned scale=1;scale<=2;scale++) {
        memcpy(data[2],input,32);j.cull=cull;j.quantization_scale=scale;ps3_geometry_triangles_result result;
        CHECK(ps3_geometry_triangles(&j,&result));CHECK(result.index_count==(cull?3:6));
        CHECK(result.aligned_index_bytes==16 && result.written_index_bytes==32);
        CHECK(!memcmp(data[2],input+(cull==2?6:0),result.index_count*2));
        for(unsigned k=result.index_count*2;k<32;k++)CHECK(data[2][k]==0);
        CHECK(ps3_geo_read_be32(data[1])==0 && ps3_geo_read_be32(data[1]+4)==0);
        CHECK(ps3_geo_read_be32(data[1]+8)==2 && ps3_geo_read_be32(data[1]+12)==0);
        CHECK((ps3_geo_read_be32(data[1]+56)&160)==160);
        CHECK(!memcmp(saved,data[0],64));
    }
    for(unsigned k=0;k<3;k++)VirtualFree(owners[k],0,MEM_RELEASE);
    puts("PASS triangles: guard pages; independent winding, no-cull, quantization, common clipping and degeneracy goldens; ten atomic rejections");
    return 0;
}
