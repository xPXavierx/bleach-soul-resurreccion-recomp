/* Native recording boundary. Preparation resolves guest state/data before
 * constructing this packet. The recorder has no dispatcher, guest memory,
 * vertex scratch, cache lookup, allocation or ring-offset dependency.
 *
 * Values are owned by the packet; GPU addresses/descriptors/resources are
 * leased from the submission slot and persistent caches. The caller must
 * record before recycling those leases, then retain them through GPU
 * completion. This first integration is synchronous: auxiliary transfers
 * and cache invalidation must join the ordered protocol before deferral.
 */
#ifndef RSX_NATIVE_DRAW_PACKET_H
#define RSX_NATIVE_DRAW_PACKET_H
#include "rsx_geometry_visibility.h"

typedef struct rsx_native_draw_packet {
    ID3D12PipelineState* pso;
    ID3D12RootSignature* root;
    ID3D12DescriptorHeap* heaps[2];
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4], dsv;
    ID3D12Resource* sampled_targets[16];
    u32 target_count, sampled_target_count, have_dsv, clear_depth;
    D3D12_GPU_DESCRIPTOR_HANDLE textures, samplers, vertex_textures;
    D3D12_GPU_VIRTUAL_ADDRESS vertex_constants, pixel_constants;
    u32 have_pixel_constants, have_vertex_textures;
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
    FLOAT blend[4];
    u32 stencil;
    D3D12_VERTEX_BUFFER_VIEW vertices;
    D3D12_INDEX_BUFFER_VIEW indices;
    D3D_PRIMITIVE_TOPOLOGY topology;
    u32 vertex_pull, indexed, count;
    rsx_geometry_visibility geometry;
} rsx_native_draw_packet;

static void rsx_native_draw_record(const rsx_native_draw_packet* p,
    ID3D12GraphicsCommandList* list, rsx_native_bindings* bindings)
{
    D3D12_INDEX_BUFFER_VIEW generated_indices={0};
    if(p->geometry.triangles) {
        if(p->topology!=D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST)abort();
        generated_indices=rsx_geometry_visibility_begin(&p->geometry,list);
        /* Compute and graphics share PSO state. Force the graphics rebind. */
        bindings->valid&=~1u;
    }
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    for (u32 i = 0; i < p->sampled_target_count; ++i) {
        barrier.Transition.pResource = p->sampled_targets[i];
        list->lpVtbl->ResourceBarrier(list, 1, &barrier);
    }
    if (p->clear_depth)
        list->lpVtbl->ClearDepthStencilView(list, p->dsv,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
    rsx_nb_targets(bindings, list, p->target_count, p->rtvs,
        p->have_dsv ? &p->dsv : NULL);
    rsx_nb_heaps(bindings, list, p->heaps);
    rsx_nb_pso(bindings, list, p->pso);
    rsx_nb_root(bindings, list, p->root);
    list->lpVtbl->SetGraphicsRootConstantBufferView(list, 0, p->vertex_constants);
    if (p->have_pixel_constants)
        list->lpVtbl->SetGraphicsRootConstantBufferView(list, 4, p->pixel_constants);
    list->lpVtbl->SetGraphicsRootDescriptorTable(list, 1, p->textures);
    list->lpVtbl->SetGraphicsRootDescriptorTable(list, 2, p->samplers);
    if (p->have_vertex_textures)
        list->lpVtbl->SetGraphicsRootDescriptorTable(list, 3, p->vertex_textures);
    rsx_nb_viewport(bindings, list, &p->viewport);
    rsx_nb_scissor(bindings, list, &p->scissor);
    rsx_nb_stencil(bindings, list, p->stencil);
    rsx_nb_blend(bindings, list, p->blend);
    if (p->vertex_pull)
        list->lpVtbl->SetGraphicsRootShaderResourceView(list, 5, p->vertices.BufferLocation);
    else if (p->vertices.StrideInBytes)
        list->lpVtbl->IASetVertexBuffers(list, 0, 1, &p->vertices);
    rsx_nb_topology(bindings, list, p->topology);
    if (p->geometry.triangles) {
        list->lpVtbl->IASetIndexBuffer(list, &generated_indices);
        list->lpVtbl->DrawIndexedInstanced(list, p->geometry.triangles*3, 1, 0, 0, 0);
        rsx_geometry_visibility_end(&p->geometry,list);
    } else if (p->indexed) {
        list->lpVtbl->IASetIndexBuffer(list, &p->indices);
        list->lpVtbl->DrawIndexedInstanced(list, p->count, 1, 0, 0, 0);
    } else {
        list->lpVtbl->DrawInstanced(list, p->count, 1, 0, 0);
    }
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    for (u32 i = 0; i < p->sampled_target_count; ++i) {
        barrier.Transition.pResource = p->sampled_targets[i];
        list->lpVtbl->ResourceBarrier(list, 1, &barrier);
    }
}
#endif
