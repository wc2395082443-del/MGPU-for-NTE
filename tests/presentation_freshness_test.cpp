#include "../src/neural_pipeline.hpp"
#include "../experimental/secondary_fg/frame_contract.hpp"
#include <cstdio>

int main()
{
    using namespace mgpu::secondary_fg;
    mgpu::neural_pipeline::schedule pipeline;
    for (unsigned frame = 1; frame <= 3; ++frame)
    {
        mgpu::neural_pipeline::job job{};
        job.seal = job.fence = job.engine_frame = frame;
        job.snapshot = job.input_index = pipeline.write_slot();
        if (!pipeline.begin(job) || !pipeline.retire(frame)) return 1;
    }
    if (pipeline.has_capacity() || pipeline.ready_count() != 3 || pipeline.newest_ready_job().seal != 3) return 2;
    if (!pipeline.discard_oldest_ready() || pipeline.ready_job().seal != 2 || !pipeline.has_capacity()) return 3;
    if (!pipeline.discard_oldest_ready() || pipeline.discard_oldest_ready() || pipeline.ready_job().seal != 3) return 4;
    if (!pipeline.publish_ready() || pipeline.display_seal() != 3 || pipeline.write_slot() == pipeline.display_slot()) return 5;
    if (pipeline.scheduled_seal() != 3) return 6;

    decision generate{};
    generate.evaluate = generate.allow_generated = true;
    generate.frame_interval_ns = 30000000;
    presentation_pair pair;
    if (!pair.enqueue(5, 100000000, generate, true, false, 6)) return 7;
    if (pair.next(100000000) != picture::interpolated || !pair.presented(picture::interpolated, true, 100000000)) return 8;
    if (pair.finish_with_real(101000000) != 4 || pair.next(101000000) != picture::real) return 9;
    if (pair.finish_with_real(101000001) != 0 || pair.dropped_generated() != 4) return 10;
    if (!pair.presented(picture::real, true, 101000001)) return 11;
    if (!pair.enqueue(6, 102000000, generate, true, false, 6) || pair.next(102000000) != picture::interpolated) return 12;
    if (pair.finish_with_real(103000000) != 5 || pair.next(103000000) != picture::real) return 13;
    std::puts("PASS: only completed ready outputs coalesce; displayed/pending slots stay protected; stale generated tail yields to real NR, no raw fallback");
    return 0;
}
