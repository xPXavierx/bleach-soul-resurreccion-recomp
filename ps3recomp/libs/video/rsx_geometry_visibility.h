/* Optional native compute -> index-buffer boundary. All inputs, pipeline
 * objects and output storage are submission-owned through the GPU fence.
 * Output enters and leaves in UAV state; no CPU count/readback is involved.
 * A draw consuming the generated index view must occur between begin/end.
 * The producer must provide vertices in ORIGINAL index order, not dc.refs
 * expanded/compacted order. Zero triangles means this extension is absent.
 * This is an integration component; the guest producer is not connected yet. */
#ifndef RSX_GEOMETRY_VISIBILITY_H
#define RSX_GEOMETRY_VISIBILITY_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct rsx_geometry_visibility {
    ID3D12PipelineState* pipeline;
    ID3D12RootSignature* root;
    ID3D12Resource* output;
    D3D12_GPU_VIRTUAL_ADDRESS input_address, output_address;
    uint64_t output_offset, output_capacity;
    uint32_t triangles;
} rsx_geometry_visibility;
static int rsx_geometry_visibility_valid(const rsx_geometry_visibility* p) {
    return p && p->pipeline && p->root && p->output && p->input_address &&
        p->output_address && !(p->input_address&15) && !(p->output_offset&15) &&
        p->triangles && p->triangles<=65535 &&
        p->output_offset<=p->output_capacity &&
        (uint64_t)p->triangles*16<=p->output_capacity-p->output_offset &&
        p->output_address<=UINT64_MAX-p->output_offset;
}
#if defined(__cplusplus) && !defined(CINTERFACE)
#define RSX_GV_CALL(l,method,...) (l)->method(__VA_ARGS__)
#else
#define RSX_GV_CALL(l,method,...) (l)->lpVtbl->method((l),__VA_ARGS__)
#endif
static D3D12_INDEX_BUFFER_VIEW rsx_geometry_visibility_begin(
    const rsx_geometry_visibility* p, ID3D12GraphicsCommandList* list) {
    /* Unsupported jobs must have chosen the ordinary path before recording. */
    if(!rsx_geometry_visibility_valid(p))abort();
    RSX_GV_CALL(list,SetPipelineState,p->pipeline);
    RSX_GV_CALL(list,SetComputeRootSignature,p->root);
    RSX_GV_CALL(list,SetComputeRootShaderResourceView,0,p->input_address);
    RSX_GV_CALL(list,SetComputeRootUnorderedAccessView,1,p->output_address+p->output_offset);
    RSX_GV_CALL(list,Dispatch,(p->triangles+63)/64,1,1);
    D3D12_RESOURCE_BARRIER b;memset(&b,0,sizeof(b));b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=p->output;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter=D3D12_RESOURCE_STATE_INDEX_BUFFER;
    RSX_GV_CALL(list,ResourceBarrier,1,&b);
    D3D12_INDEX_BUFFER_VIEW view;memset(&view,0,sizeof(view));view.BufferLocation=p->output_address+p->output_offset;
    view.SizeInBytes=p->triangles*12;view.Format=DXGI_FORMAT_R32_UINT;return view;
}
static void rsx_geometry_visibility_end(
    const rsx_geometry_visibility* p, ID3D12GraphicsCommandList* list) {
    D3D12_RESOURCE_BARRIER b;memset(&b,0,sizeof(b));b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=p->output;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_INDEX_BUFFER;
    b.Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RSX_GV_CALL(list,ResourceBarrier,1,&b);
}
#undef RSX_GV_CALL
#endif
