#include "../src/game_work_trace.hpp"
#include "../src/log_queue.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
#include <vector>

namespace { std::vector<std::string> lines; }
namespace mgpu::lq
{
bool running() { return true; }
void post(dest, const char *line) { lines.emplace_back(line); }
}

int main(int argc, char **)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return 1;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue)))) return 2;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    if (FAILED(device->CreateCommandAllocator(description.Type, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, description.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&command)))) return 3;
    mgpu::game_work::bind_device(device.Get());
    mgpu::game_work::poll(123);
    Sleep(2050);
    mgpu::game_work::poll(123);
    if (!mgpu::game_work::active()) return 4;
    mgpu::game_work::marker(command.Get(), mgpu::game_work::phase::graphics, 123, 0, 0);
    mgpu::game_work::marker(command.Get(), mgpu::game_work::phase::target_change, 456, 2560, 1440);
    mgpu::game_work::marker(command.Get(), mgpu::game_work::phase::graphics);
    mgpu::game_work::marker(command.Get(), mgpu::game_work::phase::sl_sr_begin);
    mgpu::game_work::marker(command.Get(), mgpu::game_work::phase::compute, 88);
    mgpu::game_work::marker(command.Get(), mgpu::game_work::phase::compute);
    mgpu::game_work::marker(command.Get(), mgpu::game_work::phase::sl_sr_end);
    mgpu::game_work::feature_created(0x101,1);
    mgpu::game_work::marker(command.Get(),mgpu::game_work::phase::ngx_begin,0x101);
    mgpu::game_work::marker(command.Get(),mgpu::game_work::phase::ngx_end,0x101);
    mgpu::game_work::feature_released(0x101);
    mgpu::game_work::marker(command.Get(),mgpu::game_work::phase::raytrace,0,1280,720);
    mgpu::game_work::tag(command.Get(), 99, 1, 53, 3840, 2160);
    mgpu::game_work::close(command.Get());
    if (FAILED(command->Close())) return 5;
    const unsigned job_count = argc > 1 ? 512 : 1;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> commands;
    std::vector<ID3D12CommandList *> lists{command.Get()};
    for (unsigned index = 1; index < job_count; ++index)
    {
        ComPtr<ID3D12CommandAllocator> next_allocator;
        ComPtr<ID3D12GraphicsCommandList> next_command;
        if (FAILED(device->CreateCommandAllocator(description.Type,IID_PPV_ARGS(&next_allocator))) ||
            FAILED(device->CreateCommandList(0,description.Type,next_allocator.Get(),nullptr,IID_PPV_ARGS(&next_command)))) return 11;
        mgpu::game_work::marker(next_command.Get(),mgpu::game_work::phase::target_change,1,2560,1440);
        for (unsigned operation = 0; operation < 200; ++operation)
        {
            mgpu::game_work::marker(next_command.Get(),mgpu::game_work::phase::graphics,operation+1);
            mgpu::game_work::marker(next_command.Get(),mgpu::game_work::phase::graphics);
        }
        mgpu::game_work::close(next_command.Get());
        if (FAILED(next_command->Close())) return 12;
        lists.push_back(next_command.Get());
        commands.push_back(next_command);
        allocators.push_back(next_allocator);
    }
    mgpu::game_work::before_submit(queue.Get(), job_count, lists.data());
    queue->ExecuteCommandLists(job_count, lists.data());
    mgpu::game_work::after_submit(queue.Get(), job_count, lists.data());
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || FAILED(queue->Signal(fence.Get(),1))) return 6;
    HANDLE completed = CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!completed || FAILED(fence->SetEventOnCompletion(1,completed)) || WaitForSingleObject(completed,5000) != WAIT_OBJECT_0) return 7;
    CloseHandle(completed);
    Sleep(800);
    for (unsigned iteration=0;iteration<job_count/8+2;++iteration)
    {
        const auto first=lines.size();
        mgpu::game_work::poll(0);
        unsigned emitted=0;
        for (auto index=first;index<lines.size();++index) emitted+=lines[index].find("kind=job")!=std::string::npos;
        if (emitted>8) return 13;
    }
    unsigned jobs=0;
    bool target=false, aggregate=false, feature=false, rays=false, prefix=false;
    for (const auto &line:lines)
    {
        if (line.find("kind=aggregate") != std::string::npos) aggregate=true;
        if (line.find("kind=job") == std::string::npos) continue;
        ++jobs;
        if (line.find("start_ms=-1") != std::string::npos || line.find("overflow=0") == std::string::npos) return 8;
        if (line.find("1:2560:1440:") != std::string::npos) target=true;
        if (line.find("ngx_handle=257 ngx_feature=1 ngx_known=1") != std::string::npos) feature=true;
        if (line.find("4:1280:720:") != std::string::npos) rays=true;
        if (line.find("data=0:") != std::string::npos) prefix=true;
    }
    if (jobs != job_count || !target || !aggregate || !feature || !rays || !prefix || mgpu::game_work::active()) return 9;
    if (argc > 2)
        for (const auto &line:lines) std::puts(line.c_str());
    mgpu::game_work::before_submit(queue.Get(), 1, lists.data());
    if (lines.back().find("command_replayed_queries_invalid") == std::string::npos) return 10;
    mgpu::game_work::poll(456);
    if (mgpu::game_work::active() || lines.back().find("process_restart_required") == std::string::npos) return 14;
    std::puts("PASS: WARP actual timestamp recording, queue fence retirement, target metadata and replay diagnosis");
    return 0;
}
