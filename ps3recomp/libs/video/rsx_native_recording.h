/* Ordered native command recording. One serialized preparation owner submits
 * copied values; one worker owns D3D command recording and binding state.
 * GPU-visible storage stays leased to the existing submission slots until
 * their real fences. Drain BEFORE Close/Reset, descriptor-source replacement,
 * diagnostics reading recording state, and resource destruction.
 *
 * This queue is not a GPU completion mechanism. Publishing or consuming an
 * entry cannot release guest labels or GPU allocations. All stack arguments
 * are copied; persistent resource pointers are valid through the drain/fence
 * boundaries enforced by the live backend. Windows implementation, opt-in.
 */
#ifndef RSX_NATIVE_RECORDING_H
#define RSX_NATIVE_RECORDING_H

enum rsx_rec_op {
    RR_DRAW, RR_BARRIER, RR_COPY_TEXTURE, RR_COPY_RESOURCE, RR_CLEAR_RTV,
    RR_CLEAR_DSV, RR_END_QUERY, RR_RESOLVE_QUERY, RR_ROOT_CONSTANTS,
    RR_ROOT_TABLE, RR_COMPUTE_ROOT, RR_COMPUTE_TABLE, RR_DISPATCH, RR_TRIANGLE,
    RR_TARGETS, RR_HEAPS, RR_PSO, RR_ROOT, RR_VIEWPORT, RR_SCISSOR, RR_TOPOLOGY
};
typedef struct rsx_rec_command {
    ld_frame_resources* frame;
    enum rsx_rec_op op;
    union {
        rsx_native_draw_packet draw;
        struct { u32 count; D3D12_RESOURCE_BARRIER items[4]; } barriers;
        struct { D3D12_TEXTURE_COPY_LOCATION dst, src; D3D12_BOX box;
                 u32 x, y, z, have_box; } copy_texture;
        struct { ID3D12Resource *dst, *src; } copy_resource;
        struct { D3D12_CPU_DESCRIPTOR_HANDLE handle; FLOAT color[4];
                 FLOAT depth; D3D12_CLEAR_FLAGS flags; UINT8 stencil; } clear;
        struct { ID3D12QueryHeap* heap; D3D12_QUERY_TYPE type; u32 index, count;
                 ID3D12Resource* dst; u64 offset; } query;
        struct { u32 index, count, offset, values[8]; } constants;
        struct { u32 index; D3D12_GPU_DESCRIPTOR_HANDLE handle; } table;
        ID3D12RootSignature* root;
        ID3D12PipelineState* pso;
        struct { u32 x, y, z; } dispatch;
        struct { u32 count, instances, first, start_instance; } triangle;
        struct { u32 count, have_depth; D3D12_CPU_DESCRIPTOR_HANDLE colors[4], depth; } targets;
        ID3D12DescriptorHeap* heaps[2];
        D3D12_VIEWPORT viewport;
        D3D12_RECT scissor;
        D3D_PRIMITIVE_TOPOLOGY topology;
    } data;
} rsx_rec_command;

