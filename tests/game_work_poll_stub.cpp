#include "../src/game_work_trace.hpp"
namespace mgpu::game_work
{
void poll(UINT64, bool) {}
void bind_device(ID3D12Device *) {}
void tag(ID3D12GraphicsCommandList *, unsigned, unsigned, unsigned, unsigned, unsigned) {}
void marker(ID3D12GraphicsCommandList *, phase, UINT64, unsigned, unsigned) {}
void before_submit(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *) {}
void after_submit(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *) {}
}
