#include "frame_contract.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace mgpu::secondary_fg;

static unsigned checks = 0;

static void require(bool value, const char *name)
{
    ++checks;
    if (!value)
    {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}

static frame_contract valid_frame(std::uint64_t frame_id)
{
    frame_contract frame;
    frame.frame_id = frame_id;
    frame.depth_frame_id = frame_id;
    frame.motion_frame_id = frame_id;
    frame.camera_frame_id = frame_id;
    frame.capture_ns = frame_id * 16000000;
    frame.neural_output = true;
    frame.depth_valid = true;
    frame.motion_valid = true;
    frame.camera_valid = true;
    frame.camera_values_valid = true;
    frame.normalized_motion_scale_x = 1.0f / 1920;
    frame.normalized_motion_scale_y = 1.0f / 1080;
    return frame;
}

int main()
{
    require(supported_generated_frames(false, 5) == 1u, "unreported MFG is not sixfold support");
    require(supported_generated_frames(true, 0) == 1u, "zero maximum means base FG only");
    require(supported_generated_frames(true, 7) == 5u, "cap runtime limit to owned outputs");
    require(supports_multiplier(6, 5) && !supports_multiplier(6, 3), "6x requires five generated frames");
    require(!supports_multiplier(7, 5) && !supports_multiplier(1, 5), "reject invalid multiplier");
    require(validate(valid_frame(1)) == rejection::none, "valid source");
    auto invalid = valid_frame(1);
    invalid.source_frame_generation = true;
    require(validate(invalid) == rejection::source_frame_generation, "no upstream interpolation");
    invalid = valid_frame(1);
    invalid.neural_output = false;
    require(validate(invalid) == rejection::missing_neural_output, "FG after NR only");
    invalid = valid_frame(1);
    invalid.depth_valid = false;
    require(validate(invalid) == rejection::missing_guides, "depth required");
    invalid = valid_frame(1);
    invalid.motion_valid = false;
    require(validate(invalid) == rejection::missing_guides, "motion required");
    invalid = valid_frame(2);
    invalid.motion_frame_id = 1;
    require(validate(invalid) == rejection::mismatched_frame, "reject stale motion");
    invalid = valid_frame(2);
    invalid.depth_frame_id = 1;
    require(validate(invalid) == rejection::mismatched_frame, "reject stale depth");
    invalid = valid_frame(2);
    invalid.camera_frame_id = 1;
    require(validate(invalid) == rejection::mismatched_frame, "reject stale camera");
    auto camera_optional = valid_frame(1);
    camera_optional.camera_frame_id = 0;
    camera_optional.camera_valid = false;
    camera_optional.camera_values_valid = false;
    require(validate(camera_optional) == rejection::none, "camera metadata is optional");
    invalid = valid_frame(1);
    invalid.camera_valid = false;
    invalid.camera_frame_id = 0;
    require(validate(invalid) == rejection::none, "missing camera does not reject real guides");
    invalid = valid_frame(1);
    invalid.camera_values_valid = false;
    require(validate(invalid) == rejection::invalid_camera, "camera values required");
    invalid = valid_frame(1);
    invalid.normalized_motion_scale_x = std::numeric_limits<float>::quiet_NaN();
    require(validate(invalid) == rejection::invalid_motion_scale, "reject NaN motion units");
    invalid = valid_frame(1);
    invalid.normalized_motion_scale_y = 0;
    require(validate(invalid) == rejection::invalid_motion_scale, "reject unknown motion units");
    invalid = valid_frame(1);
    invalid.capture_ns = 0;
    require(validate(invalid) == rejection::invalid_timestamp, "timestamp required");

    frame_history history;
    auto first = history.inspect(valid_frame(1));
    require(first.evaluate && first.reset && !first.allow_generated, "warmup suppresses generated frame");
    history.complete(valid_frame(1), true);
    auto second = history.inspect(valid_frame(2));
    require(second.evaluate && !second.reset && second.allow_generated, "continuous frames interpolate");
    require(second.frame_interval_ns == 16000000, "frame interval timing");
    require(history.inspect(valid_frame(1)).reason == rejection::non_monotonic_frame, "no duplicate evaluation");
    auto reset = valid_frame(2);
    reset.reset = true;
    require(history.inspect(reset).reset && !history.inspect(reset).allow_generated, "scene reset");
    require(history.inspect(valid_frame(3)).reset, "dropped frame resets history");
    auto pause = valid_frame(2);
    pause.capture_ns = 1000000000;
    require(history.inspect(pause).reset, "long pause resets history");
    auto rapid = valid_frame(2);
    rapid.capture_ns = 16000001;
    require(history.inspect(rapid).reset, "unrealistic cadence resets history");
    history.complete(valid_frame(2), false);
    require(history.inspect(valid_frame(3)).reset, "SDK failure resets history");

    presentation_pair pair;
    require(!pair.enqueue(2, 100000000, second, false, false, 2), "GPU completion required");
    require(pair.enqueue(2, 100000000, second, true, false, 2), "queue generated-real pair");
    require(pair.next(100000000) == picture::interpolated, "interpolated before current real");
    require(!pair.enqueue(3, 101000000, second, true, false, 2), "no overwrite while pending");
    require(!pair.presented(picture::real, true), "reject out-of-order presentation");
    require(pair.presented(picture::interpolated, true), "generated presentation acknowledged");
    require(pair.next(107999999) == picture::none, "real frame not presented back-to-back");
    require(pair.next(108000000) == picture::real, "real frame due at half interval");
    require(pair.presented(picture::real, true) && !pair.pending(), "pair drained before next NR");
    require(pair.enqueue(3, 116000000, second, true, true, 2), "SDK disable hint falls back");
    require(pair.next(116000000) == picture::real, "hint suppresses generated frame");
    require(!pair.presented(picture::real, false) && !pair.pending(), "failed present clears pair");
    require(pair.enqueue(4, 132000000, first, true, false, 2), "warmup real frame allowed");
    require(pair.next(132000000) == picture::real, "warmup never displays generated texture");
    require(pair.presented(picture::real, true), "warmup drain");
    require(pair.enqueue(5, 148000000, second, true, false, 2), "queue delayed presentation");
    require(pair.presented(picture::interpolated, true, 160000000), "acknowledge actual generated present time");
    require(pair.next(160000000) == picture::real, "late acknowledgement does not push real-frame deadline");
    require(pair.late_ns() == 12000000, "lateness recorded separately from deadlines");
    pair.clear();
    require(!pair.pending() && pair.frame_id() == 0 && pair.real_due_ns() == 0, "teardown clears pacing state");

    presentation_pair triple;
    require(triple.enqueue(6, 200000000, second, true, false, 3), "queue 3x sequence");
    require(triple.generated_count() == 2 && triple.generated_index() == 0, "3x has two generated outputs");
    require(triple.next(200000000) == picture::interpolated, "3x first generated output");
    require(triple.presented(picture::interpolated, true, 200000000), "3x first generated acknowledged");
    require(triple.generated_index() == 1, "3x advances output index");
    require(triple.next(205333332) == picture::none, "3x waits one third frame");
    require(triple.next(205333333) == picture::interpolated, "3x second generated output");
    require(triple.presented(picture::interpolated, true, 205333333), "3x second generated acknowledged");
    require(triple.next(210666665) == picture::none, "3x waits before real output");
    require(triple.next(210666666) == picture::real, "3x real output due");
    require(triple.presented(picture::real, true) && !triple.pending(), "3x sequence drained");

    presentation_pair sixfold;
    require(sixfold.enqueue(7, 300000000, second, true, false, 6), "queue 6x sequence");
    require(sixfold.generated_count() == 5, "6x has five generated outputs");
    for (unsigned index = 0; index < 5; ++index)
    {
        require(sixfold.next(300000000 + (index * 2666666)) == picture::interpolated,
                "6x generated output order");
        require(sixfold.presented(picture::interpolated, true,
                                  300000000 + (index * 2666666)), "6x generated acknowledged");
    }
    require(sixfold.next(313333329) == picture::none, "6x waits before real output");
    require(sixfold.next(313333330) == picture::real, "6x real output due");
    require(sixfold.presented(picture::real, true) && !sixfold.pending(), "6x sequence drained");
    presentation_pair late;
    require(late.enqueue(10, 400000000, second, true, false, 6), "enqueue late-display case");
    require(late.next(413333330) == picture::real && late.dropped_generated() == 5,
            "skip stale generated pictures instead of replaying a burst");
    require(late.presented(picture::real, true, 413333330), "late real acknowledged");
    require(late.enqueue(11, 414000000, second, true, false, 6), "next input can complete early");
    require(late.next(415999999) == picture::none && late.next(416000000) == picture::interpolated,
            "retain absolute cadence across frame groups");
    late.clear();
    require(!late.enqueue(12, std::numeric_limits<std::uint64_t>::max() - 1,
                         second, true, false, 6), "deadline overflow rejected");
    std::printf("PASS: %u CPU-only frame contract and presentation checks; no GPU used\n", checks);
}
