/* GPU oracle: production decoder versus existing independent CPU fetcher.
 * Source memory is destroyed after packet serialization, before submission. */
#include "rsx_vertex_pull.h"
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include "rsx_vertex_formats.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
#define HR(x) CHECK(SUCCEEDED(x))
static uint32_t state=0x6173f12d;
static uint32_t next(){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
static void be32(uint8_t* p,uint32_t w){p[0]=w>>24;p[1]=w>>16;p[2]=w>>8;p[3]=w;}
static ID3D12Resource* buffer(ID3D12Device* d,uint64_t size,D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_STATES initial,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
    D3D12_HEAP_PROPERTIES h{};h.Type=heap;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=size;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;
    desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;
    ID3D12Resource* r=nullptr;
    if(FAILED(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&desc,initial,nullptr,IID_PPV_ARGS(&r))))return nullptr;
    return r;
}
int main(){
    const uint32_t count=64,cases=192;
    struct Case {uint32_t input,output;};std::vector<Case> ranges;
    std::vector<uint8_t> packets;std::vector<float> expected;
    std::vector<uint8_t> types;
    uint64_t shared_spans=0;
    for(uint32_t test=0;test<cases;test++){
        const bool linear=test>=128;
        uint8_t source[16][64*32+32];for(auto& row:source)for(auto& b:row)b=(uint8_t)next();
        rsx_vertex_fetch_plan f{};rsx_vertex_layout_plan_init(&f.layout,0xffff);
        rsx_vertex_ref refs[count];
        for(uint32_t v=0;v<count;v++)refs[v]={!linear&&test%8==0?0xffff0u+v:v,!linear&&test%8==0?16u:0u};
        for(uint32_t a=0;a<16;a++){
            auto& attr=f.attr[a];uint32_t type=1+(a+test)%7,size=1+(a+test/7)%4;
            attr.desc.type=type;attr.desc.size=size;attr.desc.frequency=linear?0:test%8==0?0:(a%4==0?7:a%4);
            if(a&1)f.divider_mask|=1u<<a;
            attr.stride=32;attr.elem_size=rsx_vertex_attrib_size(type,size);
            attr.stable_base=source[a]+test%4;attr.stable_first=0;attr.stable_last=count-1;
            attr.default_value[0]=.25f;attr.default_value[1]=-.5f;attr.default_value[2]=.75f;attr.default_value[3]=1;
            if(type==2)for(uint32_t v=0;v<count;v++)for(uint32_t c=0;c<size;c++){
                uint32_t w=next();if(v%4)w=(w&0x807fffffu)|((100+(w%50))<<23);
                be32(source[a]+test%4+v*32+c*4,w);
            }
            if(type==1 || type==5 || type==3) {
                const uint16_t bounds[]={0x8000,0x7fff,0x0001,0x8001,0x7c00,0xfc00,0x03ff,0x0400};
                for(uint32_t v=0;v<8;v++){uint8_t* p=source[a]+test%4+v*32;p[0]=bounds[v]>>8;p[1]=bounds[v];}
            }
            if(a==15 && test%2)attr.desc.type=0;
            if(a==14 && test%3==0){attr=f.attr[13];shared_spans++;}
        }
        rsx_vertex_pull_plan p{};
        if(linear) {
            CHECK(rsx_vertex_pull_prepare_linear(&p,&f,count,1024*1024));
            CHECK(!p.refs&&!p.header.refs_offset&&p.header.version==2);
            rsx_vertex_pull_plan legacy{};CHECK(rsx_vertex_pull_prepare(&legacy,&f,refs,count,1024*1024));
            CHECK(legacy.bytes==p.bytes+count*sizeof(rsx_vertex_ref));
            auto invalid=f;invalid.attr[0].stable_last=count-2;
            auto untouched=p;CHECK(!rsx_vertex_pull_prepare_linear(&p,&invalid,count,1024*1024));
            CHECK(!memcmp(&p,&untouched,sizeof(p)));
            invalid=f;invalid.attr[0].desc.frequency=2;CHECK(!rsx_vertex_pull_prepare_linear(&p,&invalid,count,1024*1024));
        } else CHECK(rsx_vertex_pull_prepare(&p,&f,refs,count,1024*1024));
        CHECK(p.bytes%16==0 && p.span_count<=16);
        auto sentinel=p;CHECK(!rsx_vertex_pull_prepare(&p,&f,refs,count,64));
        CHECK(!memcmp(&p,&sentinel,sizeof(p)));
        uint32_t offset=(uint32_t)packets.size();packets.resize(offset+p.bytes);
        CHECK(rsx_vertex_pull_write(&p,packets.data()+offset,p.bytes));
        CHECK(!rsx_vertex_pull_write(&p,packets.data()+offset,p.bytes-1));
        ranges.push_back({offset,(uint32_t)(expected.size()*4)});
        for(uint32_t a=0;a<16;a++)for(uint32_t v=0;v<count;v++){
            rsx_vertex_fetch_plan one=f;rsx_vertex_layout_plan_init(&one.layout,1u<<a);
            float value[4];CHECK(rsx_vertex_fetch_one(&one,&refs[v],(uint8_t*)value));
            for(float c:value){expected.push_back(c);types.push_back((uint8_t)f.attr[a].desc.type);}
        }
        // Serialized submissions must own all data, not retain guest pointers.
        memset(source,0xdd,sizeof(source));memset(refs,0xee,sizeof(refs));
    }
    CHECK(shared_spans>0);
    // Fail closed on an unmapped input; preserve the caller's CPU fallback.
    {rsx_vertex_fetch_plan f{};rsx_vertex_layout_plan_init(&f.layout,1);f.attr[0].desc.type=2;
     f.attr[0].desc.size=3;rsx_vertex_ref ref{};rsx_vertex_pull_plan p{};
     CHECK(!rsx_vertex_pull_prepare(&p,&f,&ref,1,4096));
     uint8_t bytes[16]{};f.inline_data=bytes;f.inline_bytes=16;f.attr[0].stable_base=bytes;
     f.attr[0].stable_last=0;f.attr[0].stride=16;f.attr[0].elem_size=12;ref.vertex_id=1;
     CHECK(!rsx_vertex_pull_prepare(&p,&f,&ref,1,4096));}
    // Production wrapper must also compile for the zero-input shader signature.
    for(uint32_t mask:{0u,0xffffu}){
        char shader[20000]="struct VSInput {";
        if(mask)for(uint32_t a=0;a<16;a++){char tmp[80];snprintf(tmp,sizeof(tmp),"float4 a%u:ATTR%u;",a,a);strcat(shader,tmp);}
        else strcat(shader,"uint vertex_id:SV_VertexID;");
        strcat(shader,"};struct VSOutput{float4 p:SV_Position;};VSOutput main(VSInput input){VSOutput o;o.p=float4(0,0,0,1);return o;}");
        CHECK(rsx_vertex_pull_wrap(shader,sizeof(shader),mask));ID3DBlob* blob=nullptr;ID3DBlob* errors=nullptr;
        HRESULT h=D3DCompile(shader,strlen(shader),"pull_vs",nullptr,nullptr,"main","vs_5_0",0,0,&blob,&errors);
        if(errors){fprintf(stderr,"%s\n",(char*)errors->GetBufferPointer());errors->Release();}HR(h);blob->Release();
    }
    ID3D12Device* device=nullptr;HR(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    D3D12_ROOT_PARAMETER param[2]{};param[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
    param[0].Descriptor.ShaderRegister=20;param[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=param;
    ID3DBlob* blob=nullptr;ID3DBlob* errors=nullptr;
    HR(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors));
    ID3D12RootSignature* root=nullptr;HR(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));blob->Release();
    std::string shader=rsx_vertex_pull_hlsl();
    shader+="RWByteAddressBuffer result:register(u0);[numthreads(64,1,1)]void main(uint3 id:SV_DispatchThreadID){uint n=rsx_raw_vertices.Load(4);if(id.x<n&&id.y<16)result.Store4((id.y*n+id.x)*16,asuint(rsx_pull_fetch(id.x,id.y)));}";
    HRESULT compiled=D3DCompile(shader.data(),shader.size(),"pull_oracle",nullptr,nullptr,"main","cs_5_0",0,0,&blob,&errors);
    if(errors){fprintf(stderr,"%s\n",(char*)errors->GetBufferPointer());errors->Release();}HR(compiled);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root;pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
    ID3D12PipelineState* pso=nullptr;HR(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)));blob->Release();
    auto* upload=buffer(device,packets.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto* output=buffer(device,expected.size()*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto* readback=buffer(device,expected.size()*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(upload&&output&&readback);void* mapped=nullptr;HR(upload->Map(0,nullptr,&mapped));memcpy(mapped,packets.data(),packets.size());upload->Unmap(0,nullptr);
    D3D12_COMMAND_QUEUE_DESC qd{};ID3D12CommandQueue* queue=nullptr;HR(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ID3D12CommandAllocator* alloc=nullptr;HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
    ID3D12GraphicsCommandList* list=nullptr;HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc,pso,IID_PPV_ARGS(&list)));
    list->SetComputeRootSignature(root);
    for(auto range:ranges){list->SetComputeRootShaderResourceView(0,upload->GetGPUVirtualAddress()+range.input);
        list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress()+range.output);list->Dispatch(1,16,1);}
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={output,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);
    list->CopyResource(readback,output);HR(list->Close());ID3D12CommandList* lists[]={list};queue->ExecuteCommandLists(1,lists);
    ID3D12Fence* fence=nullptr;HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);CHECK(event);HR(queue->Signal(fence,1));HR(fence->SetEventOnCompletion(1,event));CHECK(WaitForSingleObject(event,30000)==WAIT_OBJECT_0);
    HR(readback->Map(0,nullptr,&mapped));auto* actual=(float*)mapped;uint64_t exact=0,rounded=0,nan=0;
    for(size_t i=0;i<expected.size();i++){
        if(!memcmp(actual+i,&expected[i],4)){exact++;continue;}
        if(std::isnan(actual[i])&&std::isnan(expected[i])){nan++;continue;}
        bool normalized=types[i]==1||types[i]==4||types[i]==6;
        if(normalized && std::isfinite(actual[i]) && std::fabs(actual[i]-expected[i])<=std::max(1.0f,std::fabs(expected[i]))*1.2e-7f){rounded++;continue;}
        fprintf(stderr,"component%zu type%u actual%.9g expected%.9g\n",i,types[i],actual[i],expected[i]);CHECK(false);
    }
    readback->Unmap(0,nullptr);CloseHandle(event);fence->Release();list->Release();alloc->Release();queue->Release();
    upload->Release();output->Release();readback->Release();pso->Release();root->Release();device->Release();
    printf("PASS GPU decoder: %zu components, %llu bit-exact, %llu normalized within float rounding, %llu NaNs; all seven formats, signed bounds, unaligned sources, defaults, div/mod/base wrap\n",expected.size(),exact,rounded,nan);
    puts("PASS owned packets survive destroyed source/ref data; shared spans, capacity/invalid fallbacks; both VS signatures compile; 64 original-vertex cases without reference tables");return 0;
}
