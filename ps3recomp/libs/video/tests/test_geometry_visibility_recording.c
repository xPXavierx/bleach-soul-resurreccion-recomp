/* Contract test: real native draw recorder, mocked D3D command sink.
 * Checks compute/graphics ordering, PSO cache invalidation and GPU index view. */
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
static unsigned state;
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"FAIL line%d state%u\n",__LINE__,state);abort();} }while(0)
static void STDMETHODCALLTYPE pso(ID3D12GraphicsCommandList*l,ID3D12PipelineState*p){
 (void)l;REQUIRE((state==0&&p==(void*)1)||(state==6&&p==(void*)2));++state;
}
static void STDMETHODCALLTYPE compute_root(ID3D12GraphicsCommandList*l,ID3D12RootSignature*p){(void)l;REQUIRE(state++==1&&p==(void*)3);}
static void STDMETHODCALLTYPE srv(ID3D12GraphicsCommandList*l,UINT n,D3D12_GPU_VIRTUAL_ADDRESS a){(void)l;REQUIRE(state++==2&&n==0&&a==4096);}
static void STDMETHODCALLTYPE uav(ID3D12GraphicsCommandList*l,UINT n,D3D12_GPU_VIRTUAL_ADDRESS a){(void)l;REQUIRE(state++==3&&n==1&&a==12544);}
static void STDMETHODCALLTYPE dispatch(ID3D12GraphicsCommandList*l,UINT x,UINT y,UINT z){(void)l;REQUIRE(state++==4&&x==1&&y==1&&z==1);}
static void STDMETHODCALLTYPE barrier(ID3D12GraphicsCommandList*l,UINT n,const D3D12_RESOURCE_BARRIER*b){
 (void)l;REQUIRE(n==1&&b->Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION&&b->Transition.pResource==(void*)4);
 if(state==5)REQUIRE(b->Transition.StateBefore==D3D12_RESOURCE_STATE_UNORDERED_ACCESS&&b->Transition.StateAfter==D3D12_RESOURCE_STATE_INDEX_BUFFER);
 else REQUIRE(state==9&&b->Transition.StateBefore==D3D12_RESOURCE_STATE_INDEX_BUFFER&&b->Transition.StateAfter==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 ++state;
}
static void STDMETHODCALLTYPE index_buffer(ID3D12GraphicsCommandList*l,const D3D12_INDEX_BUFFER_VIEW*v){(void)l;REQUIRE(state++==7&&v->BufferLocation==12544&&v->SizeInBytes==24&&v->Format==DXGI_FORMAT_R32_UINT);}
static void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList*l,UINT count,UINT instances,UINT first,INT base,UINT instance){(void)l;REQUIRE(state++==8&&count==6&&instances==1&&!first&&!base&&!instance);}
static void STDMETHODCALLTYPE cbv(ID3D12GraphicsCommandList*l,UINT n,D3D12_GPU_VIRTUAL_ADDRESS a){(void)l;(void)n;(void)a;REQUIRE(state==7);}
static void STDMETHODCALLTYPE table(ID3D12GraphicsCommandList*l,UINT n,D3D12_GPU_DESCRIPTOR_HANDLE a){(void)l;(void)n;(void)a;REQUIRE(state==7);}
int main(void){
 _putenv("RSX_NATIVE_BINDINGS=1");
 ID3D12GraphicsCommandListVtbl v={0};v.SetPipelineState=pso;v.SetComputeRootSignature=compute_root;
 v.SetComputeRootShaderResourceView=srv;v.SetComputeRootUnorderedAccessView=uav;
 v.Dispatch=dispatch;v.ResourceBarrier=barrier;v.IASetIndexBuffer=index_buffer;v.DrawIndexedInstanced=draw;
 v.SetGraphicsRootConstantBufferView=cbv;v.SetGraphicsRootDescriptorTable=table;
 ID3D12GraphicsCommandList list={&v};rsx_native_draw_packet p={0};rsx_native_bindings b={0};
 p.pso=(void*)2;p.topology=D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
 b.valid=~0u;b.pso=p.pso;b.topology=p.topology;
 p.geometry.pipeline=(void*)1;p.geometry.root=(void*)3;p.geometry.output=(void*)4;
 p.geometry.input_address=4096;p.geometry.output_address=12288;p.geometry.output_offset=256;
 p.geometry.output_capacity=512;p.geometry.triangles=2;
 p.count=999; /* CPU count must not accidentally control this native draw. */
 rsx_native_draw_record(&p,&list,&b);REQUIRE(state==10);
 puts("PASS production recorder: compute -> UAV/index transition -> forced graphics PSO -> GPU index view -> indexed draw -> UAV restore; no CPU index/count read");return 0;
}
