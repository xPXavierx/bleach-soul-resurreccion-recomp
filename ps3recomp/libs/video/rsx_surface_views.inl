/* GPU image views for rectangular aliases of render surfaces.
 *
 * A view has independent identity (source resource, logical rectangle),
 * storage and content generation. Switching between glyph sizes must not
 * destroy and reallocate images. GPU copies update only stale content.
 *
 * CPU descriptors are pinned through the current draw's descriptor-table
 * copy. Earlier draws own immutable shader-visible descriptor copies; evicted
 * resources remain in retire_texture until the submission fence completes.
 * No extra GPU waits or synthetic completion acknowledgements are added.
 */
#define LD_VIEW_HASH_CAP (LD_VIEW_CACHE_CAP * 4)
typedef struct {
    ID3D12Resource *tex, *source;
    u32 w,h,generation;
    u64 allocation_bytes,last_use,pinned_draw;
} surface_view_t;
static struct {
    surface_view_t entries[LD_VIEW_CACHE_CAP];
    u32 hash[LD_VIEW_HASH_CAP]; /* entry index+1; 0 is empty */
    u64 use_serial,draw_serial;
    rsx_surface_view_stats stats;
} g_surface_views;

static int surface_views_enabled(void)
{
    static int on=-1;
    if(on<0) {
        const char* value=getenv("RSX_SURFACE_VIEWS");
        on=!(value && !strcmp(value,"legacy"));
        const char* size=getenv("RSX_SURFACE_VIEW_MB");
        unsigned long mb=size?strtoul(size,NULL,10):128;
        if(mb<1)mb=1; if(mb>512)mb=512;
        g_surface_views.stats.budget_bytes=(u64)mb*1024*1024;
        fprintf(stderr,"[surface-views] mode=%s resident_budget_mb=%lu capacity=%u\n",
            on?"pooled":"legacy",mb,LD_VIEW_CACHE_CAP);
    }
    return on;
}
static u32 surface_view_hash(ID3D12Resource* source,u32 w,u32 h)
{
    u64 x=(u64)(uintptr_t)source ^ ((u64)w<<32) ^ h;
    x^=x>>33; x*=0xff51afd7ed558ccdULL; x^=x>>33;
    return (u32)x & (LD_VIEW_HASH_CAP-1);
}
static void surface_view_insert(u32 index)
{
    const surface_view_t* e=&g_surface_views.entries[index];
    u32 p=surface_view_hash(e->source,e->w,e->h);
    while(g_surface_views.hash[p])p=(p+1)&(LD_VIEW_HASH_CAP-1);
    g_surface_views.hash[p]=index+1;
}
static void surface_view_erase(u32 index)
{
    surface_view_t* e=&g_surface_views.entries[index];
    u32 p=surface_view_hash(e->source,e->w,e->h);
    while(g_surface_views.hash[p]!=index+1)p=(p+1)&(LD_VIEW_HASH_CAP-1);
    g_surface_views.hash[p]=0;
    /* Reinsert the following cluster: no tombstones accumulate under churn. */
    for(p=(p+1)&(LD_VIEW_HASH_CAP-1);g_surface_views.hash[p];p=(p+1)&(LD_VIEW_HASH_CAP-1)) {
        u32 displaced=g_surface_views.hash[p]-1;
        g_surface_views.hash[p]=0; surface_view_insert(displaced);
    }
    retire_texture(e->tex);
    g_surface_views.stats.resident_bytes-=e->allocation_bytes;
    --g_surface_views.stats.resident_entries;
    ++g_surface_views.stats.evictions;
    memset(e,0,sizeof(*e));
}
static void surface_views_invalidate(ID3D12Resource* source)
{
    for(u32 i=0;i<LD_VIEW_CACHE_CAP;i++)
        if(g_surface_views.entries[i].tex && g_surface_views.entries[i].source==source)
            surface_view_erase(i);
}
static u32 surface_view_oldest(void)
{
    u32 oldest=LD_VIEW_CACHE_CAP;
    for(u32 i=0;i<LD_VIEW_CACHE_CAP;i++) {
        const surface_view_t* e=&g_surface_views.entries[i];
        if(!e->tex || e->pinned_draw==g_surface_views.draw_serial)continue;
        if(oldest==LD_VIEW_CACHE_CAP || e->last_use<g_surface_views.entries[oldest].last_use)oldest=i;
    }
    return oldest;
}
static int surface_view(u32 si,const rsx_dsp_texture* t)
{
    surface_t* source=&g.surfaces[si];
    const u32 w=t->width,h=t->height;
    if(!ld_swin_on() || !source->tex || !w || !h ||
       (w==source->w && h==source->h) || w>source->w || h>source->h)return -1;
    const u32 bytes=source->fmt==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
    if(t->dimension!=2 || t->cubemap || !(t->format&TEX_FMT_LINEAR) ||
       !source->pitch || t->pitch!=source->pitch || ld_tex_texel_bytes(t->format)!=bytes)return -1;
    /* A glyph scratch above the display scale (ld_logic_target_scale) is seen at
     * the display scale, box-reduced by ld_box_view_refresh instead of copied. */
    const float reduce=source->scale>g.resolution.scale?source->scale/g.resolution.scale:0.0f;
    u32 index=LD_VIEW_CACHE_CAP,p=surface_view_hash(source->tex,w,h);
    while(g_surface_views.hash[p]) {
        u32 candidate=g_surface_views.hash[p]-1;
        const surface_view_t* e=&g_surface_views.entries[candidate];
        if(e->source==source->tex && e->w==w && e->h==h){index=candidate;break;}
        p=(p+1)&(LD_VIEW_HASH_CAP-1);
    }
    if(index==LD_VIEW_CACHE_CAP) {
        ++g_surface_views.stats.misses;
        D3D12_RESOURCE_DESC rd;
        source->tex->lpVtbl->GetDesc(source->tex,&rd);
        rd.Width=ld_surface_physical(source,w);rd.Height=ld_surface_physical(source,h);rd.Alignment=0;
        rd.Flags=D3D12_RESOURCE_FLAG_NONE;
        if(reduce) {
            rd.Width=rsx_resolution_extent(g.resolution,w);rd.Height=rsx_resolution_extent(g.resolution,h);
            rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
        D3D12_RESOURCE_ALLOCATION_INFO info;
        g.dev->lpVtbl->GetResourceAllocationInfo(g.dev,&info,0,1,&rd);
        if(!info.SizeInBytes || info.SizeInBytes==UINT64_MAX)return -1;
        const u64 budget=g_surface_views.stats.budget_bytes;
        while(g_surface_views.stats.resident_bytes+info.SizeInBytes>budget) {
            u32 oldest=surface_view_oldest();
            if(oldest==LD_VIEW_CACHE_CAP)break; /* current draw/oversized image */
            surface_view_erase(oldest);
        }
        for(u32 i=0;i<LD_VIEW_CACHE_CAP;i++)if(!g_surface_views.entries[i].tex){index=i;break;}
        if(index==LD_VIEW_CACHE_CAP) {
            index=surface_view_oldest();
            if(index==LD_VIEW_CACHE_CAP)return -1;
            surface_view_erase(index);
        }
        D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource* texture=NULL;
        HRESULT hr=g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,NULL,&IID_ID3D12Resource,(void**)&texture);
        if(FAILED(hr)) {
            fprintf(stderr,"[surface-views] allocation failed %ux%u hr=0x%08lX\n",w,h,(unsigned long)hr);
            return -1;
        }
        surface_view_t* e=&g_surface_views.entries[index];
        e->tex=texture;e->source=source->tex;e->w=w;e->h=h;
        e->allocation_bytes=info.SizeInBytes;e->generation=source->swin_wgen-1;
        surface_view_insert(index);
        srv_write(SRV_VIEW_CACHE_BASE+index,texture);
        if(reduce) {
            D3D12_RENDER_TARGET_VIEW_DESC rv={0};
            rv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;rv.Format=source->fmt;
            rsx_rec_drain();   /* the slot may still be named by queued commands of an evicted view */
            g.dev->lpVtbl->CreateRenderTargetView(g.dev,texture,&rv,rtv_handle(RTV_VIEW_CACHE_BASE+index));
        }
        ++g_surface_views.stats.allocations;++g_surface_views.stats.resident_entries;
        g_surface_views.stats.resident_bytes+=info.SizeInBytes;
        if(g_surface_views.stats.resident_bytes>g_surface_views.stats.peak_resident_bytes)
            g_surface_views.stats.peak_resident_bytes=g_surface_views.stats.resident_bytes;
    } else ++g_surface_views.stats.hits;
    surface_view_t* e=&g_surface_views.entries[index];
    e->last_use=++g_surface_views.use_serial;e->pinned_draw=g_surface_views.draw_serial;
    if(e->generation!=source->swin_wgen && reduce) {
        if(!ld_box_view_refresh(si,e->tex,rtv_handle(RTV_VIEW_CACHE_BASE+index),
                                rsx_resolution_extent(g.resolution,w),rsx_resolution_extent(g.resolution,h),reduce))
            return -1;
        e->generation=source->swin_wgen;++g_surface_views.stats.copies;
    }
    if(e->generation!=source->swin_wgen) {
        D3D12_RESOURCE_BARRIER barriers[2]={0};
        for(unsigned i=0;i<2;i++) {
            barriers[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        barriers[0].Transition.pResource=source->tex;
        barriers[0].Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.pResource=e->tex;
        barriers[1].Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
        rsx_rec_ResourceBarrier(g.frame,2,barriers);
        D3D12_TEXTURE_COPY_LOCATION dst={0},src={0};
        dst.pResource=e->tex;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource=source->tex;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box={0,0,0,ld_surface_physical(source,w),ld_surface_physical(source,h),1};
        rsx_rec_CopyTextureRegion(g.frame,&dst,0,0,0,&src,&box);
        for(unsigned i=0;i<2;i++) {
            D3D12_RESOURCE_STATES before=barriers[i].Transition.StateBefore;
            barriers[i].Transition.StateBefore=barriers[i].Transition.StateAfter;
            barriers[i].Transition.StateAfter=before;
        }
        rsx_rec_ResourceBarrier(g.frame,2,barriers);
        e->generation=source->swin_wgen;++g_surface_views.stats.copies;
    }
    return (int)(SRV_VIEW_CACHE_BASE+index);
}
void rsx_live_draw_surface_view_stats(rsx_surface_view_stats* stats)
{ if(stats)*stats=g_surface_views.stats; }
static void surface_views_shutdown(void)
{
    for(u32 i=0;i<LD_VIEW_CACHE_CAP;i++)if(g_surface_views.entries[i].tex)
        g_surface_views.entries[i].tex->lpVtbl->Release(g_surface_views.entries[i].tex);
    memset(&g_surface_views,0,sizeof(g_surface_views));
}