static void rsx_rec_execute(const rsx_rec_command* c)
{
    ID3D12GraphicsCommandList* l = c->frame->list;
    rsx_native_bindings* b = &c->frame->bindings;
    switch (c->op) {
    case RR_DRAW: rsx_native_draw_record(&c->data.draw, l, b); break;
    case RR_BARRIER: l->lpVtbl->ResourceBarrier(l, c->data.barriers.count, c->data.barriers.items); break;
    case RR_COPY_TEXTURE: l->lpVtbl->CopyTextureRegion(l, &c->data.copy_texture.dst,
        c->data.copy_texture.x, c->data.copy_texture.y, c->data.copy_texture.z,
        &c->data.copy_texture.src, c->data.copy_texture.have_box ? &c->data.copy_texture.box : NULL); break;
    case RR_COPY_RESOURCE: l->lpVtbl->CopyResource(l, c->data.copy_resource.dst, c->data.copy_resource.src); break;
    case RR_CLEAR_RTV: l->lpVtbl->ClearRenderTargetView(l, c->data.clear.handle, c->data.clear.color, 0, NULL); break;
    case RR_CLEAR_DSV: l->lpVtbl->ClearDepthStencilView(l, c->data.clear.handle,
        c->data.clear.flags, c->data.clear.depth, c->data.clear.stencil, 0, NULL); break;
    case RR_END_QUERY: l->lpVtbl->EndQuery(l, c->data.query.heap, c->data.query.type, c->data.query.index); break;
    case RR_RESOLVE_QUERY: l->lpVtbl->ResolveQueryData(l, c->data.query.heap, c->data.query.type,
        c->data.query.index, c->data.query.count, c->data.query.dst, c->data.query.offset); break;
    case RR_ROOT_CONSTANTS: l->lpVtbl->SetGraphicsRoot32BitConstants(l, c->data.constants.index,
        c->data.constants.count, c->data.constants.values, c->data.constants.offset); break;
    case RR_ROOT_TABLE: l->lpVtbl->SetGraphicsRootDescriptorTable(l, c->data.table.index, c->data.table.handle); break;
    case RR_COMPUTE_ROOT: l->lpVtbl->SetComputeRootSignature(l, c->data.root); break;
    case RR_COMPUTE_TABLE: l->lpVtbl->SetComputeRootDescriptorTable(l, c->data.table.index, c->data.table.handle); break;
    case RR_DISPATCH: l->lpVtbl->Dispatch(l, c->data.dispatch.x, c->data.dispatch.y, c->data.dispatch.z); break;
    case RR_TRIANGLE: l->lpVtbl->DrawInstanced(l, c->data.triangle.count, c->data.triangle.instances,
        c->data.triangle.first, c->data.triangle.start_instance); break;
    case RR_TARGETS: rsx_nb_targets(b, l, c->data.targets.count, c->data.targets.colors,
        c->data.targets.have_depth ? &c->data.targets.depth : NULL); break;
    case RR_HEAPS: rsx_nb_heaps(b, l, c->data.heaps); break;
    case RR_PSO: rsx_nb_pso(b, l, c->data.pso); break;
    case RR_ROOT: rsx_nb_root(b, l, c->data.root); break;
    case RR_VIEWPORT: rsx_nb_viewport(b, l, &c->data.viewport); break;
    case RR_SCISSOR: rsx_nb_scissor(b, l, &c->data.scissor); break;
    case RR_TOPOLOGY: rsx_nb_topology(b, l, c->data.topology); break;
    default: abort();
    }
}

typedef struct rsx_rec_queue {
    rsx_rec_command* entries;
    u32 capacity;
    u64 head, last_published, publications, drains, full_waits, draws;
    volatile LONG64 published, consumed;
    volatile LONG stop;
    HANDLE worker, ready, progress;
    u64 wait_ticks, busy_ticks;
} rsx_rec_queue;
static rsx_rec_queue g_rs_rec;

