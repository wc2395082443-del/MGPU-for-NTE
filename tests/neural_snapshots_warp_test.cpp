#include "../src/neural_snapshots.hpp"
#include "../src/neural_difference.hpp"
#include <dxgi1_4.h>
#include <cstdio>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace
{
void require(HRESULT status) { if (FAILED(status)) throw status; }
void transition(ID3D12GraphicsCommandList *command, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    command->ResourceBarrier(1, &barrier);
}
void submit(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command, ID3D12Fence *fence, UINT64 value)
{
    require(command->Close());
    ID3D12CommandList *commands[] = {command};
    queue->ExecuteCommandLists(1, commands);
    require(queue->Signal(fence, value));
}
void wait(ID3D12Fence *fence, UINT64 value)
{
    const auto deadline = GetTickCount64() + 5000;
    while (fence->GetCompletedValue() < value && GetTickCount64() < deadline) Sleep(1);
    if (fence->GetCompletedValue() == UINT64_MAX || fence->GetCompletedValue() < value) throw E_FAIL;
}
struct unblock_on_exit
{
    ComPtr<ID3D12Fence> fence;
    ~unblock_on_exit() { if (fence != nullptr) fence->Signal(1); }
};
void check_difference(mgpu::neural_pipeline::difference_audit &audit, double expected)
{
    std::array<mgpu::neural_pipeline::difference_audit::difference, 3> result;
    if (!audit.collect(result)) throw E_FAIL;
    for (const auto &entry : result)
        if (std::abs(entry.mean - expected) > 0.002 || entry.changed_fraction != 1.0) throw E_FAIL;
}
}

int main()
{
    try
    {
        ComPtr<IDXGIFactory4> factory;
        require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> adapter;
        require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        DXGI_ADAPTER_DESC adapter_desc{};
        require(adapter->GetDesc(&adapter_desc));
        std::printf("Software-only adapter: %ls\n", adapter_desc.Description);
        ComPtr<ID3D12Device> device;
        require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        require(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
        constexpr auto slot_count = mgpu::neural_pipeline::schedule::SLOTS;
        std::array<ComPtr<ID3D12CommandAllocator>, slot_count> allocators;
        std::array<ComPtr<ID3D12GraphicsCommandList>, slot_count> commands;
        for (unsigned index = 0; index < slot_count; ++index)
        {
            require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[index])));
            require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[index].Get(),
                nullptr, IID_PPV_ARGS(&commands[index])));
        }
        ComPtr<ID3D12Fence> fence;
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = description.Height = 8;
        description.DepthOrArraySize = description.MipLevels = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> input, output;
        require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&input)));
        require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&output)));
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 2;
        ComPtr<ID3D12DescriptorHeap> views;
        require(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&views)));
        auto input_view = views->GetCPUDescriptorHandleForHeapStart();
        auto output_view = input_view;
        output_view.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(input.Get(), nullptr, input_view);
        device->CreateRenderTargetView(output.Get(), nullptr, output_view);
        mgpu::neural_pipeline::snapshots snapshots;
        std::array<mgpu::neural_pipeline::difference_audit, slot_count> audits;
        mgpu::neural_pipeline::schedule pipeline;
        require(snapshots.initialize(output.Get()));
        unblock_on_exit gate;
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)));
        require(queue->Wait(gate.fence.Get(), 1));
        for (unsigned epoch = 1; epoch <= slot_count; ++epoch)
        {
            auto *command = commands[epoch - 1].Get();
            if (epoch > 1)
            {
                transition(command, input.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
                transition(command, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            const float before[4] = {0.1f, 0.2f, 0.3f, 0.0f};
            const float delta = 0.2f * epoch;
            const float after[4] = {0.1f + delta, 0.2f + delta, 0.3f + delta, 1.0f};
            command->ClearRenderTargetView(input_view, before, 0, nullptr);
            command->ClearRenderTargetView(output_view, after, 0, nullptr);
            transition(command, input.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            transition(command, output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            snapshots.record(command, epoch - 1, input.Get(), D3D12_RESOURCE_STATE_COMMON, output.Get());
            require(audits[epoch - 1].record(command, snapshots.input(epoch - 1), snapshots.output(epoch - 1)));
            mgpu::neural_pipeline::job next;
            next.seal = next.fence = epoch;
            next.input_index = next.snapshot = epoch - 1;
            if (!pipeline.begin(next)) throw E_FAIL;
            submit(queue.Get(), command, fence.Get(), epoch);
        }
        if (fence->GetCompletedValue() != 0 || pipeline.has_capacity() || pipeline.publish(0)) throw E_FAIL;
        require(gate.fence->Signal(1));
        wait(fence.Get(), slot_count);
        for (unsigned index = 0; index < slot_count; ++index)
        {
            if (!pipeline.retire(fence->GetCompletedValue()) || pipeline.displayed()) throw E_FAIL;
            check_difference(audits[index], 0.2 * (index + 1));
        }
        if (pipeline.pending() || pipeline.ready_count() != slot_count || pipeline.has_capacity()) throw E_FAIL;
        for (unsigned index = 0; index < slot_count; ++index)
            if (pipeline.ready_job().seal != index + 1 || !pipeline.publish_ready() || pipeline.display_slot() != index)
                throw E_FAIL;

        unblock_on_exit display;
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&display.fence)));
        snapshots.presented(0, display.fence.Get(), 1);
        if (snapshots.reads_complete()) throw E_FAIL;
        require(snapshots.wait_for_read(queue.Get(), 0));
        auto *command = commands[0].Get();
        require(allocators[0]->Reset());
        require(command->Reset(allocators[0].Get(), nullptr));
        snapshots.record(command, 0, input.Get(), D3D12_RESOURCE_STATE_COMMON, output.Get());
        require(audits[0].record(command, snapshots.input(0), snapshots.output(0)));
        submit(queue.Get(), command, fence.Get(), slot_count + 1);
        Sleep(20);
        if (fence->GetCompletedValue() != slot_count) throw E_FAIL;
        require(display.fence->Signal(1));
        wait(fence.Get(), slot_count + 1);
        check_difference(audits[0], 0.2 * slot_count);
        if (!snapshots.reads_complete()) throw E_FAIL;
        snapshots.release();
        for (auto &audit : audits) audit.release();
        std::puts("PASS: three GPU-completed slots retain distinct readbacks before publication; display fence gates reuse; RGB difference ignores alpha");
        return 0;
    }
    catch (HRESULT status)
    {
        std::printf("FAIL: 0x%08lx\n", static_cast<unsigned long>(status));
        return 1;
    }
}
