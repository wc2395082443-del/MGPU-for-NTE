#include "../src/depth_normalizer.hpp"
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace mgpu::aligned;

namespace
{
void require(HRESULT status)
{
    if (FAILED(status)) throw status;
}

ComPtr<ID3D12Resource> buffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = description.DepthOrArraySize = description.MipLevels = description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&result)));
    return result;
}

void barrier(ID3D12GraphicsCommandList *command, ID3D12Resource *resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER transition{};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    command->ResourceBarrier(1, &transition);
}

void finish(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command, ID3D12Fence *fence, UINT64 value)
{
    require(command->Close());
    ID3D12CommandList *commands[] = {command};
    queue->ExecuteCommandLists(1, commands);
    require(queue->Signal(fence, value));
    const auto deadline = GetTickCount64() + 5000;
    while (fence->GetCompletedValue() < value && GetTickCount64() < deadline) Sleep(1);
    if (fence->GetCompletedValue() == UINT64_MAX || fence->GetCompletedValue() < value) throw E_FAIL;
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
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        ComPtr<ID3D12CommandQueue> queue;
        require(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator;
        require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> command;
        require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&command)));
        ComPtr<ID3D12Fence> fence;
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        plane source{};
        source.description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        source.description.Width = source.description.Height = 8;
        source.description.DepthOrArraySize = source.description.MipLevels = source.description.SampleDesc.Count = 1;
        source.description.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        source.description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        ComPtr<ID3D12Resource> texture;
        require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &source.description,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&texture)));
        D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
        descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        descriptors.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> views;
        require(device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&views)));
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        const auto handle = views->GetCPUDescriptorHandleForHeapStart();
        device->CreateDepthStencilView(texture.Get(), &view, handle);
        for (LONG row = 0; row < 8; ++row)
            for (LONG column = 0; column < 8; ++column)
            {
                D3D12_RECT rectangle{column, row, column + 1, row + 1};
                command->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    float(row * 8 + column) / 64.0f, 0xa5, 1, &rectangle);
            }
        device->GetCopyableFootprints(&source.description, 0, 1, 0, &source.footprint, nullptr, nullptr, &source.bytes);
        std::printf("Native depth footprint: format=%u row_pitch=%u bytes=%llu\n",
                    unsigned(source.footprint.Footprint.Format), source.footprint.Footprint.RowPitch, source.bytes);
        auto staging = buffer(device.Get(), source.bytes, D3D12_HEAP_TYPE_DEFAULT);
        source.buffer = staging.Get();
        barrier(command.Get(), texture.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(command.Get(), staging.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = texture.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = staging.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = source.footprint;
        command->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier(command.Get(), staging.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        finish(queue.Get(), command.Get(), fence.Get(), 1);
        depth_normalizer normalizer;
        depth_job job;
        plane output{};
        tagged_resource region{};
        region.left = 1;
        region.top = 2;
        region.width = 5;
        region.height = 3;
        for (UINT run = 1; run <= 2; ++run)
        {
            region.left = run;
            require(normalizer.submit(source, region, output, job, run));
            const auto deadline = GetTickCount64() + 5000;
            while (!normalizer.complete(job) && GetTickCount64() < deadline) Sleep(1);
            if (!normalizer.complete(job)) throw E_FAIL;
            const auto measured = job.timing.read(run, true);
            if (measured.gpu_ms < 0 || job.timing.read(run + 1, true).gpu_ms != -1 ||
                job.timing.read(run, false).gpu_ms != -1) throw E_FAIL;
            std::printf("Depth timestamp: serial=%u GPU=%.6f ms calibrated=%u\n", run,
                        measured.gpu_ms, unsigned(measured.start_ms >= 0));
            auto readback = buffer(device.Get(), output.bytes, D3D12_HEAP_TYPE_READBACK);
            require(allocator->Reset());
            require(command->Reset(allocator.Get(), nullptr));
            barrier(command.Get(), output.buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            command->CopyBufferRegion(readback.Get(), 0, output.buffer, 0, output.bytes);
            barrier(command.Get(), output.buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            finish(queue.Get(), command.Get(), fence.Get(), run + 1);
            void *mapped = nullptr;
            D3D12_RANGE range{0, static_cast<SIZE_T>(output.bytes)};
            require(readback->Map(0, &range, &mapped));
            bool equal = true;
            for (UINT row = 0; row < region.height; ++row)
                for (UINT column = 0; column < region.width; ++column)
                {
                    const float expected = float((row + region.top) * 8 + column + region.left) / 64.0f;
                    const auto *actual = static_cast<const char *>(mapped) + row * output.footprint.Footprint.RowPitch + column * 4;
                    equal = equal && std::memcmp(actual, &expected, sizeof expected) == 0;
                }
            D3D12_RANGE written{};
            readback->Unmap(0, &written);
            if (!equal) throw E_FAIL;
        }
        output.buffer->Release();
        std::puts("PASS: two native depth/stencil captures cropped to bit-exact R32_FLOAT; allocator and buffer reuse passed");
        return 0;
    }
    catch (HRESULT status)
    {
        std::printf("FAIL: 0x%08lx\n", static_cast<unsigned long>(status));
        return 1;
    }
}
