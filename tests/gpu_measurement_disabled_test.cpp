#include "../src/gpu_timestamp_span.hpp"
#include "../src/neural_snapshots.hpp"
#include "../src/neural_pipeline.hpp"
#include <cstdio>

int main()
{
    static_assert(!mgpu::gpu_measurement::timestamps);
    static_assert(!mgpu::gpu_measurement::diagnostic_pixels);
    unsigned failures = 0;
    const auto check = [&](bool condition, const char *name) {
        std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) ++failures;
    };
    const auto first = mgpu::timing::cpu_ticks();
    const auto second = mgpu::timing::cpu_ticks();
    check(first != 0 && second >= first && mgpu::timing::cpu_frequency() != 0,
        "real CPU source clocks are preserved rather than globally zeroed");
    check(mgpu::timing::calibrate(reinterpret_cast<ID3D12CommandQueue *>(1)).gpu_frequency == 0,
        "disabled GPU calibration cannot call into a driver queue");
    mgpu::timing::gpu_timestamp_span span;
    span.begin(reinterpret_cast<ID3D12Device *>(1), reinterpret_cast<ID3D12GraphicsCommandList *>(1), 7);
    span.end(reinterpret_cast<ID3D12GraphicsCommandList *>(1));
    span.submitted(reinterpret_cast<ID3D12CommandQueue *>(1), first);
    check(span.read(7, true).gpu_ms < 0 && span.recorded_ms() > 0,
        "capture creates no query heap or GPU marks while preserving source CPU time");
    mgpu::neural_pipeline::schedule pipeline;
    mgpu::neural_pipeline::job job;
    job.seal = 1;
    job.fence = 1;
    job.engine_frame = 100;
    job.input_index = job.snapshot = pipeline.write_slot();
    job.settings.active_passes = 2;
    check(pipeline.begin(job) && !pipeline.retire(0) && pipeline.retire(1),
        "real resource completion remains required without GPU time samples");
    check(mgpu::neural_pipeline::parallel_passes_supported(2, true),
        "official double-pass parallel eligibility is unchanged");
    return failures ? 1 : 0;
}
