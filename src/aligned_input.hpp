#pragma once

#include "aligned_frame_policy.hpp"
#include "motion_frame_packet.hpp"
#include <d3d12.h>
#include <wrl/client.h>

namespace mgpu::aligned
{
struct tagged_resource
{
    unsigned long long native = 0;
    unsigned state = UINT_MAX;
    int lifecycle = -1;
    unsigned top = 0, left = 0, width = 0, height = 0;
};
struct plane
{
    ID3D12Resource *buffer = nullptr;
    D3D12_RESOURCE_DESC description{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
};
struct lease
{
    unsigned slot = UINT_MAX;
    std::uint64_t serial = 0;
    std::uint64_t measurement_session = 0;
    frame_key key{};
    guide_constants constants{};
    std::array<plane, 3> planes{};
    lease() = default;
    lease(const lease &) = delete;
    lease &operator=(const lease &) = delete;
    ~lease();
    explicit operator bool() const { return serial != 0; }
};

void enable();
bool enabled();
void observe_queue(ID3D12CommandQueue *queue);
void set_game_device(ID3D12Device *device);
void reset_command(ID3D12GraphicsCommandList *command);
void capture(void *command, unsigned buffer_type, const tagged_resource &resource,
             motion_packet::identity engine);
void capture_constants(const guide_constants &constants);
void set_ready_dispatch(bool enabled);
HANDLE dispatch_event();
std::uint64_t submitted_color_frame();
bool acquire(lease &result);
void commit_transfer(lease &source);
HRESULT submit_transfer(lease &source, ID3D12Resource *seal_upload, UINT64 seal_source,
    ID3D12Resource *target, UINT64 seal_target, UINT64 seal_bytes, const std::array<UINT64, 3> &offsets,
    ID3D12Fence *arrival, UINT64 arrival_value, ID3D12Fence *color_arrival = nullptr,
    ID3D12Fence *depth_arrival = nullptr);
bool outstanding();
bool matches(const plane &input, unsigned width, unsigned height, DXGI_FORMAT format,
             const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint, UINT64 bytes);
DXGI_FORMAT depth_format(DXGI_FORMAT format);
void stop();
}
