#ifndef RSX_GEOMETRY_GPU_RUNTIME_H
#define RSX_GEOMETRY_GPU_RUNTIME_H
#include "rsx_geometry_visibility_source.h"
#define GEOMETRY_RING_SIZE (64u*1024u*1024u)
static ID3D12RootSignature* geometry_root;
static ID3D12PipelineState* geometry_pipeline;
static int geometry_failed;
static u64 geometry_gpu_draws,geometry_cpu_draws;
static int geometry_gpu_init(ID3D12Device* dev,ld_frame_resources* f) {
    if(geometry_failed)return 0;
    if(!geometry_pipeline) {
        ID3DBlob *blob=NULL,*error=NULL;
        D3D12_ROOT_PARAMETER p[2]={0};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_ROOT_SIGNATURE_DESC r={0};r.NumParameters=2;r.pParameters=p;
        HRESULT hr=D3D12SerializeRootSignature(&r,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
        if(error)error->lpVtbl->Release(error);error=NULL;
        if(FAILED(hr))goto fail;
        hr=dev->lpVtbl->CreateRootSignature(dev,0,blob->lpVtbl->GetBufferPointer(blob),blob->lpVtbl->GetBufferSize(blob),&IID_ID3D12RootSignature,(void**)&geometry_root);
        blob->lpVtbl->Release(blob);blob=NULL;if(FAILED(hr))goto fail;
        hr=D3DCompile(rsx_geometry_source,sizeof(rsx_geometry_source)-1,"native geometry",NULL,NULL,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
        if(error){fprintf(stderr,"[geometry-gpu] %s\n",(char*)error->lpVtbl->GetBufferPointer(error));error->lpVtbl->Release(error);}
        if(FAILED(hr))goto fail;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc={0};desc.pRootSignature=geometry_root;
        desc.CS.pShaderBytecode=blob->lpVtbl->GetBufferPointer(blob);desc.CS.BytecodeLength=blob->lpVtbl->GetBufferSize(blob);
        hr=dev->lpVtbl->CreateComputePipelineState(dev,&desc,&IID_ID3D12PipelineState,(void**)&geometry_pipeline);
        blob->lpVtbl->Release(blob);if(FAILED(hr))goto fail;
    }
    if(!f->geometry_input && !ld_frame_buffer(dev,GEOMETRY_RING_SIZE,&f->geometry_input,(void**)&f->geometry_mapped))goto fail;
    if(!f->geometry_output) {
        D3D12_HEAP_PROPERTIES h={0};h.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d={0};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=GEOMETRY_RING_SIZE;
        d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;
        d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if(FAILED(dev->lpVtbl->CreateCommittedResource(dev,&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            NULL,&IID_ID3D12Resource,(void**)&f->geometry_output)))goto fail;
    }
    return 1;
fail:
    geometry_failed=1;fprintf(stderr,"[geometry-gpu] initialization failed; using CPU recovery\n");return 0;
}
static void geometry_cpu_recover(const rsx_geometry_lease* lease) {
    if(!lease->cpu_fallback || !lease->index_destination ||
       lease->cpu_fallback(lease->data,lease->bytes,lease->index_destination,lease->input_indices*2)==UINT32_MAX)abort();
    ++geometry_cpu_draws;
}
static void geometry_gpu_packet(ld_frame_resources* f,const rsx_geometry_lease* lease,rsx_geometry_visibility* out) {
    const u32 input_bytes=((u32)lease->bytes+15)&~15u,output_bytes=lease->input_indices/3*16;
    if(f->geometry_input_used+input_bytes>GEOMETRY_RING_SIZE || f->geometry_output_used+output_bytes>GEOMETRY_RING_SIZE)abort();
    memcpy(f->geometry_mapped+f->geometry_input_used,lease->data,lease->bytes);
    out->pipeline=geometry_pipeline;out->root=geometry_root;out->output=f->geometry_output;
    out->input_address=f->geometry_input->lpVtbl->GetGPUVirtualAddress(f->geometry_input)+f->geometry_input_used;
    out->output_address=f->geometry_output->lpVtbl->GetGPUVirtualAddress(f->geometry_output);
    out->output_offset=f->geometry_output_used;out->output_capacity=GEOMETRY_RING_SIZE;out->triangles=lease->input_indices/3;
    f->geometry_input_used+=input_bytes;f->geometry_output_used+=output_bytes;
    ++geometry_gpu_draws;
    if((geometry_gpu_draws&8191)==0)fprintf(stderr,"[geometry-gpu] draws=%llu cpu_recovery=%llu\n",
        (unsigned long long)geometry_gpu_draws,(unsigned long long)geometry_cpu_draws);
}
#endif
