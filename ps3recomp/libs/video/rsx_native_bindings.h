/* State owned by exactly one recording D3D12 command list. All callers that
 * modify these states (including auxiliary graphics/compute passes) use this
 * emitter. Reset invalidates every state. Dynamic root arguments and buffers
 * are deliberately still emitted on every draw. No draw order is changed. */
#ifndef RSX_NATIVE_BINDINGS_H
#define RSX_NATIVE_BINDINGS_H
typedef struct rsx_native_bindings {
    u32 valid;
    ID3D12PipelineState* pso;
    ID3D12RootSignature* root;
    ID3D12DescriptorHeap* heaps[2];
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4], dsv;
    u32 n_rtvs, has_dsv, stencil;
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
    D3D_PRIMITIVE_TOPOLOGY topology;
    FLOAT blend[4];
} rsx_native_bindings;
static u64 g_native_binding_emitted, g_native_binding_elided;
static int rsx_nb_enabled(void)
{
    static int enabled=-1;
    if(enabled<0) { const char* e=getenv("RSX_NATIVE_BINDINGS"); enabled=e && e[0]=='1'; }
    return enabled;
}
static int rsx_nb_change(rsx_native_bindings* b, u32 bit, int equal)
{
    if(rsx_nb_enabled() && (b->valid&bit) && equal) { ++g_native_binding_elided; return 0; }
    b->valid|=bit; ++g_native_binding_emitted; return 1;
}
static void rsx_nb_pso(rsx_native_bindings* b, ID3D12GraphicsCommandList* l, ID3D12PipelineState* p)
{ if(rsx_nb_change(b,1,b->pso==p)) { l->lpVtbl->SetPipelineState(l,p); b->pso=p; } }
static void rsx_nb_root(rsx_native_bindings* b, ID3D12GraphicsCommandList* l, ID3D12RootSignature* p)
{ if(rsx_nb_change(b,2,b->root==p)) { l->lpVtbl->SetGraphicsRootSignature(l,p); b->root=p; } }
static void rsx_nb_heaps(rsx_native_bindings* b, ID3D12GraphicsCommandList* l, ID3D12DescriptorHeap* const* p)
{
    if(rsx_nb_change(b,4,b->heaps[0]==p[0] && b->heaps[1]==p[1])) {
        l->lpVtbl->SetDescriptorHeaps(l,2,p); memcpy(b->heaps,p,sizeof(b->heaps));
    }
}
static void rsx_nb_targets(rsx_native_bindings* b, ID3D12GraphicsCommandList* l,
    u32 count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
    int equal=count==b->n_rtvs && !!dsv==b->has_dsv &&
        (!dsv || dsv->ptr==b->dsv.ptr) && !memcmp(b->rtvs,rtvs,count*sizeof(*rtvs));
    if(rsx_nb_change(b,8,equal)) {
        l->lpVtbl->OMSetRenderTargets(l,count,rtvs,FALSE,dsv);
        b->n_rtvs=count; b->has_dsv=!!dsv;
        if(dsv)b->dsv=*dsv;
        memcpy(b->rtvs,rtvs,count*sizeof(*rtvs));
    }
}
static void rsx_nb_viewport(rsx_native_bindings* b, ID3D12GraphicsCommandList* l, const D3D12_VIEWPORT* p)
{ if(rsx_nb_change(b,16,!memcmp(&b->viewport,p,sizeof(*p)))) { l->lpVtbl->RSSetViewports(l,1,p); b->viewport=*p; } }
static void rsx_nb_scissor(rsx_native_bindings* b, ID3D12GraphicsCommandList* l, const D3D12_RECT* p)
{ if(rsx_nb_change(b,32,!memcmp(&b->scissor,p,sizeof(*p)))) { l->lpVtbl->RSSetScissorRects(l,1,p); b->scissor=*p; } }
static void rsx_nb_topology(rsx_native_bindings* b, ID3D12GraphicsCommandList* l, D3D_PRIMITIVE_TOPOLOGY p)
{ if(rsx_nb_change(b,64,b->topology==p)) { l->lpVtbl->IASetPrimitiveTopology(l,p); b->topology=p; } }
static void rsx_nb_stencil(rsx_native_bindings* b, ID3D12GraphicsCommandList* l, u32 p)
{ if(rsx_nb_change(b,128,b->stencil==p)) { l->lpVtbl->OMSetStencilRef(l,p); b->stencil=p; } }
static void rsx_nb_blend(rsx_native_bindings* b, ID3D12GraphicsCommandList* l, const FLOAT* p)
{ if(rsx_nb_change(b,256,!memcmp(b->blend,p,sizeof(b->blend)))) { l->lpVtbl->OMSetBlendFactor(l,p); memcpy(b->blend,p,sizeof(b->blend)); } }
#endif
