#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include "shared_frame_contract.hpp"

namespace mgpu::official_fg
{
struct texture_binding
{
    ID3D12Resource *texture = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

class shared_frame_transport
{
public:
    shared_frame_transport() = default;
    shared_frame_transport(const shared_frame_transport &) = delete;
    shared_frame_transport &operator=(const shared_frame_transport &) = delete;
    ~shared_frame_transport();
    HRESULT create(ID3D12Device *device, const std::array<plane_description, shared_planes> &descriptions,
                   std::uint64_t generation);
    HRESULT open(ID3D12Device *device, const shared_manifest &manifest);
    const shared_manifest &manifest() const { return manifest_; }
    std::vector<HANDLE> inheritable_handles() const;
    ID3D12Resource *texture(unsigned slot, unsigned plane) const;
    ID3D12Fence *ready_fence() const { return ready_.Get(); }
    ID3D12Fence *released_fence() const { return released_.Get(); }
    HRESULT record_write(ID3D12GraphicsCommandList *commands, unsigned slot,
        const std::array<texture_binding, shared_planes> &sources) const;
    HRESULT record_read(ID3D12GraphicsCommandList *commands, unsigned slot,
        const std::array<texture_binding, shared_planes> &destinations) const;
    HRESULT signal_ready(ID3D12CommandQueue *queue, std::uint64_t sequence) const;
    HRESULT wait_ready(ID3D12CommandQueue *queue, std::uint64_t sequence) const;
    HRESULT signal_released(ID3D12CommandQueue *queue, std::uint64_t sequence) const;
    bool same_device(ID3D12DeviceChild *object) const;
    static D3D12_RESOURCE_DESC texture_description(const plane_description &description);
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    std::array<std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, shared_planes>, shared_slots> textures_{};
    Microsoft::WRL::ComPtr<ID3D12Fence> ready_, released_;
    shared_manifest manifest_{};
    std::vector<HANDLE> owned_handles_;
    bool producer_ = false;
    HRESULT export_handle(ID3D12DeviceChild *object, std::uint64_t &output);
    HRESULT record_copy(ID3D12GraphicsCommandList *commands, unsigned slot,
        const std::array<texture_binding, shared_planes> &bindings, bool writing) const;
};
}