static u64 rsx_rec_load(volatile LONG64* p)
{ return (u64)InterlockedCompareExchange64(p, 0, 0); }
static void rsx_rec_signal(HANDLE event)
{ if (!SetEvent(event)) { fprintf(stderr,"[native-record] event failure\n"); abort(); } }
static DWORD WINAPI rsx_rec_worker(void* unused)
{
    (void)unused;
    u64 at = 0;
    for (;;) {
        u64 end = rsx_rec_load(&g_rs_rec.published);
        if (at == end) {
            if (InterlockedCompareExchange(&g_rs_rec.stop,0,0)) return 0;
            if (WaitForSingleObject(g_rs_rec.ready,INFINITE) != WAIT_OBJECT_0) abort();
            continue;
        }
        /* Amortize publication and timing; never retain a pointer into a slot
         * after advancing consumed, which permits its producer to overwrite it. */
        if (end - at > 64) end = at + 64;
        LARGE_INTEGER start, finish; QueryPerformanceCounter(&start);
        for (; at < end; ++at)
            rsx_rec_execute(&g_rs_rec.entries[at % g_rs_rec.capacity]);
        QueryPerformanceCounter(&finish);
        g_rs_rec.busy_ticks += (u64)(finish.QuadPart - start.QuadPart);
        InterlockedExchange64(&g_rs_rec.consumed,(LONG64)at);
        rsx_rec_signal(g_rs_rec.progress);
    }
}
static void rsx_rec_publish(void)
{
    if (g_rs_rec.head == g_rs_rec.last_published) return;
    InterlockedExchange64(&g_rs_rec.published,(LONG64)g_rs_rec.head);
    g_rs_rec.last_published = g_rs_rec.head;
    ++g_rs_rec.publications;
    /* One retained auto-reset event per batch: no check/sleep lost-wake race. */
    rsx_rec_signal(g_rs_rec.ready);
}
static void rsx_rec_wait_until(u64 sequence)
{
    LARGE_INTEGER start, finish; QueryPerformanceCounter(&start);
    while (rsx_rec_load(&g_rs_rec.consumed) < sequence) {
        HANDLE handles[2] = {g_rs_rec.progress,g_rs_rec.worker};
        if (WaitForMultipleObjects(2,handles,FALSE,INFINITE) != WAIT_OBJECT_0) {
            fprintf(stderr,"[native-record] recording worker failed\n"); abort();
        }
    }
    QueryPerformanceCounter(&finish);
    g_rs_rec.wait_ticks += (u64)(finish.QuadPart - start.QuadPart);
}
static void rsx_rec_drain(void)
{
    if (!g_rs_rec.worker) return;
    rsx_rec_publish();
    if (rsx_rec_load(&g_rs_rec.consumed) != g_rs_rec.head) {
        ++g_rs_rec.drains;
        rsx_rec_wait_until(g_rs_rec.head);
    }
}
static void rsx_rec_submit(const rsx_rec_command* command)
{
    if (!g_rs_rec.worker) { rsx_rec_execute(command); return; }
    if (g_rs_rec.head - rsx_rec_load(&g_rs_rec.consumed) >= g_rs_rec.capacity) {
        ++g_rs_rec.full_waits;
        rsx_rec_publish();
        rsx_rec_wait_until(g_rs_rec.head - g_rs_rec.capacity + 1);
    }
    g_rs_rec.entries[g_rs_rec.head % g_rs_rec.capacity] = *command;
    ++g_rs_rec.head;
    if (command->op == RR_DRAW) ++g_rs_rec.draws;
    if (g_rs_rec.head - g_rs_rec.last_published >= 16) rsx_rec_publish();
}
static void rsx_rec_initialize(void)
{
    const char* enable = getenv("RSX_NATIVE_RECORDING");
    if (!enable || strcmp(enable,"1")) {
        fprintf(stderr,"[native-record] enabled=0\n"); return;
    }
    if (g_rs_rec.worker) abort();
    memset(&g_rs_rec,0,sizeof(g_rs_rec));
    g_rs_rec.capacity = 4096;
    const char* limit = getenv("RSX_RECORD_QUEUE_CAPACITY");
    if (limit) {
        char* end = NULL; unsigned long n = strtoul(limit,&end,10);
        if (!*limit || *end || n < 32 || n > 4096) abort();
        g_rs_rec.capacity = (u32)n;
    }
    g_rs_rec.entries = calloc(g_rs_rec.capacity,sizeof(*g_rs_rec.entries));
    g_rs_rec.ready = CreateEventW(NULL,FALSE,FALSE,NULL);
    g_rs_rec.progress = CreateEventW(NULL,FALSE,FALSE,NULL);
    if (!g_rs_rec.entries || !g_rs_rec.ready || !g_rs_rec.progress) abort();
    (void)rsx_nb_enabled(); /* initialize shared configuration before worker */
    g_rs_rec.worker = CreateThread(NULL,0,rsx_rec_worker,NULL,0,NULL);
    if (!g_rs_rec.worker) abort();
    fprintf(stderr,"[native-record] enabled=1 capacity=%u packet_bytes=%zu batch=16\n",
        g_rs_rec.capacity,sizeof(rsx_rec_command));
}
static void rsx_rec_report(void)
{
    if (!g_rs_rec.worker) return;
    rsx_rec_drain();
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    fprintf(stderr,"[native-record-summary] commands=%llu consumed=%llu draws=%llu "
        "publications=%llu drains=%llu full_waits=%llu wait_ms=%.3f worker_ms=%.3f\n",
        g_rs_rec.head,rsx_rec_load(&g_rs_rec.consumed),g_rs_rec.draws,
        g_rs_rec.publications,g_rs_rec.drains,g_rs_rec.full_waits,
        1000.0*g_rs_rec.wait_ticks/frequency.QuadPart,1000.0*g_rs_rec.busy_ticks/frequency.QuadPart);
}
static void rsx_rec_shutdown(void)
{
    if (!g_rs_rec.worker) return;
    rsx_rec_drain();
    InterlockedExchange(&g_rs_rec.stop,1);
    rsx_rec_signal(g_rs_rec.ready);
    if (WaitForSingleObject(g_rs_rec.worker,INFINITE) != WAIT_OBJECT_0) abort();
    rsx_rec_report();
    CloseHandle(g_rs_rec.worker); CloseHandle(g_rs_rec.ready); CloseHandle(g_rs_rec.progress);
    free(g_rs_rec.entries); memset(&g_rs_rec,0,sizeof(g_rs_rec));
}

