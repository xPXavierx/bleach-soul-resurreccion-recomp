#include "rsx_commands.h"
#include "rsx_dispatch.h"
#include "rsx_render_packet.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint32_t ppu_hle_inject_base=0x03000000;
static u32 addresses[16],values[16],writes;
void vm_write32(uint32_t a,uint32_t v){assert(writes<16);addresses[writes]=a;values[writes++]=v;}
void ps3_ms(const char* key){(void)key;}
u32 gcm_io_offset_to_ea(u32 io){return io+0x10000000u;}
unsigned long long ps3_ms_now(void){return 1234;}
extern u32 g_rsx_last_reference;
static rsx_dispatch a,b;
static unsigned event_count[2];static uint64_t events[2];
static void event(void* user,const rsx_dispatch* r,u32 x,u32 y){unsigned n=(unsigned)(uintptr_t)user;event_count[n]++;events[n]=events[n]*1099511628211ull+x+((uint64_t)y<<32)+r->current_primitive;}
static void clear(void* u,const rsx_dispatch* r,u32 mask){event(u,r,mask,0);}
static void begin(void* u,const rsx_dispatch* r,u32 prim){event(u,r,prim,1);}
static void end(void* u,const rsx_dispatch* r){event(u,r,0,2);}
static void inline_data(void* u,const rsx_dispatch* r,const u8* p,u32 n){event(u,r,n,3);for(u32 i=0;i<n;i++)event(u,r,p[i],4);}
static void packet(u32 m,const u32* words,u32 n,int inc){
 unsigned char be[8192];assert(n<=2047);
 for(u32 i=0;i<n;i++){u32 v=words[i];be[4*i]=v>>24;be[4*i+1]=v>>16;be[4*i+2]=v>>8;be[4*i+3]=v;rsx_dispatch_method(&a,m+(inc?4*i:0),v);}
 rsx_dispatch_packet(&b,m,be,n,inc);
 rsx_dispatch_sink saved=b.sink;b.sink=a.sink;
 assert(!memcmp(&a,&b,sizeof a));b.sink=saved;
 assert(event_count[0]==event_count[1] && events[0]==events[1]);
}
int main(void){
 assert(rsx_process_control_method(0x50,0x12345678));assert(g_rsx_last_reference==0x12345678);
 assert(!rsx_process_control_method(0x1a00,0x12345678));
 rsx_process_control_method(0x1a4,0xfeed0001);rsx_process_control_method(0x1d6c,0x40);
 rsx_process_control_method(0x1d70,0x11223344);rsx_process_control_method(0x1d74,0x11223344);
 assert(writes==2 && addresses[0]==0x10000040 && values[0]==0x11443322 && values[1]==0x11223344);
 rsx_process_control_method(0x1a4,0);rsx_process_control_method(0x1d74,0x1234);
 assert(addresses[2]==0x03000040 && values[2]==0x1234);
 rsx_process_control_method(0x180,0x6660420e);rsx_process_control_method(0x104,0);
 assert(writes==7 && addresses[3]==0x1f100040 && values[3]==0 && values[4]==1234000000 && values[5]==0 && values[6]==0);
 assert(!rsx_render_packet_valid(0x1d68,3,1));assert(!rsx_render_packet_valid(0x1a4,2047,0));
 assert(!rsx_render_packet_valid(0x1ffc,2,1));assert(!rsx_render_packet_valid(0,0,1));
 assert(rsx_render_packet_valid(0x1a00,32,1));assert(rsx_render_packet_valid(0x1818,2047,0));
 rsx_dispatch_sink sink={0};sink.clear=clear;sink.begin=begin;sink.end=end;sink.draw_arrays=event;sink.draw_index_array=event;sink.inline_array=inline_data;
 rsx_dispatch_init(&a,&sink);sink.user=(void*)1;rsx_dispatch_init(&b,&sink);
 u32 words[2047],seed=12345;
 for(unsigned round=0;round<1000;round++){
  for(unsigned i=0;i<2047;i++){seed=seed*1664525+1013904223;words[i]=seed;}
  packet(0x1e9c,(u32[]){0},1,0);packet(0x1f00,words,32,1);
  packet(0x1e9c,(u32[]){2},1,0);packet(0xb80,words,32,1);
  packet(0x1a00,words,128,1);packet(0x1808,(u32[]){5},1,0);
  packet(0x1814,words,8,0);packet(0x1824,words,8,0);packet(0x1818,words,63,0);
  packet(0x1808,(u32[]){0},1,0);packet(0x1d94,(u32[]){0xf3},1,0);
 }
 assert(event_count[0]>1000);printf("PASS: transport writes, endian/method bounds and 10000 ordered graphics packets (%u callbacks).\n",event_count[0]);return 0;
}
