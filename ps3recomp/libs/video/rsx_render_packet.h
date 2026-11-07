#ifndef PS3RECOMP_RSX_RENDER_PACKET_H
#define PS3RECOMP_RSX_RENDER_PACKET_H
#include <stdint.h>
/* Only ordinary NV4097 packets are eligible. Ordered transport operations,
 * reports, subchannel routing and mapping boundaries stay with the FIFO owner.
 * No borrowed pointer may survive the synchronous dispatch call. */
static inline int rsx_render_packet_valid(uint32_t method,uint32_t count,int increment) {
    if(!count || count>2047 || (method&3) || method>0x1ffc)return 0;
    uint32_t last=method+(increment?(count-1)*4:0);
    if(last>0x1ffc)return 0;
    static const uint32_t controls[]={0x50,0x64,0x68,0x6c,0x104,0x180,0x1a4,0x1a8,0x1800,0x1d6c,0x1d70,0x1d74};
    for(unsigned i=0;i<sizeof(controls)/sizeof(*controls);i++)
        if(method<=controls[i] && last>=controls[i])return 0;
    return 1;
}
#endif
