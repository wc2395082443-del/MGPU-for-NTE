#include "fg_gpu_audit.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace audit = mgpu::official_fg::gpu_audit;

void check(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

int main()
{
    try {
        ComPtr<IDXGIFactory4> factory;
        check(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))), "factory");
        ComPtr<IDXGIAdapter> warp;
        check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP");
        ComPtr<ID3D12Device> device;
        check(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))), "device");
        audit::attach_device(device.Get());
        D3D12_COMMAND_QUEUE_DESC description{};
        ComPtr<ID3D12CommandQueue> queue;
        check(SUCCEEDED(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue))), "queue");
        ComPtr<ID3D12CommandAllocator> allocator;
        check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "allocator");
        ComPtr<ID3D12GraphicsCommandList> commands;
        check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands))), "commands");
        check(audit::begin(commands.Get(), 500, 5, 1, 7) == 0, "recorded without trigger");
        audit::start_test_capture();
        audit::present(77, 500);
        const auto ticket = audit::begin(commands.Get(), 500, 5, 1, 7);
        check(ticket == 1, "first ticket");
        audit::end(ticket, commands.Get(), 1);
        audit::finish_present();
        audit::poll();
        check(audit::completed_records() == 0, "read before submission");
        check(SUCCEEDED(commands->Close()), "close");
        ComPtr<ID3D12Fence> gate;
        check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "gate");
        check(SUCCEEDED(queue->Wait(gate.Get(), 1)), "gate wait");
        ID3D12CommandList *lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        audit::poll();
        check(audit::completed_records() == 0, "read before actual GPU completion");
        check(SUCCEEDED(gate->Signal(1)), "release gate");
        const auto deadline = GetTickCount64() + 5000;
        while (!audit::completed_records() && GetTickCount64() < deadline) { audit::poll(); Sleep(1); }
        check(audit::completed_records() == 1, "no fence-validated timestamp readback");
        audit::stop();
        std::puts("PASS: capture disabled by default; original queue submits without CPU wait; GPU-gated timestamps read only after completion");
        return 0;
    } catch (const std::exception &error) {
        std::printf("FAIL: %s\n", error.what()); return 1;
    }
}
