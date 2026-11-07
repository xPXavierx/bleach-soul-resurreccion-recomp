// Captured PS3 outputs are the oracle. GPU readback is for this test only.
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <iterator>
#include "../rsx_geometry_visibility.h"
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
#define HR(x) CHECK(SUCCEEDED(x))
static ID3D12Resource* buffer(ID3D12Device*d,uint64_t size,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
 D3D12_HEAP_PROPERTIES h{};h.Type=heap;D3D12_RESOURCE_DESC b{};
 b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=size;b.Height=1;b.DepthOrArraySize=1;b.MipLevels=1;
 b.SampleDesc.Count=1;b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;b.Flags=flags;
 ID3D12Resource*r=nullptr;
 if(FAILED(d->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&b,state,nullptr,IID_PPV_ARGS(&r))))return nullptr;return r;
}
int main(int argc,char**argv){
 CHECK(argc==3);std::ifstream file(argv[1],std::ios::binary),shader_file(argv[2]);CHECK(file&&shader_file);
 std::string shader((std::istreambuf_iterator<char>(shader_file)),{});
 uint32_t magic=0,cases=0;file.read((char*)&magic,4);file.read((char*)&cases,4);CHECK(magic==0x47563290 && cases>0 && cases<1000);
 struct Case{uint32_t input,output,triangles;std::vector<uint32_t> expected;};
 std::vector<Case> ranges;std::vector<uint8_t> packets;uint32_t output_bytes=0;
 for(uint32_t i=0;i<cases;i++){
  uint32_t n=0,expected=0;file.read((char*)&n,4);file.read((char*)&expected,4);CHECK(file&&n>=192&&n<1000000&&expected<1000000);
  packets.resize((packets.size()+255)&~size_t(255));Case c{};c.input=(uint32_t)packets.size();
  packets.resize(c.input+n);file.read((char*)packets.data()+c.input,n);CHECK(file);
  memcpy(&c.triangles,packets.data()+c.input,4);CHECK(c.triangles&&c.triangles<65536);
  c.output=output_bytes;output_bytes=(output_bytes+c.triangles*16+255)&~255u;
  c.expected.resize(expected);file.read((char*)c.expected.data(),expected*4);CHECK(file);ranges.push_back(c);
 }
 ID3D12Device*device=nullptr;HR(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
 IDXGIFactory4*factory=nullptr;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));IDXGIAdapter1*adapter=nullptr;
 HR(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)));DXGI_ADAPTER_DESC1 desc{};HR(adapter->GetDesc1(&desc));
 CHECK(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE));printf("GPU vendor=%04x device=%04x\n",desc.VendorId,desc.DeviceId);adapter->Release();factory->Release();
 D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
 D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=params;ID3DBlob*blob=nullptr,*errors=nullptr;
 HR(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors));ID3D12RootSignature*root=nullptr;
 HR(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));blob->Release();
 HRESULT compiled=D3DCompile(shader.data(),shader.size(),"visibility",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
 if(errors){fprintf(stderr,"%s\n",(char*)errors->GetBufferPointer());errors->Release();}HR(compiled);
 D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root;pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};ID3D12PipelineState*pso=nullptr;
 HR(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)));blob->Release();
 auto*upload=buffer(device,packets.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
 auto*output=buffer(device,output_bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
 auto*readback=buffer(device,output_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);CHECK(upload&&output&&readback);
 void*mapped=nullptr;HR(upload->Map(0,nullptr,&mapped));memcpy(mapped,packets.data(),packets.size());upload->Unmap(0,nullptr);
 D3D12_COMMAND_QUEUE_DESC qd{};ID3D12CommandQueue*queue=nullptr;HR(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
 ID3D12CommandAllocator*alloc=nullptr;HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
 ID3D12GraphicsCommandList*list=nullptr;HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc,pso,IID_PPV_ARGS(&list)));list->SetComputeRootSignature(root);
 for(auto&c:ranges){
  rsx_geometry_visibility packet{};packet.pipeline=pso;packet.root=root;packet.output=output;
  packet.input_address=upload->GetGPUVirtualAddress()+c.input;packet.output_address=output->GetGPUVirtualAddress();
  packet.output_offset=c.output;packet.output_capacity=output_bytes;packet.triangles=c.triangles;
  CHECK(rsx_geometry_visibility_valid(&packet));
  auto invalid=packet;invalid.output_capacity=invalid.output_offset+invalid.triangles*16-1;
  CHECK(!rsx_geometry_visibility_valid(&invalid));
  invalid=packet;invalid.output_offset=UINT64_MAX;CHECK(!rsx_geometry_visibility_valid(&invalid));
  const auto view=rsx_geometry_visibility_begin(&packet,list);
  CHECK(view.BufferLocation==packet.output_address+c.output&&view.SizeInBytes==c.triangles*12&&view.Format==DXGI_FORMAT_R32_UINT);
  rsx_geometry_visibility_end(&packet,list);
 }
 D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
 barrier.Transition={output,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);
 list->CopyResource(readback,output);HR(list->Close());ID3D12CommandList*lists[]={list};queue->ExecuteCommandLists(1,lists);
 ID3D12Fence*fence=nullptr;HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);CHECK(event);
 HR(queue->Signal(fence,1));HR(fence->SetEventOnCompletion(1,event));CHECK(WaitForSingleObject(event,15000)==WAIT_OBJECT_0);
 HR(readback->Map(0,nullptr,&mapped));unsigned failures=0,total=0,kept=0;
 for(size_t i=0;i<ranges.size();i++){
  auto&c=ranges[i];auto*p=(uint32_t*)((uint8_t*)mapped+c.output);std::vector<uint32_t> actual;
  for(uint32_t t=0;t<c.triangles;t++){
   CHECK(p[c.triangles*3+t]<=1);
   if(p[c.triangles*3+t])actual.insert(actual.end(),p+t*3,p+t*3+3);
   else CHECK(p[t*3]==0&&p[t*3+1]==0&&p[t*3+2]==0);
  }
  total+=c.triangles;kept+=(unsigned)actual.size()/3;
  if(actual!=c.expected){printf("DIFF case=%zu expected_indices=%zu actual_indices=%zu\n",i,c.expected.size(),actual.size());failures++;}
 }
 readback->Unmap(0,nullptr);CloseHandle(event);fence->Release();list->Release();alloc->Release();queue->Release();
 upload->Release();output->Release();readback->Release();pso->Release();root->Release();device->Release();
 printf("%s cases=%zu input_triangles=%u kept=%u differing_cases=%u; fixed output slots, stable order, degenerate rejected slots\n",failures?"FAIL":"PASS",ranges.size(),total,kept,failures);return failures?2:0;
}
