/* Transient D3D12 resources owned by one submission slot. The owner must wait
 * for submitted_fence before resetting the allocator, overwriting mapped
 * storage/descriptors, or releasing retired resources. Persistent surfaces,
 * shader caches and CPU descriptor sources stay in the device state. */
#ifndef RSX_LIVE_FRAME_RESOURCES_H
#define RSX_LIVE_FRAME_RESOURCES_H
#include "rsx_native_bindings.h"
typedef struct ld_frame_resources {
    rsx_native_bindings bindings;
    ID3D12CommandAllocator* alloc;
    ID3D12GraphicsCommandList* list;
    ID3D12DescriptorHeap *srv_heap, *smp_heap;
    ID3D12Resource *upload, *vb, *ib, *cb, *ps_cb;
    u8 *upload_mapped, *vb_mapped, *cb_mapped, *ps_cb_mapped;
    u32* ib_mapped;
    u32 upload_used, vb_used, ib_used, cb_used, ps_cb_used;
    u32 srv_ring_used, smp_ring_used;
    u32 smp_ring_slots[SMP_RING_TABLES][SMP_TABLE_SIZE];
    ID3D12Resource* retired_textures[MAX_RETIRED_TEXTURES];
    u32 n_retired_textures;
    u64 submitted_fence;
    ID3D12Resource *geometry_input,*geometry_output;
    u8* geometry_mapped;
    u32 geometry_input_used,geometry_output_used;
#if defined(YZ_PERF_PROFILE)
    ID3D12QueryHeap* timing_heap;
    ID3D12Resource* timing_readback;
#endif
} ld_frame_resources;

static void ld_frame_resources_destroy(ld_frame_resources* f)
{
    /* Caller guarantees that every submitted command referencing this slot
     * has completed, including on shutdown and device reinitialization. */
    for (u32 i=0; i<f->n_retired_textures; ++i)
        f->retired_textures[i]->lpVtbl->Release(f->retired_textures[i]);
#define LD_FRAME_RELEASE(field) if(f->field)f->field->lpVtbl->Release(f->field)
    LD_FRAME_RELEASE(list); LD_FRAME_RELEASE(alloc);
    LD_FRAME_RELEASE(srv_heap); LD_FRAME_RELEASE(smp_heap);
    LD_FRAME_RELEASE(upload); LD_FRAME_RELEASE(vb); LD_FRAME_RELEASE(ib);
    LD_FRAME_RELEASE(cb); LD_FRAME_RELEASE(ps_cb);
    LD_FRAME_RELEASE(geometry_input);LD_FRAME_RELEASE(geometry_output);
#if defined(YZ_PERF_PROFILE)
    LD_FRAME_RELEASE(timing_heap); LD_FRAME_RELEASE(timing_readback);
#endif
#undef LD_FRAME_RELEASE
    memset(f,0,sizeof(*f));
}

static int ld_frame_buffer(ID3D12Device* dev, u64 bytes,
                          ID3D12Resource** resource, void** mapped)
{
    D3D12_HEAP_PROPERTIES heap={0}; heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc={0};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes;
    desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1;
    desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(dev->lpVtbl->CreateCommittedResource(dev,&heap,D3D12_HEAP_FLAG_NONE,
        &desc,D3D12_RESOURCE_STATE_GENERIC_READ,NULL,&IID_ID3D12Resource,(void**)resource)))return 0;
    const D3D12_RANGE no_read={0,0};
    return SUCCEEDED((*resource)->lpVtbl->Map(*resource,0,&no_read,mapped));
}

static int ld_frame_resources_create(ID3D12Device* dev, ld_frame_resources* f)
{
    memset(f,0,sizeof(*f));
    if(FAILED(dev->lpVtbl->CreateCommandAllocator(dev,D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator,(void**)&f->alloc)))goto fail;
    if(FAILED(dev->lpVtbl->CreateCommandList(dev,0,D3D12_COMMAND_LIST_TYPE_DIRECT,
        f->alloc,NULL,&IID_ID3D12GraphicsCommandList,(void**)&f->list)))goto fail;
    if(!ld_frame_buffer(dev,UPLOAD_SIZE,&f->upload,(void**)&f->upload_mapped) ||
       !ld_frame_buffer(dev,VERT_BUFFER_SIZE,&f->vb,(void**)&f->vb_mapped) ||
       !ld_frame_buffer(dev,INDEX_BUFFER_SIZE,&f->ib,(void**)&f->ib_mapped) ||
       !ld_frame_buffer(dev,CB_RING_BYTES,&f->cb,(void**)&f->cb_mapped) ||
       !ld_frame_buffer(dev,PS_CB_RING_BYTES,&f->ps_cb,(void**)&f->ps_cb_mapped))goto fail;
    D3D12_DESCRIPTOR_HEAP_DESC heap={0};
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors=SRV_RING_TABLES*SRV_TABLE_SIZE;
    heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(dev->lpVtbl->CreateDescriptorHeap(dev,&heap,&IID_ID3D12DescriptorHeap,(void**)&f->srv_heap)))goto fail;
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heap.NumDescriptors=SMP_RING_TABLES*SMP_TABLE_SIZE;
    if(FAILED(dev->lpVtbl->CreateDescriptorHeap(dev,&heap,&IID_ID3D12DescriptorHeap,(void**)&f->smp_heap)))goto fail;
#if defined(YZ_PERF_PROFILE)
    D3D12_QUERY_HEAP_DESC query={0};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=2;
    D3D12_HEAP_PROPERTIES readback={0};readback.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer={0};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width=16;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;
    buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    dev->lpVtbl->CreateQueryHeap(dev,&query,&IID_ID3D12QueryHeap,(void**)&f->timing_heap);
    dev->lpVtbl->CreateCommittedResource(dev,&readback,D3D12_HEAP_FLAG_NONE,&buffer,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&f->timing_readback);
#endif
    return 1;
fail:
    fprintf(stderr,"[live-draw] submission resource allocation failed\n");
    ld_frame_resources_destroy(f);return 0;
}
#endif
