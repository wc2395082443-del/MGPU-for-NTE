#include "queue_dependency_trace.hpp"

namespace mgpu::queue_trace
{
void observe_queue(ID3D12CommandQueue *) {}
void observe_fence(ID3D12Fence *) {}
void alias(ID3D12Fence *, ID3D12Fence *) {}
void job(ID3D12CommandQueue *, ID3D12GraphicsCommandList *, UINT64, UINT64) {}
void poll(UINT64, bool) {}
bool active() { return false; }
const wchar_t *export_path() { return L""; }
}
