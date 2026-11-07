/* Insert the host identity BEFORE the original 0x2A00 command finalizer.
 * It remains an ordinary NV4097 NOP for other consumers. No buffer grows;
 * insufficient padding or an unfamiliar command grammar rejects unchanged.
 */
#ifndef BLEACH_NATIVE_GEOMETRY_MARKER_H
#define BLEACH_NATIVE_GEOMETRY_MARKER_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "rsx_geometry_handoff.h"
static uint32_t bleach_marker_read(const unsigned char* p) {
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static void bleach_marker_write(unsigned char* p,uint32_t x) {
    p[0]=(unsigned char)(x>>24);p[1]=(unsigned char)(x>>16);p[2]=(unsigned char)(x>>8);p[3]=(unsigned char)x;
}
static int bleach_geometry_marker_insert(unsigned char* ls,size_t size,uint32_t descriptor,
    uint32_t info,uint32_t ea,uint32_t extra,rsx_geometry_lease* lease) {
    if(!lease || !lease->token || !ea || (info&15) || descriptor>size || size-descriptor<416 ||
       info>size || size-info<16 || extra>1)return 0;
    uint32_t first=bleach_marker_read(ls+info),end=bleach_marker_read(ls+info+4),cur=bleach_marker_read(ls+info+8);
    if((first|end|cur)&3 || (first&15) || first>cur || cur>end || end>size || (first&127)>112 ||
       (first&127)!=(ea&127) || (uint64_t)ea+(end-first)>UINT32_MAX ||
       !ls[descriptor+1] || end-first!=(uint32_t)ls[descriptor+1]*16)return 0;
    /* Finalizer may split each packet at 128-byte boundaries, inserting a
     * four-byte header per crossing, then append one 16-byte semaphore. */
    size_t payload=(size_t)(cur-first)+16+(size_t)extra*16;
    size_t worst=payload+4*((payload+127)/124+1);
    if(worst>end-first || (info<end && first<info+16) ||
       (descriptor<end && first<descriptor+416))return 0;
    unsigned begins=0,ends=0,selected=0,active=0;
    for(uint32_t at=first;at<cur;) {
        uint32_t h=bleach_marker_read(ls+at),type=h>>29,n=(h>>18)&2047,m=h&8191;
        if((type!=0 && type!=2) || !n || (h&0x1e003)!=0 || n>(cur-at-4)/4)return 0;
        for(unsigned k=0;k<n;k++) {
            uint32_t method=m+(type==0?k*4:0),arg=bleach_marker_read(ls+at+4+k*4);
            if(method==0x1808) {
                if(arg==5 && !begins && !ends){begins++;active=1;}
                else if(arg==0 && active){ends++;active=0;}
                else return 0;
            } else if(method==0x1824) {
                if(!active || (arg&0xffffff)!=selected)return 0;
                selected+=(arg>>24)+1;
            } else if(method==0x1814 || method==0x1818)return 0;
        }
        at+=4+n*4;
    }
    if(begins!=1 || ends!=1 || active || !selected || selected!=lease->selected_indices)return 0;
    memmove(ls+first+16,ls+first,cur-first);
    bleach_marker_write(ls+first,RSX_GEOMETRY_MARKER_HEADER);
    bleach_marker_write(ls+first+4,RSX_GEOMETRY_MARKER_MAGIC);
    bleach_marker_write(ls+first+8,(uint32_t)(lease->token>>32));
    bleach_marker_write(ls+first+12,(uint32_t)lease->token);
    bleach_marker_write(ls+info+8,cur+16);
    lease->command_ea=ea;return 1;
}
#endif
