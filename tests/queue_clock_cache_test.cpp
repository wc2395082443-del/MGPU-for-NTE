#include "../src/gpu_timestamp_span.hpp"
#include <array>
#include <cstdio>

namespace
{
unsigned frequency_calls = 0, calibration_calls = 0;
HRESULT STDMETHODCALLTYPE frequency(ID3D12CommandQueue *, UINT64 *value)
{
    ++frequency_calls;
    *value = 1000;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE calibration(ID3D12CommandQueue *, UINT64 *gpu, UINT64 *cpu)
{
    ++calibration_calls;
    *gpu = 10000;
    *cpu = mgpu::timing::cpu_ticks();
    return S_OK;
}
struct fake_queue { void **methods; };
}

int main()
{
    std::array<void *, 19> methods{};
    methods[16] = reinterpret_cast<void *>(&frequency);
    methods[17] = reinterpret_cast<void *>(&calibration);
    fake_queue first{methods.data()}, second{methods.data()};
    auto *queue = reinterpret_cast<ID3D12CommandQueue *>(&first);
    mgpu::timing::queue_clock_cache cache;
    for (UINT64 stamp = 1; stamp <= 120; ++stamp)
        if (cache.sample(queue, stamp).gpu_frequency != 1000) return 1;
    if (frequency_calls != 1 || calibration_calls != 1) return 2;
    const auto tick = mgpu::timing::cpu_frequency() + 1;
    cache.sample(queue, tick);
    if (frequency_calls != 2 || calibration_calls != 2) return 3;
    cache.sample(reinterpret_cast<ID3D12CommandQueue *>(&second), tick);
    if (frequency_calls != 3 || calibration_calls != 3) return 4;
    cache.sample(queue, 1);
    if (frequency_calls != 4 || calibration_calls != 4) return 5;
    std::puts("PASS: 120 timestamps reuse one calibration; elapsed interval and queue changes refresh it");
    return 0;
}
