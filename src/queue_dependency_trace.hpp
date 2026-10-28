#pragma once
#include <d3d12.h>

namespace mgpu::queue_trace
{
enum class kind : unsigned { object = 1, alias, execute, command, queue_wait,
    queue_signal, completed, set_event, cpu_signal, job };
struct record
{
    UINT64 sequence = 0, parent = 0, begin = 0, end = 0, object = 0, other = 0;
    UINT64 value = 0, extra = 0, raw = 0, auxiliary = 0;
    unsigned operation = 0, thread = 0;
    LONG result = 0;
    unsigned flags = 0;
};
struct header
{
    UINT64 magic = 0x314543415254514dull, version = 1, record_bytes = sizeof(record);
    UINT64 session = 0, frequency = 0, begin = 0, end = 0, count = 0;
    UINT64 overflow = 0, failures = 0, metadata = 0, pending = 0;
};
static_assert(sizeof(record) == 96 && sizeof(header) == 96, "trace ABI");
void observe_queue(ID3D12CommandQueue *queue);
void observe_fence(ID3D12Fence *fence);
void alias(ID3D12Fence *first, ID3D12Fence *second);
void job(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command, UINT64 session, UINT64 job_id);
void poll(UINT64 session, bool continuous = false);
bool active();
const wchar_t *export_path();
}
