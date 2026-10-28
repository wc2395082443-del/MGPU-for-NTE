#include "../src/neural_pipeline.hpp"
#include <cstdio>

int main()
{
    using namespace mgpu::neural_pipeline;
    schedule pipeline;
    unsigned failures = 0;
    const auto check = [&](bool passed, const char *name)
    {
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    check(!pipeline.displayed() && pipeline.check(0) == completion::idle, "no uninitialized display");

    job first{1, 10, 1, 0, 0, 0, 100, true};
    job second{2, 11, 2, 0, 1, 1, 101, true};
    job third{3, 12, 3, 0, 2, 2, 102, true};
    check(pipeline.begin(first), "first submission reserves slot zero");
    const unsigned prefetched_slot = pipeline.write_slot();
    check(!pipeline.reserve(prefetched_slot, 3), "prefetch cannot skip the next input identity");
    check(pipeline.reserve(prefetched_slot, 2), "next free slot can be reserved for copy prefetch");
    check(pipeline.write_slot() == prefetched_slot, "prefetch reservation pins the next pipeline slot");
    auto invalid = second;
    invalid.input_index = first.input_index;
    check(!pipeline.begin(invalid), "pending input cannot be overwritten by another job");
    invalid = second;
    invalid.fence = first.fence;
    check(!pipeline.begin(invalid), "repeated fence cannot acknowledge a new submission");
    invalid.fence = first.fence - 1;
    check(!pipeline.begin(invalid), "fences cannot move backward");
    invalid = second;
    invalid.input_index = 2;
    check(!pipeline.begin(invalid), "input and snapshot must have the same owner");
    auto prefetched = second;
    prefetched.input_index = prefetched.snapshot = prefetched_slot;
    check(pipeline.begin(prefetched), "prefetched input can be adopted by the next neural job");
    check(pipeline.write_slot() != prefetched_slot, "adopted prefetch slot is no longer free");
    auto third_after_prefetch = third;
    third_after_prefetch.input_index = third_after_prefetch.snapshot = pipeline.write_slot();
    check(pipeline.begin(third_after_prefetch), "third submission can overlap the prefetched job");
    check(pipeline.inflight() == 3 && !pipeline.has_capacity(), "three jobs remain bounded");
    job fourth{4, 13, 4, 0, 0, 0, 103, true};
    check(!pipeline.begin(fourth), "fourth submission waits for an ordered retirement");
    check(pipeline.current().seal == 1, "oldest seal is selected first");
    check(pipeline.latest().engine_frame == 102, "temporal predecessor is the most recently submitted job");
    check(!pipeline.all_complete(11) && pipeline.all_complete(12), "teardown checks the last queued fence");
    check(!pipeline.all_complete(UINT64_MAX), "device removal never authorizes teardown");
    check(!pipeline.publish(9) && pipeline.current().seal == 1, "partial completion cannot publish");
    check(pipeline.check(UINT64_MAX) == completion::failed, "removed device is not completion");
    check(pipeline.pending() && !pipeline.publish(UINT64_MAX), "failed submission keeps ownership");
    check(pipeline.publish(10) && pipeline.display_seal() == 1 && pipeline.display_slot() == 0,
          "first completed job becomes visible");
    check(pipeline.inflight() == 2 && pipeline.write_slot() == 3,
          "display and two in-flight slots block every snapshot slot");
    check(!pipeline.has_capacity(), "a display slot is not spare capacity");
    check(pipeline.publish(11) && pipeline.display_seal() == 2 && pipeline.display_slot() == 1,
          "second job publishes only after the first");
    check(pipeline.write_slot() == 0, "retired display slot becomes reusable");
    check(pipeline.publish(12) && pipeline.display_seal() == 3 && pipeline.display_slot() == 2,
          "third job publishes in seal order");
    check(pipeline.inflight() == 0 && pipeline.scheduled_seal() == 3,
          "FIFO drains without losing the schedule cursor");
    check(!pipeline.begin(first), "retirement does not make an old frame admissible");
    for (unsigned frame = 4; frame < 1000; ++frame)
    {
        job next;
        next.seal = frame;
        next.fence = frame + 9;
        next.input_index = next.snapshot = pipeline.write_slot();
        if (!pipeline.begin(next) || !pipeline.publish(next.fence) || pipeline.display_seal() != frame)
        {
            check(false, "FIFO remains ordered across repeated wraparound");
            break;
        }
    }
    check(pipeline.display_seal() == 999 && !pipeline.pending(), "FIFO reuses retired slots over 996 frames");

    schedule reserved_pipeline;
    check(reserved_pipeline.begin(first) && reserved_pipeline.begin(second) && reserved_pipeline.publish(10),
          "reservation regression starts with displayed and pending slots");
    check(reserved_pipeline.reserve(2, 3), "prefetch reserves the remaining slot");
    check(reserved_pipeline.publish(11) && reserved_pipeline.write_slot() == 2,
          "retiring a lower slot must not redirect an in-flight prefetch");
    auto wrong_seal = third;
    wrong_seal.seal = 4;
    check(!reserved_pipeline.begin(wrong_seal), "prefetched pixels cannot be adopted under a different seal");
    check(reserved_pipeline.begin(third) && reserved_pipeline.reserved_seal() == 0,
          "matching neural submission consumes the reservation");
    check(!reserved_pipeline.reserve(2, 4), "in-flight neural input cannot be reserved");
    check(reserved_pipeline.reserve(0, 4), "new reservation uses a retired slot");
    reserved_pipeline.cancel_reservation();
    check(reserved_pipeline.reserved_seal() == 0 && reserved_pipeline.reserve(0, 4),
          "unsubmitted copy failure can release and retry its reservation");

    schedule history_pipeline;
    mgpu::neural_history::history completed_history;
    first.settings.tuning_enabled = true;
    first.settings.skin = 2;
    first.settings_change = completed_history.changes(first.settings);
    check(history_pipeline.begin(first), "first temporal frame is submitted");
    auto submission_history = completed_history;
    submission_history.complete(history_pipeline.latest().settings);
    second.settings = first.settings;
    check(submission_history.changes(second.settings) == mgpu::neural_history::none,
          "unchanged queued settings do not repeatedly reset history");
    second.settings.structure = 2;
    second.settings_change = submission_history.changes(second.settings);
    check(second.settings_change == mgpu::neural_history::tuning && history_pipeline.begin(second),
          "queued structure edit resets exactly its first affected frame");
    submission_history = completed_history;
    submission_history.complete(history_pipeline.latest().settings);
    check(submission_history.changes(second.settings) == mgpu::neural_history::none &&
          completed_history.revision() == 0,
          "next queued frame inherits the edit without acknowledging GPU completion");
    check(history_pipeline.latest().engine_frame + 1 == third.engine_frame,
          "continuous queued frames have no artificial history gap");
    return failures == 0 ? 0 : 1;
}
