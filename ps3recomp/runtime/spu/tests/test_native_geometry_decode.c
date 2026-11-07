#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "native_geometry_decode.h"
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;} } while(0)
static unsigned char* boundary(size_t bytes,void** allocation) {
    SYSTEM_INFO s;GetSystemInfo(&s);
    unsigned char* p=(unsigned char*)VirtualAlloc(0,2*s.dwPageSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!p)return 0;DWORD old;
    if(!VirtualProtect(p+s.dwPageSize,s.dwPageSize,PAGE_NOACCESS,&old)){VirtualFree(p,0,MEM_RELEASE);return 0;}
    *allocation=p;return p+s.dwPageSize-bytes;
}
int main(void) {
    void *positions_owner=0,*packed_owner=0;
    unsigned char* positions=boundary(12,&positions_owner);
    unsigned char* packed=boundary(4,&packed_owner);
    CHECK(positions && packed);
    unsigned char out[2][16],saved[sizeof(out)];
    const uint32_t xyz[3]={0x3f800000,0xc0000000,0x40400000};
    for(unsigned i=0;i<3;i++)ps3_geo_write_be32(positions+4*i,xyz[i]);
    ps3_geo_write_be32(packed,0x8103d804);
    ps3_geometry_decode_job j={0};
    j.xyz_f32_be=(ps3_geometry_input){positions,12};j.packed_vectors_be=(ps3_geometry_input){packed,4};
    j.vectors[0]=(ps3_geometry_output){out[0],16};j.vectors[1]=(ps3_geometry_output){out[1],16};
    j.vertices=1;j.packed_count=1;j.packed_stride=4;
    _mm_setcsr((_mm_getcsr()&~0x6000)|0xffc0);
    // Golden packed direction taken from the original title's recorded output.
    const uint32_t direction[4]={0x3b901202,0x3df71ee3,0xbf7dff7f,0x3f800000};
    for(int big=0;big<2;big++) {
        j.output_big_endian=big;CHECK(ps3_geometry_decode(&j));
        for(unsigned k=0;k<4;k++) {
            uint32_t a,b;memcpy(&a,out[0]+k*4,4);memcpy(&b,out[1]+k*4,4);
            if(big){a=__builtin_bswap32(a);b=__builtin_bswap32(b);}
            CHECK(a==(k<3?xyz[k]:0x3f800000));CHECK(b==direction[k]);
        }
    }
    ps3_geo_write_be32(packed,0x00000400);CHECK(ps3_geometry_decode(&j));
    CHECK(ps3_geo_read_be32(out[1])==0xbf7fffff);
    CHECK(ps3_geo_read_be32(out[1]+4)==0x3a001002);
    CHECK(ps3_geo_read_be32(out[1]+8)==0x3a802008);
    memset(out,0xa5,sizeof(out));memcpy(saved,out,sizeof(out));
    for(unsigned bad=0;bad<8;bad++) {
        ps3_geometry_decode_job b=j;
        switch(bad) {
            case 0:b.vertices=0;break;
            case 1:b.xyz_f32_be.bytes=11;break;
            case 2:b.packed_vectors_be.bytes=3;break;
            case 3:b.vectors[1].bytes=15;break;
            case 4:b.vectors[1]=b.vectors[0];break;
            case 5:b.packed_offset[0]=1;break;
            case 6:b.packed_count=4;break;
            case 7:b.vectors[1].data=packed;break;
        }
        CHECK(!ps3_geometry_decode(&b));CHECK(!memcmp(out,saved,sizeof(out)));
    }
    CHECK(ps3_geo_read_be32(packed)==0x00000400);
    VirtualFree(positions_owner,0,MEM_RELEASE);VirtualFree(packed_owner,0,MEM_RELEASE);
    puts("PASS geometry decoder: guard-page input bounds, golden vectors, host/BE outputs, eight atomic rejection cases");
    return 0;
}
