#pragma once

#include "pipeline_timing.hpp"
#include <d3d12.h>
#include <wrl/client.h>

namespace mgpu::timing
{
inline UINT64 cpu_frequency()
{
    static const UINT64 frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) ? UINT64(value.QuadPart) : 0;
    }();
    return frequency;
}

inline UINT64 cpu_ticks()
{
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) ? UINT64(value.QuadPart) : 0;
}

inline double cpu_ms(UINT64 ticks)
{
    return ticks != 0 && cpu_frequency() != 0 ? double(ticks) * 1000.0 / double(cpu_frequency()) : -1;
}

inline calibration calibrate(ID3D12CommandQueue *queue)
{
    calibration clock{};
    clock.cpu_frequency = cpu_frequency();
    if (queue == nullptr || FAILED(queue->GetTimestampFrequency(&clock.gpu_frequency))) return {};
    clock.query_before = cpu_ticks();
    const auto result = queue->GetClockCalibration(&clock.gpu, &clock.cpu);
    clock.query_after = cpu_ticks();
    if (FAILED(result))
    {
        clock.gpu = clock.cpu = 0;
    }
    return clock;
}

class queue_clock_cache
{
    ID3D12CommandQueue *queue_ = nullptr;
    UINT64 sampled_at_ = 0;
    calibration clock_{};

public:
    calibration sample(ID3D12CommandQueue *queue, UINT64 ticks)
    {
        if (queue != queue_ || sampled_at_ == 0 || ticks < sampled_at_ ||
            ticks - sampled_at_ >= cpu_frequency())
        {
            clock_ = calibrate(queue);
            queue_ = queue;
            sampled_at_ = ticks;
        }
        return clock_;
    }
};

class gpu_timestamp_span
{
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    bool attempted_ = false, recorded_ = false;
    UINT64 serial_ = 0;
    calibration clock_{};
    double recorded_ms_ = -1, submitted_ms_ = -1;

    bool initialize(ID3D12Device *device)
    {
        if (attempted_) return queries_ != nullptr && readback_ != nullptr;
        attempted_ = true;
        if (device == nullptr) return false;
        D3D12_QUERY_HEAP_DESC queries{};
        queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queries.Count = 2;
        if (FAILED(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&queries_)))) return false;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 2 * sizeof(UINT64);
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)))) return false;
        return true;
    }

public:
    void begin(ID3D12Device *device, ID3D12GraphicsCommandList *command, UINT64 serial)
    {
        serial_ = serial;
        recorded_ = false;
        clock_ = {};
        recorded_ms_ = cpu_ms(cpu_ticks());
        submitted_ms_ = -1;
        if (command == nullptr || serial == 0 ||
            (command->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT &&
             command->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE) || !initialize(device)) return;
        command->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        recorded_ = true;
    }

    void end(ID3D12GraphicsCommandList *command)
    {
        if (!recorded_) return;
        command->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        command->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback_.Get(), 0);
    }

    void submitted(ID3D12CommandQueue *queue, UINT64 ticks, queue_clock_cache *cache = nullptr)
    {
        if (!recorded_) return;
        submitted_ms_ = cpu_ms(ticks);
        clock_ = cache != nullptr ? cache->sample(queue, ticks) : calibrate(queue);
    }

    span read(UINT64 serial, bool gpu_completed) const
    {
        if (!gpu_completed || !recorded_ || serial != serial_ || readback_ == nullptr) return {};
        void *mapped = nullptr;
        const D3D12_RANGE range{0, 2 * sizeof(UINT64)};
        if (FAILED(readback_->Map(0, &range, &mapped)) || mapped == nullptr) return {};
        const auto *values = static_cast<const UINT64 *>(mapped);
        const auto result = decode(values[0], values[1], clock_);
        const D3D12_RANGE written{0, 0};
        readback_->Unmap(0, &written);
        return result;
    }

    double recorded_ms() const { return recorded_ms_; }
    double submitted_ms() const { return submitted_ms_; }
};
}
