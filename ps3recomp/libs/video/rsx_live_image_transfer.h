/* Included by rsx_live_draw.c after ld_scaled_blit_pso. All rectangles here
 * are physical GPU pixels; UVs are normalized. Callers explicitly supply
 * resource ownership states, which are restored before returning. */
static int ld_transfer_image(ID3D12Resource* source, u32 source_srv,
    D3D12_RESOURCE_STATES source_state, ID3D12Resource* destination,
    D3D12_CPU_DESCRIPTOR_HANDLE destination_rtv,
    D3D12_RESOURCE_STATES destination_state, DXGI_FORMAT format,
    const float uv[4], u32 x, u32 y, u32 width, u32 height)
{
    if (!source || !destination || source == destination || !width || !height)
        return 0;
    ID3D12PipelineState* pso = ld_scaled_blit_pso(format);
    if (!pso) return 0;
    u32 slots[SRV_TABLE_SIZE];
    for (u32 i = 0; i < SRV_TABLE_SIZE; ++i) slots[i] = SRV_WHITE;
    slots[0] = source_srv;
    D3D12_GPU_DESCRIPTOR_HANDLE table = srv_table(slots);
    if (!table.ptr) return 0;

    D3D12_RESOURCE_BARRIER barriers[2] = {0};
    u32 count = 0;
    if (source_state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER* b = &barriers[count++];
        b->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b->Transition.pResource = source;
        b->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b->Transition.StateBefore = source_state;
        b->Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if (destination_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        D3D12_RESOURCE_BARRIER* b = &barriers[count++];
        b->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b->Transition.pResource = destination;
        b->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b->Transition.StateBefore = destination_state;
        b->Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (count) rsx_rec_ResourceBarrier(g.frame, count, barriers);
    rsx_rec_targets(g.frame, 1, &destination_rtv, NULL);
    ID3D12DescriptorHeap* heaps[] = {g.frame->srv_heap, g.frame->smp_heap};
    rsx_rec_heaps(g.frame, heaps);
    rsx_rec_pso(g.frame, pso);
    rsx_rec_root(g.frame, g_sblit_rs);
    rsx_rec_SetGraphicsRoot32BitConstants(g.frame, 0, 4, uv, 0);
    rsx_rec_SetGraphicsRootDescriptorTable(g.frame, 1, table);
    D3D12_VIEWPORT vp = {(float)x, (float)y, (float)width, (float)height, 0, 1};
    D3D12_RECT sc = {(LONG)x, (LONG)y, (LONG)(x + width), (LONG)(y + height)};
    rsx_rec_viewport(g.frame, &vp);
    rsx_rec_scissor(g.frame, &sc);
    rsx_rec_topology(g.frame, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    rsx_rec_DrawInstanced(g.frame, 3, 1, 0, 0);
    for (u32 i = 0; i < count; ++i) {
        D3D12_RESOURCE_STATES before = barriers[i].Transition.StateBefore;
        barriers[i].Transition.StateBefore = barriers[i].Transition.StateAfter;
        barriers[i].Transition.StateAfter = before;
    }
    if (count) rsx_rec_ResourceBarrier(g.frame, count, barriers);
    return 1;
}

/* Host images have no guest address and never enter the guest alias table.
 * 0: decoded movie upload. 1: logical-resolution CPU overlay readback. */
static ID3D12Resource* ld_host_image(u32 slot, u32 width, u32 height)
{
    if (slot >= 2 || !width || !height) return NULL;
    if (g.host_image[slot].tex && g.host_image[slot].width == width &&
        g.host_image[slot].height == height) return g.host_image[slot].tex;
    D3D12_HEAP_PROPERTIES heap = {0}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {0};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* image = NULL;
    HRESULT hr = g.dev->lpVtbl->CreateCommittedResource(g.dev, &heap,
        D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
        NULL, &IID_ID3D12Resource, (void**)&image);
    if (FAILED(hr)) {
        fprintf(stderr, "[image-transfer] allocation failed %ux%u hr=0x%08lX\n",
            width, height, (unsigned long)hr);
        return NULL;
    }
    if (g.host_image[slot].tex) retire_texture(g.host_image[slot].tex);
    g.host_image[slot].tex = image;
    g.host_image[slot].width = width; g.host_image[slot].height = height;
    srv_write(SRV_HOST_IMAGE_BASE + slot, image);
    rsx_rec_drain();
    if (g.frame) g.frame->bindings.valid &= ~8u;
    g.dev->lpVtbl->CreateRenderTargetView(g.dev, image, NULL,
        rtv_handle(RTV_HOST_IMAGE_BASE + slot));
    return image;
}
