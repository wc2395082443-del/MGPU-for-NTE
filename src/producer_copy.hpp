#pragma once

#include "gpu_timestamp_span.hpp"
#include "queue_dependency_trace.hpp"
#include <array>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace mgpu::producer_copy
{
struct range
{
    ID3D12Resource *source = nullptr;
    UINT64 source_offset = 0, target_offset = 0, bytes = 0;
};

struct dependency
{
    ID3D12Fence *fence = nullptr;
    UINT64 value = 0;
};

struct transfer_sample
{
    UINT64 retirement = 0;
    double submitted_ms = -1;
    timing::span execution{};
    timing::span color_execution{}, guides_execution{};
    timing::span depth_execution{}, motion_execution{};
};

class transfer_queue
{
    struct submission
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> color_allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> color_command;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> depth_allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> depth_command;
        UINT64 value = 0;
        bool measured = false;
        bool split = false;
        bool split_guides = false;
        timing::calibration clock{};
        double submitted_ms = -1;
    };
    std::array<submission, 4> slots_{};
    std::array<transfer_sample, 16> history_{};
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries_;
    Microsoft::WRL::ComPtr<ID3D12Resource> timestamps_;
    UINT64 next_ = 0, frequency_ = 0, measured_ = 0;
    double gpu_ms_ = 0;
    timing::metric queue_wait_{};
    timing::queue_clock_cache clock_;
    bool failed_ = false;

public:
    HRESULT initialize(ID3D12Device *device)
    {
        if (queue_ != nullptr) return S_OK;
        if (device == nullptr) return E_INVALIDARG;
        D3D12_COMMAND_QUEUE_DESC description{};
        description.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        HRESULT result = device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue));
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(result)) return result;
        device_ = device;
        queue_ = queue;
        fence_ = fence;
        mgpu::queue_trace::observe_queue(queue_.Get());
        mgpu::queue_trace::observe_fence(fence_.Get());

        D3D12_FEATURE_DATA_D3D12_OPTIONS3 support{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &support, sizeof support)) &&
            support.CopyQueueTimestampQueriesSupported && SUCCEEDED(queue_->GetTimestampFrequency(&frequency_)) && frequency_ != 0)
        {
            D3D12_QUERY_HEAP_DESC query_desc{};
            query_desc.Type = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
            query_desc.Count = unsigned(slots_.size()) * 6;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = query_desc.Count * sizeof(UINT64);
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries_))) ||
                FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timestamps_))))
            {
                queries_.Reset();
                timestamps_.Reset();
            }
        }
        return S_OK;
    }

    ID3D12CommandQueue *queue() const { return queue_.Get(); }
    ID3D12Fence *fence() const { return fence_.Get(); }
    UINT64 measured() const { return measured_; }
    double gpu_ms() const { return gpu_ms_; }
    const timing::metric &queue_wait() const { return queue_wait_; }

    bool pending() const
    {
        if (failed_) return true;
        if (next_ == 0) return false;
        const auto completed = fence_->GetCompletedValue();
        return completed == UINT64_MAX || completed < next_;
    }

    void collect()
    {
        if (timestamps_ == nullptr) return;
        const auto completed = fence_->GetCompletedValue();
        if (completed == UINT64_MAX) return;
        for (unsigned index = 0; index < slots_.size(); ++index)
        {
            auto &slot = slots_[index];
            if (slot.value == 0 || slot.value > completed || slot.measured) continue;
            void *mapped = nullptr;
            const SIZE_T offset = index * 6 * sizeof(UINT64);
            const D3D12_RANGE read{offset, offset + 6 * sizeof(UINT64)};
            if (SUCCEEDED(timestamps_->Map(0, &read, &mapped)) && mapped != nullptr)
            {
                const auto *values = reinterpret_cast<const UINT64 *>(static_cast<const char *>(mapped) + offset);
                const unsigned last = slot.split_guides ? 5 : slot.split ? 3 : 1;
                bool ordered = true;
                for (unsigned point = 1; point <= last; ++point)
                    ordered = ordered && values[point] >= values[point - 1];
                if (ordered)
                {
                    UINT64 ticks = 0;
                    for (unsigned point = 0; point < last; point += 2)
                        ticks += values[point + 1] - values[point];
                    gpu_ms_ += double(ticks) * 1000.0 / double(frequency_);
                    ++measured_;
                    const auto sample = timing::decode(values[0], values[last], slot.clock);
                    auto &entry = history_[slot.value % history_.size()];
                    entry = {slot.value, slot.submitted_ms, sample};
                    if (slot.split)
                    {
                        entry.color_execution = timing::decode(values[0], values[1], slot.clock);
                        entry.guides_execution = timing::decode(values[2], values[last], slot.clock);
                    }
                    if (slot.split_guides)
                    {
                        entry.depth_execution = timing::decode(values[2], values[3], slot.clock);
                        entry.motion_execution = timing::decode(values[4], values[5], slot.clock);
                    }
                    queue_wait_.add(timing::interval(slot.submitted_ms, sample.start_ms));
                }
                const D3D12_RANGE written{0, 0};
                timestamps_->Unmap(0, &written);
            }
            slot.measured = true;
        }
    }

    transfer_sample sample(UINT64 retirement)
    {
        collect();
        const auto &found = history_[retirement % history_.size()];
        return retirement != 0 && found.retirement == retirement ? found : transfer_sample{};
    }

    HRESULT submit(const std::array<range, 4> &ranges, unsigned count,
                   ID3D12Resource *target, ID3D12Fence *arrival, UINT64 arrival_value,
                   UINT64 &retirement, const std::array<dependency, 3> &dependencies = {},
                   ID3D12Fence *color_arrival = nullptr, ID3D12Fence *depth_arrival = nullptr)
    {
        retirement = 0;
        if (failed_ || queue_ == nullptr || count == 0 || count > ranges.size()) return E_FAIL;
        if (target == nullptr || arrival == nullptr || arrival_value == 0 || arrival_value == UINT64_MAX) return E_INVALIDARG;
        if (color_arrival != nullptr && (count != 4 || color_arrival == arrival || color_arrival == fence_.Get())) return E_INVALIDARG;
        if (depth_arrival != nullptr && (color_arrival == nullptr || depth_arrival == color_arrival ||
            depth_arrival == arrival || depth_arrival == fence_.Get())) return E_INVALIDARG;
        for (const auto &gate : dependencies)
            if ((gate.fence == nullptr) != (gate.value == 0) || gate.value == UINT64_MAX)
                return E_INVALIDARG;
        const auto destination = target->GetDesc();
        if (destination.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return E_INVALIDARG;
        for (unsigned range_index = 0; range_index != count; ++range_index)
        {
            const auto &copy = ranges[range_index];
            if (copy.source == nullptr || copy.bytes == 0) return E_INVALIDARG;
            const auto source = copy.source->GetDesc();
            if (source.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || copy.source == target ||
                copy.source_offset > source.Width || copy.bytes > source.Width - copy.source_offset ||
                copy.target_offset > destination.Width || copy.bytes > destination.Width - copy.target_offset) return E_INVALIDARG;
        }
        if (color_arrival != nullptr)
            for (unsigned plane_index = 1; plane_index < (depth_arrival != nullptr ? 3u : 2u); ++plane_index)
                for (unsigned range_index = 0; range_index < count; ++range_index)
                    if (range_index != plane_index && ranges[plane_index].target_offset < ranges[range_index].target_offset + ranges[range_index].bytes &&
                        ranges[range_index].target_offset < ranges[plane_index].target_offset + ranges[plane_index].bytes) return E_INVALIDARG;
        const auto completed = fence_->GetCompletedValue();
        if (completed == UINT64_MAX) { failed_ = true; return DXGI_ERROR_DEVICE_REMOVED; }
        collect();
        const unsigned index = unsigned(next_ % slots_.size());
        auto &slot = slots_[index];
        if (slot.value > completed) return DXGI_ERROR_WAS_STILL_DRAWING;
        HRESULT result = S_OK;
        if (slot.allocator == nullptr)
            result = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&slot.allocator));
        if (SUCCEEDED(result)) result = slot.allocator->Reset();
        if (SUCCEEDED(result))
            result = slot.command == nullptr
                ? device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.command))
                : slot.command->Reset(slot.allocator.Get(), nullptr);
        if (FAILED(result)) return result;
        slot.split = color_arrival != nullptr;
        slot.split_guides = depth_arrival != nullptr;
        if (slot.split)
        {
            if (slot.color_allocator == nullptr)
                result = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&slot.color_allocator));
            if (SUCCEEDED(result)) result = slot.color_allocator->Reset();
            if (SUCCEEDED(result)) result = slot.color_command == nullptr
                ? device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, slot.color_allocator.Get(), nullptr, IID_PPV_ARGS(&slot.color_command))
                : slot.color_command->Reset(slot.color_allocator.Get(), nullptr);
            if (FAILED(result)) return result;
            if (queries_ != nullptr) slot.color_command->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 6);
            const auto &color = ranges[1];
            slot.color_command->CopyBufferRegion(target, color.target_offset, color.source, color.source_offset, color.bytes);
            if (queries_ != nullptr)
            {
                slot.color_command->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 6 + 1);
                slot.color_command->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 6, 2,
                    timestamps_.Get(), index * 6 * sizeof(UINT64));
            }
            result = slot.color_command->Close();
            if (FAILED(result)) return result;
        }
        if (slot.split_guides)
        {
            if (slot.depth_allocator == nullptr)
                result = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&slot.depth_allocator));
            if (SUCCEEDED(result)) result = slot.depth_allocator->Reset();
            if (SUCCEEDED(result)) result = slot.depth_command == nullptr
                ? device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, slot.depth_allocator.Get(), nullptr, IID_PPV_ARGS(&slot.depth_command))
                : slot.depth_command->Reset(slot.depth_allocator.Get(), nullptr);
            if (FAILED(result)) return result;
            if (queries_ != nullptr) slot.depth_command->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 6 + 2);
            const auto &depth = ranges[2];
            slot.depth_command->CopyBufferRegion(target, depth.target_offset, depth.source, depth.source_offset, depth.bytes);
            if (queries_ != nullptr)
            {
                slot.depth_command->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 6 + 3);
                slot.depth_command->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 6 + 2, 2,
                    timestamps_.Get(), (index * 6 + 2) * sizeof(UINT64));
            }
            result = slot.depth_command->Close();
            if (FAILED(result)) return result;
        }
        const unsigned first_query = index * 6 + (slot.split_guides ? 4 : slot.split ? 2 : 0);
        if (queries_ != nullptr) slot.command->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first_query);
        // Dependency fences retire each owned DEFAULT buffer in COMMON before COPY promotion.
        for (unsigned range_index = 0; range_index != count; ++range_index)
        {
            if (slot.split && range_index == 1) continue;
            if (slot.split_guides && range_index == 2) continue;
            const auto &copy = ranges[range_index];
            slot.command->CopyBufferRegion(target, copy.target_offset, copy.source, copy.source_offset, copy.bytes);
        }
        if (queries_ != nullptr)
        {
            slot.command->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first_query + 1);
            slot.command->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first_query, 2,
                timestamps_.Get(), first_query * sizeof(UINT64));
        }
        result = slot.command->Close();
        if (FAILED(result)) return result;
        slot.value = UINT64_MAX;
        slot.measured = false;
        slot.submitted_ms = timing::cpu_ms(timing::cpu_ticks());
        if (slot.split)
        {
            const auto &color_gate = dependencies[0];
            if (color_gate.fence != nullptr) result = queue_->Wait(color_gate.fence, color_gate.value);
            if (FAILED(result)) { failed_ = true; return result; }
            ID3D12CommandList *color_commands[] = {slot.color_command.Get()};
            queue_->ExecuteCommandLists(1, color_commands);
            result = queue_->Signal(color_arrival, arrival_value);
            if (FAILED(result)) { failed_ = true; return result; }
        }
        if (slot.split_guides)
        {
            const auto &depth_gate = dependencies[1];
            if (depth_gate.fence != nullptr) result = queue_->Wait(depth_gate.fence, depth_gate.value);
            if (FAILED(result)) { failed_ = true; return result; }
            ID3D12CommandList *depth_commands[] = {slot.depth_command.Get()};
            queue_->ExecuteCommandLists(1, depth_commands);
            result = queue_->Signal(depth_arrival, arrival_value);
            if (FAILED(result)) { failed_ = true; return result; }
        }
        for (unsigned gate_index = slot.split_guides ? 2 : slot.split ? 1 : 0; gate_index < dependencies.size(); ++gate_index)
        {
            const auto &gate = dependencies[gate_index];
            if (gate.fence == nullptr) continue;
            result = queue_->Wait(gate.fence, gate.value);
            if (FAILED(result)) { failed_ = true; return result; }
        }
        ID3D12CommandList *commands[] = {slot.command.Get()};
        queue_->ExecuteCommandLists(1, commands);
        result = queue_->Signal(fence_.Get(), ++next_);
        if (SUCCEEDED(result)) result = queue_->Signal(arrival, arrival_value);
        if (FAILED(result)) { failed_ = true; return result; }
        slot.value = next_;
        retirement = next_;
        if (queries_ != nullptr) slot.clock = clock_.sample(queue_.Get(), timing::cpu_ticks());
        return S_OK;
    }

    HRESULT submit(const std::array<range, 4> &ranges, ID3D12Resource *target,
                   ID3D12Fence *arrival, UINT64 arrival_value, UINT64 &retirement)
    {
        return submit(ranges, static_cast<unsigned>(ranges.size()), target, arrival,
                      arrival_value, retirement);
    }
};
}
