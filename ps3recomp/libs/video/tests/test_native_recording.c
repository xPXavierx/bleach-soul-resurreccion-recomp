/* Exercise the production ordered queue with a deliberately slow recorder.
 * Stack inputs are overwritten immediately; the queue must retain their bytes,
 * preserve order across wrap/backpressure, drain and stop/restart cleanly.
 * No GPU completion is simulated by these CPU-recording assertions. */
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3emu/ps3types.h"
#include "rsx_native_bindings.h"
#include "rsx_native_draw_packet.h"
typedef struct ld_frame_resources {
    ID3D12GraphicsCommandList* list;
    rsx_native_bindings bindings;
} ld_frame_resources;
#include "rsx_native_recording.h"

static u32 next_draw, current_value;
static void STDMETHODCALLTYPE constants(ID3D12GraphicsCommandList* l,UINT index,UINT count,
    const void* values,UINT offset)
{
    (void)l;const u32* words=values;
    if(index!=3 || count!=4 || offset!=2 || words[0]!=next_draw ||
       words[1]!=(next_draw^0xdeadbeefu) || words[2]!=~next_draw || words[3]!=17)abort();
    current_value=words[0];
}
static void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList* l,UINT count,UINT instances,
    UINT first,UINT start_instance)
{
    (void)l;
    if(count!=next_draw || current_value!=next_draw || instances!=2 ||
       first!=next_draw+1 || start_instance!=next_draw+2)abort();
    if(!(next_draw%1000))Sleep(1); /* guarantee pressure on a 32-entry queue */
    ++next_draw;
}
int main(void)
{
    ID3D12GraphicsCommandListVtbl vtable={0};
    vtable.SetGraphicsRoot32BitConstants=constants;vtable.DrawInstanced=draw;
    ID3D12GraphicsCommandList list={&vtable};ld_frame_resources frame={&list,{0}};
    _putenv("RSX_RECORD_QUEUE_CAPACITY=32");
    for(u32 mode=0;mode<2;++mode)for(u32 round=0;round<2;++round) {
        _putenv(mode?"RSX_NATIVE_RECORDING=1":"RSX_NATIVE_RECORDING=0");
        next_draw=0;current_value=~0u;rsx_rec_initialize();
        for(u32 i=0;i<10000;i++) {
            u32 words[4]={i,i^0xdeadbeefu,~i,17};
            rsx_rec_SetGraphicsRoot32BitConstants(&frame,3,4,words,2);
            memset(words,0xa5,sizeof(words));
            rsx_rec_DrawInstanced(&frame,i,2,i+1,i+2);
        }
        rsx_rec_drain();
        if(next_draw!=10000 || (mode && (!g_rs_rec.full_waits || g_rs_rec.head!=20000 ||
            rsx_rec_load(&g_rs_rec.consumed)!=20000)))return 2;
        rsx_rec_shutdown();
        printf("PASS mode=%u restart=%u:10000 ordered draws, copied stack inputs, drain%s\n",
            mode,round,mode?", wrap/backpressure and worker shutdown":"");
    }
    return 0;
}
