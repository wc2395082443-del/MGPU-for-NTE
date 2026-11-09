#include "../src/neural_pipeline.hpp"
#include <cstdio>

int main()
{
    using namespace mgpu::neural_pipeline;
    unsigned failures = 0;
    const auto check = [&](bool condition, const char *name) {
        std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) ++failures;
    };
    check(parallel_passes_supported(1, false) && parallel_passes_supported(1, true),
        "single-pass eligibility remains unchanged");
    check(parallel_passes_supported(2, true), "official dual pass keeps the owned-slot async consumer path");
    check(!parallel_passes_supported(2, false), "legacy secondary FG dual-pass path remains excluded");
    check(!parallel_passes_supported(0, true) && !parallel_passes_supported(3, true),
        "unreviewed pass counts remain excluded");
    schedule pipeline;
    mgpu::neural_history::history history;
    for (unsigned frame = 1; frame <= 3; ++frame) {
        job next;
        next.seal = frame;
        next.fence = frame * 2;
        next.engine_frame = frame + 100;
        next.input_index = next.snapshot = pipeline.write_slot();
        next.settings.active_passes = 2;
        next.settings.strengths = {2.0f, 2.0f};
        next.bound_strengths = next.settings.strengths;
        next.bound_strength_mask = 3;
        if (pipeline.pending()) history.complete(pipeline.latest().settings);
        next.settings_change = history.changes(next.settings);
        check(next.snapshot < schedule::SLOTS && pipeline.begin(next),
            "each queued dual-pass frame owns a distinct input/output/pong slot");
        check(pipeline.latest().settings.active_passes == 2 && pipeline.latest().bound_strength_mask == 3,
            "queued job preserves both pass strengths and completion metadata");
        if (frame > 1) check(next.settings_change == mgpu::neural_history::none,
            "unchanged queued dual-pass settings do not reset each frame");
    }
    check(pipeline.inflight() == 3 && !pipeline.has_capacity(), "dual-pass queue stays bounded by existing slots");
    check(!pipeline.retire(1) && pipeline.inflight() == 3,
        "a fence before frame completion cannot retire a frame that still needs pass two");
    check(pipeline.retire(2) && pipeline.current().engine_frame == 102,
        "first full dual-pass frame retires without waiting for later source frames");
    check(pipeline.retire(4) && pipeline.retire(6) && !pipeline.pending(),
        "both-pass frame completion remains in source order");
    schedule transition;
    job single;
    single.seal = 1;
    single.fence = 10;
    single.engine_frame = 100;
    single.input_index = single.snapshot = transition.write_slot();
    single.settings.active_passes = 1;
    single.settings.strengths = {2.0f, 2.0f};
    check(transition.begin(single), "live pass change starts with an in-flight single-pass frame");
    auto dual = single;
    dual.seal = 2;
    dual.fence = 11;
    dual.engine_frame = 101;
    dual.input_index = dual.snapshot = transition.write_slot();
    dual.settings.active_passes = 2;
    history.complete(single.settings);
    check(history.changes(dual.settings) == mgpu::neural_history::passes && transition.begin(dual),
        "one-to-two pass transition records a history change on the first affected source frame");
    check(transition.current().settings.active_passes == 1 && transition.latest().settings.active_passes == 2,
        "queued pass change cannot mutate the older job's pass count");
    check(transition.retire(10) && transition.current().settings.active_passes == 2 && transition.retire(11),
        "single and dual-pass jobs drain in order with their own completion fences");
    return failures == 0 ? 0 : 1;
}
