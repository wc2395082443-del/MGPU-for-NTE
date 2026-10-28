#pragma once

#include "depth_plane_policy.hpp"
#include "gpu_timestamp_span.hpp"
#include <wrl/client.h>
#include <d3dcompiler.h>

namespace mgpu::aligned
{
struct depth_job
{
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command;
    UINT64 value = 0;
    std::uint64_t serial = 0;
    timing::gpu_timestamp_span timing;
};

HRESULT compile_depth_shader(ID3DBlob **shader);

class depth_normalizer
{
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    UINT64 next_ = 0;
    timing::queue_clock_cache clock_;
    HRESULT initialize(ID3D12Device *device);

public:
    ID3D12Fence *fence() const { return fence_.Get(); }
    bool complete(const depth_job &job) const;
    HRESULT submit(const plane &source, const tagged_resource &region, plane &output,
                   depth_job &job, std::uint64_t serial,
                   ID3D12Fence *gate = nullptr, UINT64 gate_value = 0);
};
}
