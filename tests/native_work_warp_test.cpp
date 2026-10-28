#include "../src/game_work_trace.hpp"
#include "../src/log_queue.hpp"
#include "../src/native_work_observer.hpp"
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

namespace
{
std::vector<std::string> lines;
unsigned before_count=0,after_count=0,pipeline_count=0;
unsigned stopping_before(ID3D12GraphicsCommandList *,mgpu::native_work::operation kind,ID3D12Object *,UINT64,UINT64,UINT64)
{
    if (kind!=mgpu::native_work::operation::copy_buffer) return UINT_MAX;
    ++before_count;
    mgpu::native_work::set_running(false);
    return 0;
}
void stopping_after(ID3D12GraphicsCommandList *,unsigned token) { if (token==0) ++after_count; }
void stopping_pipeline(ID3D12GraphicsCommandList *,ID3D12PipelineState *) { ++pipeline_count; }
}
namespace mgpu::lq
{
bool running() { return true; }
void post(dest,const char *line) { lines.emplace_back(line); }
}

__declspec(noinline) HRESULT record_iteration(ID3D12GraphicsCommandList *command,
    ID3D12CommandAllocator *allocator,ID3D12PipelineState *pipeline,ID3D12RootSignature *root,
    ID3D12Resource *upload,ID3D12Resource *readback,ID3D12Resource *output,unsigned iteration,
    ID3D12DescriptorHeap *gpu_heap,D3D12_CPU_DESCRIPTOR_HANDLE cpu,D3D12_GPU_DESCRIPTOR_HANDLE gpu,
    UINT descriptor_size,ID3D12Resource *clear_readback,ID3D12GraphicsCommandList *bundle)
{
    auto result=command->Reset(allocator,pipeline);
    if (FAILED(result)) return result;
    if (iteration==4) return command->Close();
    if (iteration!=0)
    {
        command->SetPipelineState(pipeline);
        command->SetComputeRootSignature(root);
        command->SetComputeRootUnorderedAccessView(0,output->GetGPUVirtualAddress());
        command->Dispatch(1,1,1);
        if (iteration==3)
        {
            mgpu::game_work::marker(command,mgpu::game_work::phase::compute);
            mgpu::game_work::callback_marker(command,mgpu::game_work::unclassified_callback::acceleration_build);
            command->Dispatch(1,1,1);
            mgpu::game_work::marker(command,mgpu::game_work::phase::ngx_begin,0x202);
            command->CopyBufferRegion(readback,0,upload,0,64);
            mgpu::game_work::marker(command,mgpu::game_work::phase::ngx_end,0x202);
            command->SetDescriptorHeaps(1,&gpu_heap);
            const UINT uint_values[]={0x11223344,0,0,0};
            const FLOAT float_values[]={0.5f,0,0,0};
            auto copy_clear=[&](UINT64 offset)
            {
                D3D12_RESOURCE_BARRIER transition{};
                transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                transition.Transition={output,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
                command->ResourceBarrier(1,&transition);
                command->CopyBufferRegion(clear_readback,offset,output,0,64);
                std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);
                command->ResourceBarrier(1,&transition);
            };
            mgpu::game_work::callback_marker(command,mgpu::game_work::unclassified_callback::uav_uint);
            command->ClearUnorderedAccessViewUint(gpu,cpu,output,uint_values,0,nullptr);
            copy_clear(0);
            cpu.ptr+=descriptor_size; gpu.ptr+=descriptor_size;
            mgpu::game_work::callback_marker(command,mgpu::game_work::unclassified_callback::uav_float);
            command->ClearUnorderedAccessViewFloat(gpu,cpu,output,float_values,0,nullptr);
            copy_clear(128);
            mgpu::game_work::callback_marker(command,mgpu::game_work::unclassified_callback::bundle);
            command->ExecuteBundle(bundle);
            command->SetPipelineState(pipeline);
            command->SetComputeRootSignature(root);
            command->SetComputeRootUnorderedAccessView(0,output->GetGPUVirtualAddress());
            command->Dispatch(1,1,1);
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource=output;
        command->ResourceBarrier(1,&barrier);
    }
    for (unsigned copy=0;copy<(iteration==2 ? 200u : 1u);++copy)
        command->CopyBufferRegion(readback,0,upload,0,256);
    return command->Close();
}

int main(int argc,char **argv)
{
    const bool capacity=argc>1 && std::strcmp(argv[1],"--capacity")==0;
    const unsigned iterations=capacity ? 48u : 5u;
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) return 1;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    if (FAILED(device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue)))) return 2;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command))) ||
        FAILED(command->Close())) return 3;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters=1; root_desc.pParameters=&parameter;
    ComPtr<ID3DBlob> root_blob,error,shader;
    ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&error)) ||
        FAILED(device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root)))) return 4;
    const char *source="RWByteAddressBuffer output : register(u0); [numthreads(1,1,1)] void main() { output.Store(0,0x12345678); }";
    if (FAILED(D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&shader,&error))) return 5;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature=root.Get();
    pipeline_desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateComputePipelineState(&pipeline_desc,IID_PPV_ARGS(&pipeline)))) return 6;
    pipeline->SetName(L"M339 WARP compute forwarding");
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=256;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;
    buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    ComPtr<ID3D12Resource> upload,readback,output;
    ComPtr<ID3D12Resource> clear_readback;
    heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)))) return 7;
    heap.Type=D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)))) return 8;
    if (FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&clear_readback)))) return 28;
    heap.Type=D3D12_HEAP_TYPE_DEFAULT; buffer.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)))) return 9;
    ComPtr<ID3D12DescriptorHeap> cpu_heap,gpu_heap;
    D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
    descriptors.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; descriptors.NumDescriptors=2;
    if (FAILED(device->CreateDescriptorHeap(&descriptors,IID_PPV_ARGS(&cpu_heap)))) return 29;
    descriptors.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&descriptors,IID_PPV_ARGS(&gpu_heap)))) return 30;
    const auto descriptor_size=device->GetDescriptorHandleIncrementSize(descriptors.Type);
    auto cpu=cpu_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER; view.Format=DXGI_FORMAT_R32_TYPELESS;
    view.Buffer.NumElements=64; view.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(output.Get(),nullptr,&view,cpu);
    cpu.ptr+=descriptor_size;
    view.Format=DXGI_FORMAT_R32_FLOAT; view.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(output.Get(),nullptr,&view,cpu);
    device->CopyDescriptorsSimple(2,gpu_heap->GetCPUDescriptorHandleForHeapStart(),cpu_heap->GetCPUDescriptorHandleForHeapStart(),descriptors.Type);
    ComPtr<ID3D12CommandAllocator> bundle_allocator;
    ComPtr<ID3D12GraphicsCommandList> bundle;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE,IID_PPV_ARGS(&bundle_allocator))) ||
        FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_BUNDLE,bundle_allocator.Get(),nullptr,IID_PPV_ARGS(&bundle))) || FAILED(bundle->Close())) return 31;
    void *mapped=nullptr;
    if (FAILED(upload->Map(0,nullptr,&mapped))) return 10;
    std::memset(mapped,0x5a,256); upload->Unmap(0,nullptr);
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) return 11;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!event) return 12;
    if (argc>1 && std::strcmp(argv[1],"--observer-stop")==0)
    {
        const auto coverage=mgpu::native_work::install(command.Get(),stopping_before,stopping_after,stopping_pipeline);
        if (coverage.failures || !mgpu::native_work::select(command.Get(),true) || FAILED(command->Reset(allocator.Get(),nullptr))) return 24;
        mgpu::native_work::set_running(true);
        command->SetPipelineState(pipeline.Get());
        command->CopyBufferRegion(readback.Get(),0,upload.Get(),0,128);
        command->CopyBufferRegion(readback.Get(),128,upload.Get(),128,128);
        if (FAILED(command->Close()) || before_count!=1 || after_count!=1 || pipeline_count!=1) return 25;
        ID3D12CommandList *list=command.Get(); queue->ExecuteCommandLists(1,&list);
        if (FAILED(queue->Signal(fence.Get(),1)) || FAILED(fence->SetEventOnCompletion(1,event)) ||
            WaitForSingleObject(event,10000)!=WAIT_OBJECT_0 || FAILED(readback->Map(0,nullptr,&mapped))) return 26;
        for (unsigned index=0;index<256;++index) if (static_cast<unsigned char *>(mapped)[index]!=0x5a) return 27;
        readback->Unmap(0,nullptr); CloseHandle(event);
        mgpu::native_work::select(command.Get(),false);
        std::puts("PASS: observer after callback survives recording stop during before; unselected work forwards unchanged");
        return 0;
    }
    mgpu::game_work::bind_device(device.Get());
    mgpu::game_work::poll(339);
    Sleep(2050);
    mgpu::game_work::poll(339);
    mgpu::game_work::observe_native_list(command.Get());
    for (unsigned iteration=0;iteration<iterations;++iteration)
    {
        if (FAILED(allocator->Reset()) || FAILED(record_iteration(command.Get(),allocator.Get(),pipeline.Get(),root.Get(),
            upload.Get(),readback.Get(),output.Get(),iteration,gpu_heap.Get(),cpu_heap->GetCPUDescriptorHandleForHeapStart(),
            gpu_heap->GetGPUDescriptorHandleForHeapStart(),descriptor_size,clear_readback.Get(),bundle.Get()))) return 13;
        ID3D12CommandList *list=command.Get();
        mgpu::game_work::before_submit(queue.Get(),1,&list);
        queue->ExecuteCommandLists(1,&list);
        mgpu::game_work::after_submit(queue.Get(),1,&list);
        if (FAILED(queue->Signal(fence.Get(),iteration+1)) || FAILED(fence->SetEventOnCompletion(iteration+1,event)) ||
            WaitForSingleObject(event,10000)!=WAIT_OBJECT_0) return 14;
        mgpu::game_work::poll(339);
    }
    if (FAILED(readback->Map(0,nullptr,&mapped))) return 15;
    for (unsigned index=0;index<256;++index) if (static_cast<unsigned char *>(mapped)[index]!=0x5a) return 16;
    readback->Unmap(0,nullptr);
    if (FAILED(clear_readback->Map(0,nullptr,&mapped))) return 32;
    for (unsigned index=0;index<16;++index)
        if (static_cast<unsigned *>(mapped)[index]!=0x11223344 || static_cast<unsigned *>(mapped)[32+index]!=0x3f000000) return 33;
    clear_readback->Unmap(0,nullptr);
    mgpu::game_work::poll(0);
    if (FAILED(allocator->Reset()) || FAILED(command->Reset(allocator.Get(),nullptr))) return 17;
    D3D12_RESOURCE_BARRIER transition{};
    transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
    command->ResourceBarrier(1,&transition);
    command->CopyBufferRegion(readback.Get(),0,output.Get(),0,256);
    if (FAILED(command->Close())) return 18;
    ID3D12CommandList *list=command.Get(); queue->ExecuteCommandLists(1,&list);
    if (FAILED(queue->Signal(fence.Get(),iterations+1)) || FAILED(fence->SetEventOnCompletion(iterations+1,event)) ||
        WaitForSingleObject(event,10000)!=WAIT_OBJECT_0) return 19;
    CloseHandle(event);
    if (FAILED(readback->Map(0,nullptr,&mapped))) { std::puts("FAIL: final compute readback Map"); return 20; }
    if (*static_cast<unsigned *>(mapped)!=0x12345678)
    { std::printf("FAIL: final compute value=%08x expected=12345678\n",*static_cast<unsigned *>(mapped)); return 20; }
    readback->Unmap(0,nullptr);
    Sleep(800);
    for (unsigned poll=0;poll<64;++poll)
    {
        const auto first=lines.size(); mgpu::game_work::poll(0);
        unsigned records=0;
        for (auto index=first;index<lines.size();++index)
        {
            if (lines[index].size()>=1900) return 21;
            if (lines[index].find("kind=job")!=std::string::npos || lines[index].find("kind=fine")!=std::string::npos ||
                lines[index].find("kind=seen")!=std::string::npos || lines[index].find("kind=label")!=std::string::npos ||
                lines[index].find("kind=callbacks")!=std::string::npos) ++records;
        }
        if (records>8) return 22;
    }
    unsigned fine=0,observations=0; bool learned=false,truncated=false,mixed=false,drained=false,empty_seen=false,callback_ids=false,scope_suppressed=false;
    for (const auto &line_text:lines)
    {
        learned|=line_text.find("kind=learn ")!=std::string::npos;
        if (line_text.find("kind=seen ")!=std::string::npos)
        {
            ++observations;
            empty_seen|=line_text.find("total=0 budget_skipped=0 scope_skipped=0 mixed=0 truncated=0 cutoff=0")!=std::string::npos;
            scope_suppressed|=line_text.find("scope_skipped=1 mixed=1 truncated=0 cutoff=0 mixed_continues=1")!=std::string::npos;
        }
        if (line_text.find("kind=fine ")!=std::string::npos)
        {
            ++fine;
            truncated|=line_text.find("count=160 truncated=1 mixed=0")!=std::string::npos;
            mixed|=line_text.find("truncated=0 mixed=1 timing=queries mixed_continues=1")!=std::string::npos &&
                line_text.find("count=1 ")==std::string::npos;
        }
        callback_ids|=line_text.find("kind=callbacks ")!=std::string::npos && line_text.find("total=4 counts=1,1,1,1")!=std::string::npos;
        if (line_text.find("reason=submitted_jobs_drained")!=std::string::npos)
            drained=line_text.find("retired="+std::to_string(iterations)+" ")!=std::string::npos && line_text.find("overflow_jobs=0 ")!=std::string::npos &&
                line_text.find("invalid_timestamps=0 encoding_errors=0 ")!=std::string::npos && line_text.find("scope_pending=0")!=std::string::npos;
        if (argc>1) std::puts(line_text.c_str());
    }
    if (!learned || fine!=iterations-2 || observations!=iterations-1 || !empty_seen || !truncated || !mixed || !drained || !callback_ids || !scope_suppressed)
    { std::printf("FAIL: learned=%u fine=%u truncated=%u mixed=%u drained=%u\n",learned,fine,truncated,mixed,drained); return 23; }
    std::puts("PASS: WARP native queries continue after mixed callbacks; all four synthetic callback identities; actual dispatch/UAV clears/bundle/copy forwarding; AS/Meta hardware not tested");
}
