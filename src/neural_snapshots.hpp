#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include "neural_pipeline.hpp"

namespace mgpu::neural_pipeline
{
class snapshots
{
public:
    static constexpr unsigned SLOTS = schedule::SLOTS;
    HRESULT initialize(ID3D12Resource *source)
    {
        if (source == nullptr) return E_INVALIDARG;
        auto description = source->GetDesc();
        if (pairs_[0].output != nullptr)
        {
            const auto existing = pairs_[0].output->GetDesc();
            return existing.Width == description.Width && existing.Height == description.Height &&
                existing.Format == description.Format ? S_OK : E_INVALIDARG;
        }
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        HRESULT result = source->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(result)) return result;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        std::array<pair, SLOTS> created{};
        for (auto &entry : created)
        {
            result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&entry.input));
            if (FAILED(result)) return result;
            result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&entry.output));
            if (FAILED(result)) return result;
        }
        pairs_ = std::move(created);
        return S_OK;
    }
    ID3D12Resource *input(unsigned index) const { return pairs_.at(index).input.Get(); }
    ID3D12Resource *output(unsigned index) const { return pairs_.at(index).output.Get(); }
    bool reads_complete() const
    {
        for (const auto &entry : pairs_)
            if (entry.read_value != 0 && (entry.read_fence == nullptr ||
                entry.read_fence->GetCompletedValue() == UINT64_MAX ||
                entry.read_fence->GetCompletedValue() < entry.read_value)) return false;
        return true;
    }
    HRESULT wait_for_read(ID3D12CommandQueue *queue, unsigned index) const
    {
        const auto &entry = pairs_.at(index);
        return entry.read_value == 0 ? S_OK : entry.read_fence != nullptr
            ? queue->Wait(entry.read_fence.Get(), entry.read_value) : E_FAIL;
    }
    void presented(unsigned index, ID3D12Fence *fence, UINT64 value)
    {
        auto &entry = pairs_.at(index);
        entry.read_fence = fence;
        entry.read_value = value;
    }
    void record(ID3D12GraphicsCommandList *command, unsigned index, ID3D12Resource *input,
                D3D12_RESOURCE_STATES input_state, ID3D12Resource *output)
    {
        auto &entry = pairs_.at(index);
        copy(command, input, input_state, entry.input.Get());
        copy(command, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, entry.output.Get());
    }
    void release() { pairs_ = {}; }
private:
    struct pair
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> input, output;
        Microsoft::WRL::ComPtr<ID3D12Fence> read_fence;
        UINT64 read_value = 0;
    };
    std::array<pair, SLOTS> pairs_{};
    static void copy(ID3D12GraphicsCommandList *command, ID3D12Resource *source,
                     D3D12_RESOURCE_STATES source_state, ID3D12Resource *destination)
    {
        D3D12_RESOURCE_BARRIER barriers[2]{};
        for (auto &barrier : barriers)
        {
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        barriers[0].Transition.pResource = source;
        barriers[0].Transition.StateBefore = source_state;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.pResource = destination;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        command->ResourceBarrier(2, barriers);
        command->CopyResource(destination, source);
        for (auto &barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        command->ResourceBarrier(2, barriers);
    }
};
}
