#pragma once
#include <d3d12.h>

namespace mgpu::game_work
{
enum class phase : unsigned { unclassified, graphics, compute, indirect, raytrace,
    copy, target_change, capture_tag, sl_sr_begin, sl_sr_end, ngx_begin, ngx_end, tail };
enum class unclassified_callback : unsigned { acceleration_build=0,uav_uint,uav_float,bundle };
void bind_device(ID3D12Device *device);
void feature_created(UINT64 handle, unsigned feature);
void feature_released(UINT64 handle);
void poll(UINT64 session, bool continuous = false);
void reset(ID3D12GraphicsCommandList *command);
void destroy(ID3D12GraphicsCommandList *command);
void observe_native_list(ID3D12GraphicsCommandList *command);
void close(ID3D12GraphicsCommandList *command);
void marker(ID3D12GraphicsCommandList *command, phase category, UINT64 pipeline = 0,
            unsigned width = 0, unsigned height = 0);
void callback_marker(ID3D12GraphicsCommandList *command,unclassified_callback kind);
void tag(ID3D12GraphicsCommandList *command, unsigned frame, unsigned viewport,
         unsigned type, unsigned width, unsigned height);
void before_submit(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists);
void after_submit(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists);
bool active();
}
