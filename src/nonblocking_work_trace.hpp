#pragma once
#include "game_work_trace.hpp"

namespace mgpu::game_work::nonblocking
{
bool engaged();
bool active();
void poll(UINT64 session);
void bind_device(ID3D12Device *device);
void observe(ID3D12GraphicsCommandList *command);
void reset(ID3D12GraphicsCommandList *command);
void destroy(ID3D12GraphicsCommandList *command);
void close(ID3D12GraphicsCommandList *command);
void marker(ID3D12GraphicsCommandList *command, phase category, UINT64 pipeline, unsigned width, unsigned height);
void tag(ID3D12GraphicsCommandList *command, unsigned frame, unsigned viewport, unsigned type, unsigned width, unsigned height);
void callback_marker(ID3D12GraphicsCommandList *command, unclassified_callback category);
void before_submit(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists);
void after_submit(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists);
bool drained();
bool finished();
struct counters
{
    UINT64 created = 0, emitted = 0, cancelled = 0, rejected = 0, invalid = 0, replayed = 0, fine_refused = 0, truncated = 0;
    unsigned live = 0, peak_live = 0, coarse_backed = 0, fine_backed = 0;
    UINT64 growth_failures = 0, drain_timeouts = 0;
};
counters snapshot();
}
