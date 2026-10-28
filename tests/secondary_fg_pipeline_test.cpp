#include "../experimental/secondary_fg/aligned_camera.hpp"
#include "../experimental/secondary_fg/frame_contract.hpp"
#include "../src/neural_pipeline.hpp"
#include "../src/streamline_frame_identity.hpp"
#include <cstdio>
#include <cstdlib>

static unsigned checks = 0;
static void require(bool value, const char *message)
{
    ++checks;
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

int main()
{
    sl::Constants input{};
    std::array<float, 16> identity{};
    for (unsigned axis = 0; axis != 4; ++axis) identity[axis * 5] = 1.0f;
    std::memcpy(&input.cameraViewToClip, identity.data(), sizeof input.cameraViewToClip);
    std::memcpy(&input.clipToCameraView, identity.data(), sizeof input.clipToCameraView);
    std::memcpy(&input.clipToPrevClip, identity.data(), sizeof input.clipToPrevClip);
    std::memcpy(&input.prevClipToClip, identity.data(), sizeof input.prevClipToClip);
    input.cameraPos = {1, 2, 3};
    input.cameraUp = {0, 1, 0};
    input.cameraRight = {1, 0, 0};
    input.cameraFwd = {0, 0, 1};
    input.cameraNear = 0.1f;
    input.cameraFar = 1000.0f;
    input.cameraFOV = 1.0f;
    input.cameraAspectRatio = 16.0f / 9.0f;
    input.mvecScale = {1.0f / 2562.0f, -1.0f / 1441.0f};
    input.jitterOffset = {0.25f, -0.125f};
    input.cameraMotionIncluded = sl::Boolean::eTrue;
    input.motionVectors3D = sl::Boolean::eFalse;
    input.motionVectorsJittered = sl::Boolean::eFalse;
    input.motionVectorsDilated = sl::Boolean::eTrue;
    input.depthInverted = sl::Boolean::eTrue;
    input.reset = sl::Boolean::eFalse;
    mgpu::motion_packet::identity engine{};
    engine.frame = 420;
    engine.viewport = 7;
    engine.frame_known = engine.viewport_known = true;
    const auto constants = mgpu::sltags::read_constants(&input, engine);
    require(constants.valid && constants.camera.valid, "full camera captured without changing NR validity");
    mgpu::fgmap::camera_snapshot camera{};
    require(mgpu::secondary_fg::aligned_camera(constants, {420, 7}, 2562, 1441, camera), "exact-frame camera accepted");
    require(camera.frame_id == 420 && camera.capture_ns == constants.camera.capture_ns, "source frame and capture time retained");
    require(camera.mvec_scale[0] == input.mvecScale.x && camera.mvec_scale[1] == input.mvecScale.y,
            "FG retains normalized units instead of NR pixel units");
    require(camera.jitter[0] == 0.25f && camera.jitter[1] == -0.125f && camera.depth_inverted == 1,
            "jitter sign and inverted depth retained");
    require(camera.camera_pos[2] == 3 && camera.camera_aspect == input.cameraAspectRatio, "full camera retained");
    require(camera.clip_to_lens_clip[0] == 1 && camera.clip_to_lens_clip[5] == 1 && camera.pinhole_offset[0] == 0,
            "unspecified optional distortion uses identity");
    require(!mgpu::secondary_fg::aligned_camera(constants, {421, 7}, 2562, 1441, camera) && !camera.valid,
            "latest camera from another frame cannot pass");
    require(!mgpu::secondary_fg::aligned_camera(constants, {420, 8}, 2562, 1441, camera), "different viewport rejected");
    input.cameraNear = sl::INVALID_FLOAT;
    auto incomplete = mgpu::sltags::read_constants(&input, engine);
    require(incomplete.valid && !incomplete.camera.valid, "missing FG camera never invalidates NR guides");

    mgpu::neural_pipeline::schedule pipeline;
    mgpu::secondary_fg::presentation_pair pair;
    mgpu::secondary_fg::decision decision{};
    decision.evaluate = decision.allow_generated = true;
    decision.reset = false;
    decision.frame_interval_ns = 30000000;
    mgpu::neural_pipeline::job first{};
    first.seal = first.fence = 1;
    first.snapshot = first.input_index = pipeline.write_slot();
    require(pipeline.begin(first) && pipeline.publish(1), "first FG job becomes display owner");
    require(pair.enqueue(1, 100000000, decision, true, false, 6), "sixfold group begins");
    const unsigned displayed = pipeline.display_slot();
    mgpu::neural_pipeline::job next{};
    next.seal = next.fence = 2;
    next.snapshot = next.input_index = pipeline.write_slot();
    require(next.snapshot != displayed && pipeline.begin(next), "next NR-FG job starts while previous group is pending");
    require(pipeline.display_slot() == displayed && pair.pending(), "new submission does not replace display owner");
    require(!pipeline.publish(1), "completion fence is required");
    require(pipeline.check(2) == mgpu::neural_pipeline::completion::ready, "next GPU work can complete during display");
    require(pair.next(100000000) == mgpu::secondary_fg::picture::interpolated, "first generated image due");
    require(pair.presented(mgpu::secondary_fg::picture::interpolated, true, 101000000), "one millisecond late present");
    require(pair.next(105000000) == mgpu::secondary_fg::picture::interpolated, "next deadline unchanged by prior delay");
    require(pair.next(125000000) == mgpu::secondary_fg::picture::real, "late generated outputs skipped");
    require(pair.presented(mgpu::secondary_fg::picture::real, true, 125000000), "real frame retires group");
    require(!pair.pending() && pipeline.publish(2), "next display owner published only after group ends");
    require(pipeline.display_slot() == next.snapshot && pipeline.write_slot() != next.snapshot,
            "new display slot remains protected from reuse");
    std::printf("PASS: %u exact-camera, units, cadence and output-ownership checks; no NVIDIA runtime loaded\n", checks);
}
