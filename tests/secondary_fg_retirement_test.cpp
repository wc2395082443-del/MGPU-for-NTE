#include "../src/neural_pipeline.hpp"
#ifndef MGPU_SOURCE_GATE_HEADER
#define MGPU_SOURCE_GATE_HEADER "../experimental/secondary_fg/source_gate.hpp"
#endif
#include MGPU_SOURCE_GATE_HEADER
#include <cstdio>
#include <cstdlib>
#include <thread>

static unsigned checks = 0;
static void require(bool valid, const char *name)
{
    ++checks;
    if (valid) return;
    std::fprintf(stderr, "FAIL: %s\n", name);
    std::exit(1);
}

int main()
{
    using namespace mgpu::secondary_fg;
    using mgpu::neural_pipeline::schedule;
    using mgpu::neural_pipeline::job;
    schedule pipeline;
    std::array<completed_output, schedule::SLOTS> outputs{};
    const auto submit = [&](unsigned frame)
    {
        job next{};
        next.seal = next.fence = next.engine_frame = frame;
        next.snapshot = next.input_index = pipeline.write_slot();
        return pipeline.begin(next);
    };
    require(submit(1) && pipeline.retire(1), "first GPU completion retires independently");
    require(!pipeline.pending() && pipeline.ready() && !pipeline.displayed(), "retired is not displayed");
    require(pipeline.publish_ready() && pipeline.display_seal() == 1, "publish first result");
    presentation_pair shown;
    decision interpolation{};
    interpolation.evaluate = interpolation.allow_generated = true;
    interpolation.frame_interval_ns = 30000000;
    require(shown.enqueue(1, 100000000, interpolation, true, false, 6), "hold displayed sixfold group");
    require(submit(2), "second GPU job starts while first displays");
    const auto second = pipeline.current();
    outputs[second.snapshot].seal = second.seal;
    outputs[second.snapshot].generation = interpolation;
    outputs[second.snapshot].generated_valid = true;
    require(!pipeline.retire(1) && pipeline.retire(2), "only actual GPU completion permits retirement");
    require(pipeline.display_seal() == 1 && pipeline.ready_count() == 1 && !pipeline.pending(),
        "history can advance without replacing display");
    require(submit(3), "third job starts before old presentation group ends");
    const auto third = pipeline.current();
    outputs[third.snapshot].seal = third.seal;
    outputs[third.snapshot].generated_valid = false;
    require(pipeline.retire(3) && pipeline.ready_count() == 2, "two completed groups retain distinct ownership");
    require(!pipeline.has_capacity() && !submit(4), "display plus ready results use exactly three slots");
    require(outputs[second.snapshot].generated_valid && !outputs[third.snapshot].generated_valid,
        "new invalid FG result cannot invalidate older generated output");
    require(shown.next(125000000) == picture::real && shown.presented(picture::real, true, 125000000),
        "old display group ends independently");
    require(pipeline.ready_job().seal == 2 && pipeline.publish_ready() && pipeline.display_seal() == 2,
        "ready results publish in source order");
    require(submit(4), "retired display slot can accept work after read-fence handoff");
    require(!pipeline.publish(UINT64_MAX) && pipeline.display_seal() == 2,
        "removed-device sentinel cannot publish pending or ready work");

    require(!source_needs_wait(0, known_frame | 100), "unknown source never blocks game");
    require(!source_needs_wait(known_frame | 100, 0), "unknown completion never blocks startup");
    require(!source_needs_wait(known_frame | 101, known_frame | 100), "one-frame lead permits overlap");
    require(source_needs_wait(known_frame | 102, known_frame | 100), "two-frame lead prevents next source frame");
    require(!source_needs_wait(known_frame | 99, known_frame | 100), "stale source never wraps into a huge wait");
    require(source_needs_wait(known_frame | 1, known_frame | 0xffffffffu), "frame wrap keeps bounded distance");

    source_gate gate;
    require(gate.initialize(), "process-lifetime wake event initialized");
    gate.complete(100);
    gate.presented(100);
    gate.enable();
    std::thread advance([&] { Sleep(10); gate.complete(102); gate.presented(101); });
    const auto waited = gate.wait(known_frame | 102);
    advance.join();
    require(waited < 100 && gate.waits() == 1 && gate.timeouts() == 0,
        "GPU progress wakes source without a mutex or GPU command wait");
    gate.complete(200);
    require(!source_needs_wait(known_frame | 103, gate.progress()) && gate.presented() == (known_frame | 101),
        "GPU retirement releases source independently of a delayed display");
    std::thread cancel([&] { Sleep(10); gate.disable(); });
    gate.wait(known_frame | 203);
    cancel.join();
    require(!gate.enabled() && gate.timeouts() == 0, "shutdown cancellation releases game promptly");
    gate.reset();
    gate.complete(100);
    gate.presented(100);
    gate.enable();
    for (unsigned iteration = 0; iteration < 3; ++iteration) gate.wait(known_frame | 102);
    require(gate.timeouts() == 3 && gate.stalled() && !gate.enabled(), "three no-progress timeouts disable pacing, not NR/FG");
    gate.enable();
    require(!gate.enabled(), "worker cannot silently re-enable a stalled gate");
    require(gate.wait(known_frame | 102) == 0, "disabled gate cannot freeze further Presents");
    gate.reset();
    gate.complete(100);
    gate.presented(100);
    gate.enable();
    for (unsigned iteration = 0; iteration < 3; ++iteration)
    {
        std::thread partial([&] { Sleep(10); gate.complete(101 + iteration); gate.presented(101 + iteration); });
        gate.wait(known_frame | (103 + iteration));
        partial.join();
    }
    require(gate.timeouts() == 6 && gate.stalled() && !gate.enabled(),
        "three expired waits disable pacing even if each made partial progress; no 114-timeout loop");

    schedule long_run;
    std::uint64_t source_frame = 0, next_source_ms = 0, source_done_ms = 0;
    std::uint64_t gpu_done_ms = 0, display_done_ms = 0, displayed = 0, retired = 0;
    std::uint64_t max_age_ms = 0, max_lead = 0, max_gpu_lead = 0;
    bool source_pending = false, gpu_pending = false, displaying = false;
    std::array<std::uint64_t, schedule::SLOTS> captures{};
    for (std::uint64_t now = 1; now < 120000 && displayed < 1000; ++now)
    {
        if (displaying && now >= display_done_ms)
        {
            displayed = long_run.display_seal();
            displaying = false;
            max_age_ms = (std::max)(max_age_ms, now - captures[long_run.display_slot()]);
        }
        if (gpu_pending && now >= gpu_done_ms)
        {
            const auto frame = long_run.current().seal;
            require(frame == retired + 1 && long_run.retire(frame), "simulation retains every source/history frame");
            retired = frame;
            gpu_pending = false;
        }
        if (!displaying && long_run.ready())
        {
            require(long_run.publish_ready(), "simulation publishes ordered completed group");
            displaying = true;
            display_done_ms = now + 27;
        }
        if (!source_pending && now >= next_source_ms &&
            !source_needs_wait(known_frame | source_frame, known_frame | retired))
        {
            ++source_frame;
            source_done_ms = now + 10;
            source_pending = true;
        }
        if (source_pending && now >= source_done_ms && !gpu_pending && long_run.has_capacity())
        {
            job next{};
            next.seal = next.fence = next.engine_frame = source_frame;
            next.snapshot = next.input_index = long_run.write_slot();
            require(long_run.begin(next), "simulation does not reuse ready/displayed slot");
            captures[next.snapshot] = source_done_ms;
            gpu_done_ms = now + 23;
            gpu_pending = true;
            source_pending = false;
            next_source_ms = now + 1;
        }
        max_lead = (std::max)(max_lead, source_frame - displayed);
        max_gpu_lead = (std::max)(max_gpu_lead, source_frame - retired);
    }
    require(displayed == 1000 && retired >= displayed, "thousand-frame faster-producer simulation completes");
    require(max_gpu_lead <= 2 && max_lead <= schedule::SLOTS + 2 && max_age_ms < 150,
        "simulation bounds GPU lead and ready/display ownership without dropped source frames");
    std::printf("Simulation only: 1000 real frames, max GPU lead=%llu, display lead=%llu, max age=%llums; not hardware timing\n",
        max_gpu_lead, max_lead, max_age_ms);
    std::printf("PASS: %u ordered-retirement, per-slot validity, source-lead, cancellation and timeout checks\n", checks);
}
