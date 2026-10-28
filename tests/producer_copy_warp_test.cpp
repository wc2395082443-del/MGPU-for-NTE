#include "../src/producer_copy.hpp"
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace
{
void require(HRESULT status) { if (FAILED(status)) throw status; }
void wait(ID3D12Fence *fence, UINT64 value)
{
    const auto deadline = GetTickCount64() + 5000;
    while (fence->GetCompletedValue() < value && GetTickCount64() < deadline) Sleep(1);
    if (fence->GetCompletedValue() == UINT64_MAX || fence->GetCompletedValue() < value) throw E_FAIL;
}
ComPtr<ID3D12Resource> buffer(ID3D12Device *device, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = 2048;
    description.Height = description.DepthOrArraySize = description.MipLevels = description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
        type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12Resource> result;
    require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr, IID_PPV_ARGS(&result)));
    return result;
}
struct release_gate
{
    ID3D12Fence *fence;
    ~release_gate() { fence->Signal(1); }
};
}

int main()
{
    try
    {
        ComPtr<IDXGIFactory4> factory;
        require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> adapter;
        require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        ComPtr<ID3D12Device> device;
        require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        mgpu::producer_copy::transfer_queue transfer;
        require(transfer.initialize(device.Get()));
        if (transfer.queue()->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_COPY) throw E_FAIL;
        ComPtr<ID3D12CommandQueue> renderer;
        D3D12_COMMAND_QUEUE_DESC description{};
        require(device->CreateCommandQueue(&description, IID_PPV_ARGS(&renderer)));
        ComPtr<ID3D12CommandAllocator> allocator;
        require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> command;
        require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&command)));
        ComPtr<ID3D12Fence> rendered, arrival, gate;
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rendered)));
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&arrival)));
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD);
        auto source = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT);
        auto target = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT);
        auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK);
        std::array<unsigned char, 2048> pattern{};
        for (unsigned index = 0; index < pattern.size(); ++index)
            pattern[index] = static_cast<unsigned char>(index * 7 + (index / 256) * 13 + 19);
        void *mapped = nullptr;
        const D3D12_RANGE empty{0, 0};
        require(upload->Map(0, &empty, &mapped));
        std::memcpy(mapped, pattern.data(), pattern.size());
        upload->Unmap(0, nullptr);
        command->CopyBufferRegion(source.Get(), 0, upload.Get(), 0, pattern.size());
        require(command->Close());
        ID3D12CommandList *commands[] = {command.Get()};
        renderer->ExecuteCommandLists(1, commands);
        require(renderer->Signal(rendered.Get(), 1));
        wait(rendered.Get(), 1);

        const std::array<mgpu::producer_copy::range, 4> copies{{
            {upload.Get(), 0, 0, 64},
            {source.Get(), 128, 128, 256},
            {source.Get(), 512, 512, 256},
            {source.Get(), 1024, 1024, 256},
        }};
        const std::array<mgpu::producer_copy::dependency, 3> gates{{{gate.Get(), 1}}};
        release_gate unblock{gate.Get()};
        UINT64 retirement = 0;
        for (unsigned index = 1; index <= 4; ++index)
        {
            require(transfer.submit(copies, 4, target.Get(), arrival.Get(), index, retirement, gates));
            if (retirement != index || !transfer.pending() || arrival->GetCompletedValue() != 0) throw E_FAIL;
        }
        if (transfer.submit(copies, target.Get(), arrival.Get(), 5, retirement) != DXGI_ERROR_WAS_STILL_DRAWING) throw E_FAIL;
        require(renderer->Signal(rendered.Get(), 2));
        wait(rendered.Get(), 2);
        if (arrival->GetCompletedValue() != 0) throw E_FAIL;
        require(gate->Signal(1));
        wait(arrival.Get(), 4);
        if (transfer.pending()) throw E_FAIL;
        transfer.collect();
        require(transfer.submit(copies, target.Get(), arrival.Get(), 5, retirement));
        wait(arrival.Get(), 5);
        require(allocator->Reset());
        require(command->Reset(allocator.Get(), nullptr));
        command->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, 2048);
        require(command->Close());
        renderer->ExecuteCommandLists(1, commands);
        require(renderer->Signal(rendered.Get(), 3));
        wait(rendered.Get(), 3);
        const D3D12_RANGE read{0, 2048};
        require(readback->Map(0, &read, &mapped));
        for (const auto &copy : copies)
            if (std::memcmp(static_cast<const char *>(mapped) + copy.target_offset,
                            pattern.data() + copy.source_offset, static_cast<size_t>(copy.bytes)) != 0) throw E_FAIL;
        readback->Unmap(0, &empty);
        auto invalid = copies;
        invalid[1].bytes = 4096;
        if (transfer.submit(invalid, target.Get(), arrival.Get(), 6, retirement) != E_INVALIDARG) throw E_FAIL;
        invalid = copies;
        invalid[2].source_offset = UINT64_MAX;
        if (transfer.submit(invalid, target.Get(), arrival.Get(), 6, retirement) != E_INVALIDARG) throw E_FAIL;
        invalid = copies;
        invalid[3].source = nullptr;
        if (transfer.submit(invalid, target.Get(), arrival.Get(), 6, retirement) != E_INVALIDARG) throw E_FAIL;
        invalid = copies;
        invalid[0].source = target.Get();
        if (transfer.submit(invalid, target.Get(), arrival.Get(), 6, retirement) != E_INVALIDARG) throw E_FAIL;
        auto invalid_gates = gates;
        invalid_gates[0] = {nullptr, 1};
        if (transfer.submit(copies, 4, target.Get(), arrival.Get(), 6, retirement, invalid_gates) != E_INVALIDARG) throw E_FAIL;
        invalid_gates[0] = {gate.Get(), 0};
        if (transfer.submit(copies, 4, target.Get(), arrival.Get(), 6, retirement, invalid_gates) != E_INVALIDARG) throw E_FAIL;
        invalid_gates[0] = {gate.Get(), UINT64_MAX};
        if (transfer.submit(copies, 4, target.Get(), arrival.Get(), 6, retirement, invalid_gates) != E_INVALIDARG) throw E_FAIL;
        require(transfer.submit(copies, target.Get(), arrival.Get(), 6, retirement));
        wait(arrival.Get(), 6);
        if (retirement != 6 || transfer.pending()) throw E_FAIL;
        std::puts("PASS: WARP-only dedicated transfer preserves all four ranges; arrival follows copy; rendering progresses while copy waits; allocators cannot recycle pending work");
        return 0;
    }
    catch (HRESULT status)
    {
        std::printf("FAIL: 0x%08lx\n", static_cast<unsigned long>(status));
        return 1;
    }
}
