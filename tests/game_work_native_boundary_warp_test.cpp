#include "../src/game_work_trace.hpp"
#include "../src/log_queue.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace { std::vector<std::string> lines; }
namespace mgpu::lq
{
bool running() { return true; }
void post(dest, const char *line) { lines.emplace_back(line); }
}

int main(int argc, char **argv)
{
    const bool reset_only=argc>1 && std::strcmp(argv[1],"--reset-only")==0;
    const unsigned expected_jobs=reset_only ? 3 : 5;
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) return 1;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC description{};
    if (FAILED(device->CreateCommandQueue(&description,IID_PPV_ARGS(&queue)))) return 2;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command))) ||
        FAILED(command->Close())) return 3;

    ComPtr<ID3D12Resource> upload,readback;
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width=256;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;
    buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)))) return 4;
    heap.Type=D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)))) return 5;
    void *mapped=nullptr;
    if (FAILED(upload->Map(0,nullptr,&mapped))) return 6;
    std::memset(mapped,0x5a,256);
    upload->Unmap(0,nullptr);

    mgpu::game_work::bind_device(device.Get());
    mgpu::game_work::poll(123);
    Sleep(2050);
    mgpu::game_work::poll(123);
    if (!mgpu::game_work::active()) return 7;
    mgpu::game_work::observe_native_list(command.Get());
    if (FAILED(command->Reset(allocator.Get(),nullptr))) return 8;
    if (SUCCEEDED(command->Reset(allocator.Get(),nullptr))) return 9;
    command->CopyBufferRegion(readback.Get(),0,upload.Get(),0,128);
    mgpu::game_work::close(command.Get());
    command->CopyBufferRegion(readback.Get(),128,upload.Get(),128,128);
    if (FAILED(command->Close())) return 10;
    std::vector<ID3D12CommandList *> lists{command.Get()};
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> commands;
    for (unsigned index=0;index<2;++index)
    {
        ComPtr<ID3D12CommandAllocator> next_allocator;
        ComPtr<ID3D12GraphicsCommandList> next;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&next_allocator))) ||
            FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,next_allocator.Get(),nullptr,IID_PPV_ARGS(&next))) ||
            FAILED(next->Close()) || FAILED(next->Reset(next_allocator.Get(),nullptr))) return 11;
        if (index==1)
        {
            mgpu::game_work::marker(next.Get(),mgpu::game_work::phase::target_change,0,UINT_MAX,UINT_MAX);
            for (unsigned mark=0;mark<62;++mark)
                mgpu::game_work::marker(next.Get(),mark%2 ? mgpu::game_work::phase::graphics : mgpu::game_work::phase::compute);
        }
        if (FAILED(next->Close())) return 12;
        lists.push_back(next.Get());
        commands.push_back(next);
        allocators.push_back(next_allocator);
    }

    ComPtr<ID3D12Device> other_device;
    ComPtr<ID3D12CommandAllocator> other_allocator;
    ComPtr<ID3D12GraphicsCommandList> other;
    if (FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&other_device))) ||
        FAILED(other_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&other_allocator))) ||
        FAILED(other_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,other_allocator.Get(),nullptr,IID_PPV_ARGS(&other))) ||
        FAILED(other->Close())) return 13;
    if (other_device.Get()!=device.Get())
    {
        if (FAILED(other->Reset(other_allocator.Get(),nullptr)) || FAILED(other->Close())) return 13;
    }
    else std::puts("SKIP: independent same-LUID device isolation; D3D12 WARP returned the existing singleton device");

    mgpu::game_work::before_submit(queue.Get(),UINT(lists.size()),lists.data());
    queue->ExecuteCommandLists(UINT(lists.size()),lists.data());
    mgpu::game_work::after_submit(queue.Get(),UINT(lists.size()),lists.data());
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))) || FAILED(queue->Signal(fence.Get(),1))) return 14;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!event || FAILED(fence->SetEventOnCompletion(1,event)) || WaitForSingleObject(event,10000)!=WAIT_OBJECT_0) return 15;
    CloseHandle(event);
    if (FAILED(readback->Map(0,nullptr,&mapped))) return 16;
    for (unsigned index=0;index<256;++index) if (static_cast<unsigned char *>(mapped)[index]!=0x5a) return 17;
    readback->Unmap(0,nullptr);
    std::vector<ComPtr<ID3D12CommandAllocator>> scope_allocators;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> scope_commands;
    mgpu::game_work::feature_created(0x101,1);
    for (unsigned index=0;index<(reset_only ? 0u : 2u);++index)
    {
        ComPtr<ID3D12CommandAllocator> scope_allocator;
        ComPtr<ID3D12GraphicsCommandList> scope_command;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&scope_allocator))) ||
            FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,scope_allocator.Get(),nullptr,IID_PPV_ARGS(&scope_command))) ||
            FAILED(scope_command->Close()) || FAILED(scope_command->Reset(scope_allocator.Get(),nullptr))) return 23;
        mgpu::game_work::marker(scope_command.Get(),index==0 ? mgpu::game_work::phase::ngx_begin : mgpu::game_work::phase::sl_sr_begin,0x101);
        scope_commands.push_back(scope_command);
        scope_allocators.push_back(scope_allocator);
    }
    mgpu::game_work::poll(0);
    if (mgpu::game_work::active()) return 24;
    if (!scope_commands.empty())
    {
        Sleep(800);
        mgpu::game_work::poll(0);
        for (const auto &line:lines)
            if (line.find("reason=submitted_jobs_drained")!=std::string::npos) return 30;
    }
    std::vector<ID3D12CommandList *> scope_lists;
    for (unsigned index=0;index<scope_commands.size();++index)
    {
        const auto ending=index==0 ? mgpu::game_work::phase::ngx_end : mgpu::game_work::phase::sl_sr_end;
        const auto wrong=index==0 ? mgpu::game_work::phase::sl_sr_end : mgpu::game_work::phase::ngx_end;
        mgpu::game_work::marker(scope_commands[index].Get(),wrong,0x101);
        mgpu::game_work::marker(scope_commands[index].Get(),ending,0x101);
        mgpu::game_work::marker(scope_commands[index].Get(),ending,0x101);
        mgpu::game_work::marker(scope_commands[index].Get(),mgpu::game_work::phase::ngx_begin,0x101);
        if (FAILED(scope_commands[index]->Close())) return 25;
        scope_lists.push_back(scope_commands[index].Get());
    }
    mgpu::game_work::feature_released(0x101);
    if (!scope_lists.empty())
    {
        mgpu::game_work::before_submit(queue.Get(),UINT(scope_lists.size()),scope_lists.data());
        queue->ExecuteCommandLists(UINT(scope_lists.size()),scope_lists.data());
        mgpu::game_work::after_submit(queue.Get(),UINT(scope_lists.size()),scope_lists.data());
    }
    if (FAILED(queue->Signal(fence.Get(),2))) return 26;
    event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!event || FAILED(fence->SetEventOnCompletion(2,event)) || WaitForSingleObject(event,10000)!=WAIT_OBJECT_0) return 27;
    CloseHandle(event);
    Sleep(800);
    for (unsigned iteration=0;iteration<5;++iteration)
    {
        const auto first=lines.size();
        mgpu::game_work::poll(0);
        unsigned emitted=0;
        for (auto index=first;index<lines.size();++index)
        {
            if (lines[index].size()>=1900) return 18;
            emitted+=lines[index].find("kind=job")!=std::string::npos;
        }
        if (emitted>8) return 19;
    }
    bool drained=false,empty=false,fragmented=false,origin=false;
    unsigned full=0;
    for (const auto &line:lines)
    {
        if (line.find("[MGPU][M337] kind=job ")==0)
        {
            full+=line.find("native_start=1 native_end=1")!=std::string::npos;
            if (line.find("marks=2 ")!=std::string::npos) empty=true;
        }
        if (line.find("kind=job_part ")!=std::string::npos) fragmented=true;
        if (line.find("kind=origin ")!=std::string::npos) origin=true;
        if (line.find("reason=submitted_jobs_drained")!=std::string::npos)
        {
            drained=line.find("retired="+std::to_string(expected_jobs)+" ")!=std::string::npos &&
                line.find("encoding_errors=0")!=std::string::npos &&
                line.find("overflow_jobs=0")!=std::string::npos &&
                line.find("native_starts="+std::to_string(expected_jobs)+" native_closes="+std::to_string(expected_jobs)+" native_reset_failures=1")!=std::string::npos;
        }
        if (argc>1) std::puts(line.c_str());
    }
    if (!drained || !empty || !fragmented || !origin || full!=expected_jobs-1)
    {
        std::printf("FAIL: drained=%u empty=%u fragmented=%u origin=%u direct_full_records=%u\n",
            unsigned(drained),unsigned(empty),unsigned(fragmented),unsigned(origin),full);
        return 20;
    }
    if (FAILED(command->Reset(allocator.Get(),nullptr)) || FAILED(command->Close())) return 21;
    const auto before=lines.size();
    ID3D12CommandList *reused=command.Get();
    mgpu::game_work::marker(command.Get(),mgpu::game_work::phase::ngx_end,0x101);
    mgpu::game_work::before_submit(queue.Get(),1,&reused);
    queue->ExecuteCommandLists(1,&reused);
    mgpu::game_work::after_submit(queue.Get(),1,&reused);
    if (FAILED(queue->Signal(fence.Get(),3))) return 28;
    event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!event || FAILED(fence->SetEventOnCompletion(3,event)) || WaitForSingleObject(event,10000)!=WAIT_OBJECT_0) return 29;
    CloseHandle(event);
    mgpu::game_work::poll(0);
    if (lines.size()!=before || mgpu::game_work::active()) return 22;
    std::puts("PASS: WARP native boundaries, off-window Reset/reexecute, paired NGX/SL completion after stop, no new scopes, failed Reset and compact fragmentation");
    return 0;
}