#define RR_BEGIN(kind) rsx_rec_command c = {0}; c.frame=f; c.op=kind
#define RR_SEND() rsx_rec_submit(&c)
static void rsx_rec_draw(const rsx_native_draw_packet* p,ld_frame_resources* f)
{
    if (!g_rs_rec.worker) { rsx_native_draw_record(p,f->list,&f->bindings); return; }
    RR_BEGIN(RR_DRAW); c.data.draw=*p; RR_SEND();
}
static void rsx_rec_ResourceBarrier(ld_frame_resources* f,UINT count,const D3D12_RESOURCE_BARRIER* barriers)
{
    if (count>4) abort(); RR_BEGIN(RR_BARRIER);
    c.data.barriers.count=count; memcpy(c.data.barriers.items,barriers,count*sizeof(*barriers)); RR_SEND();
}
static void rsx_rec_CopyTextureRegion(ld_frame_resources* f,const D3D12_TEXTURE_COPY_LOCATION* dst,
    UINT x,UINT y,UINT z,const D3D12_TEXTURE_COPY_LOCATION* src,const D3D12_BOX* box)
{
    RR_BEGIN(RR_COPY_TEXTURE); c.data.copy_texture.dst=*dst; c.data.copy_texture.src=*src;
    c.data.copy_texture.x=x;c.data.copy_texture.y=y;c.data.copy_texture.z=z;
    c.data.copy_texture.have_box=box!=NULL; if(box)c.data.copy_texture.box=*box; RR_SEND();
}
static void rsx_rec_CopyResource(ld_frame_resources* f,ID3D12Resource* dst,ID3D12Resource* src)
{ RR_BEGIN(RR_COPY_RESOURCE);c.data.copy_resource.dst=dst;c.data.copy_resource.src=src;RR_SEND(); }
static void rsx_rec_ClearRenderTargetView(ld_frame_resources* f,D3D12_CPU_DESCRIPTOR_HANDLE handle,
    const FLOAT* color,UINT rect_count,const D3D12_RECT* rects)
{
    if(rect_count || rects)abort(); RR_BEGIN(RR_CLEAR_RTV);c.data.clear.handle=handle;
    memcpy(c.data.clear.color,color,sizeof(c.data.clear.color));RR_SEND();
}
static void rsx_rec_ClearDepthStencilView(ld_frame_resources* f,D3D12_CPU_DESCRIPTOR_HANDLE handle,
    D3D12_CLEAR_FLAGS flags,FLOAT depth,UINT8 stencil,UINT rect_count,const D3D12_RECT* rects)
{
    if(rect_count || rects)abort();RR_BEGIN(RR_CLEAR_DSV);c.data.clear.handle=handle;
    c.data.clear.flags=flags;c.data.clear.depth=depth;c.data.clear.stencil=stencil;RR_SEND();
}
static void rsx_rec_EndQuery(ld_frame_resources* f,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE type,UINT index)
{ RR_BEGIN(RR_END_QUERY);c.data.query.heap=heap;c.data.query.type=type;c.data.query.index=index;RR_SEND(); }
static void rsx_rec_ResolveQueryData(ld_frame_resources* f,ID3D12QueryHeap* heap,D3D12_QUERY_TYPE type,
    UINT index,UINT count,ID3D12Resource* dst,UINT64 offset)
{
    RR_BEGIN(RR_RESOLVE_QUERY);c.data.query.heap=heap;c.data.query.type=type;
    c.data.query.index=index;c.data.query.count=count;c.data.query.dst=dst;c.data.query.offset=offset;RR_SEND();
}
static void rsx_rec_SetGraphicsRoot32BitConstants(ld_frame_resources* f,UINT index,UINT count,const void* values,UINT offset)
{
    if(count>8)abort();RR_BEGIN(RR_ROOT_CONSTANTS);c.data.constants.index=index;
    c.data.constants.count=count;c.data.constants.offset=offset;
    memcpy(c.data.constants.values,values,count*4u);RR_SEND();
}
static void rsx_rec_SetGraphicsRootDescriptorTable(ld_frame_resources* f,UINT index,D3D12_GPU_DESCRIPTOR_HANDLE handle)
{ RR_BEGIN(RR_ROOT_TABLE);c.data.table.index=index;c.data.table.handle=handle;RR_SEND(); }
static void rsx_rec_SetComputeRootSignature(ld_frame_resources* f,ID3D12RootSignature* root)
{ RR_BEGIN(RR_COMPUTE_ROOT);c.data.root=root;RR_SEND(); }
static void rsx_rec_SetComputeRootDescriptorTable(ld_frame_resources* f,UINT index,D3D12_GPU_DESCRIPTOR_HANDLE handle)
{ RR_BEGIN(RR_COMPUTE_TABLE);c.data.table.index=index;c.data.table.handle=handle;RR_SEND(); }
static void rsx_rec_Dispatch(ld_frame_resources* f,UINT x,UINT y,UINT z)
{ RR_BEGIN(RR_DISPATCH);c.data.dispatch.x=x;c.data.dispatch.y=y;c.data.dispatch.z=z;RR_SEND(); }
static void rsx_rec_DrawInstanced(ld_frame_resources* f,UINT count,UINT instances,UINT first,UINT start_instance)
{
    RR_BEGIN(RR_TRIANGLE);c.data.triangle.count=count;c.data.triangle.instances=instances;
    c.data.triangle.first=first;c.data.triangle.start_instance=start_instance;RR_SEND();
}
static void rsx_rec_targets(ld_frame_resources* f,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* colors,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
{
    if(count>4)abort();RR_BEGIN(RR_TARGETS);c.data.targets.count=count;c.data.targets.have_depth=depth!=NULL;
    if(depth)c.data.targets.depth=*depth;memcpy(c.data.targets.colors,colors,count*sizeof(*colors));RR_SEND();
}
static void rsx_rec_heaps(ld_frame_resources* f,ID3D12DescriptorHeap* const* heaps)
{ RR_BEGIN(RR_HEAPS);memcpy(c.data.heaps,heaps,sizeof(c.data.heaps));RR_SEND(); }
static void rsx_rec_pso(ld_frame_resources* f,ID3D12PipelineState* pso)
{ RR_BEGIN(RR_PSO);c.data.pso=pso;RR_SEND(); }
static void rsx_rec_root(ld_frame_resources* f,ID3D12RootSignature* root)
{ RR_BEGIN(RR_ROOT);c.data.root=root;RR_SEND(); }
static void rsx_rec_viewport(ld_frame_resources* f,const D3D12_VIEWPORT* viewport)
{ RR_BEGIN(RR_VIEWPORT);c.data.viewport=*viewport;RR_SEND(); }
static void rsx_rec_scissor(ld_frame_resources* f,const D3D12_RECT* scissor)
{ RR_BEGIN(RR_SCISSOR);c.data.scissor=*scissor;RR_SEND(); }
static void rsx_rec_topology(ld_frame_resources* f,D3D_PRIMITIVE_TOPOLOGY topology)
{ RR_BEGIN(RR_TOPOLOGY);c.data.topology=topology;RR_SEND(); }
#undef RR_BEGIN
#undef RR_SEND
#endif
