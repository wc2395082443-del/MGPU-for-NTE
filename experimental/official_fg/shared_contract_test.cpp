#include "shared_constants.hpp"
#include <cstdio>
#include <limits>

int main()
{
    using namespace mgpu::official_fg;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool condition, const char *name) {
        ++checks;
        if (!condition) { ++failures; std::printf("FAIL %s\n", name); }
    };
    mgpu::fgmap::camera_snapshot source{};
    source.valid = 1;
    source.frame_id = 100;
    source.capture_ns = 1000000000;
    source.camera_near = 0.1f;
    source.camera_far = 10000.0f;
    source.camera_fov = 1.2f;
    source.camera_aspect = 16.0f / 9.0f;
    source.mvec_scale[0] = 0.001f;
    source.mvec_scale[1] = -0.002f;
    source.jitter[0] = 0.25f;
    source.jitter[1] = -0.125f;
    source.camera_motion_included = 1;
    source.depth_inverted = 1;
    source.motion_vectors_dilated = 1;
    source.mvec_jittered = 1;
    source.camera_up[1] = source.camera_right[0] = source.camera_fwd[2] = 1;
    source.depth_subrect_width = source.mvec_subrect_width = 2562;
    source.depth_subrect_height = source.mvec_subrect_height = 1441;
    const std::array<plane_description, shared_planes> descriptions{{{3840, 2160, 10}, {2562, 1441, 41}, {2562, 1441, 34}}};
    for (unsigned component = 0; component < 16; ++component) {
        source.camera_view_to_clip[component] = static_cast<float>(component + 1);
        source.clip_to_camera_view[component] = static_cast<float>(component + 17);
        source.clip_to_lens_clip[component] = static_cast<float>(component + 33);
        source.clip_to_prev_clip[component] = static_cast<float>(component + 49);
        source.prev_clip_to_clip[component] = static_cast<float>(component + 65);
    }
    frame_packet packet{};
    check(pack_frame(packet, 42, 1, 100, 100, 100, 7, source, 21.0f, descriptions), "pack exact same-frame camera");
    frame_packet rejected{};
    auto cropped = source;
    cropped.mvec_subrect_x = 1;
    check(!pack_frame(rejected, 42, 1, 100, 100, 100, 7, cropped, 21.0f, descriptions), "unapplied motion crop rejected");
    cropped = source;
    cropped.depth_subrect_width--;
    check(!pack_frame(rejected, 42, 1, 100, 100, 100, 7, cropped, 21.0f, descriptions), "stale depth extent rejected");
    cropped = source;
    cropped.mvec_subrect_height--;
    check(!pack_frame(rejected, 42, 1, 100, 100, 100, 7, cropped, 21.0f, descriptions), "stale motion extent rejected");
    sl::Constants constants{};
    check(unpack_constants(packet, 42, constants), "decode SDK constants");
    check(constants.mvecScale.x == source.mvec_scale[0] && constants.mvecScale.y == source.mvec_scale[1],
        "motion remains normalized, never multiplied by NR guide width");
    check(constants.jitterOffset.x == 0.25f && constants.jitterOffset.y == -0.125f, "jitter retained");
    check(constants.minRelativeLinearDepthObjectSeparation == 21.0f, "depth separation retained");
    check(std::memcmp(&constants.cameraViewToClip, source.camera_view_to_clip, 64) == 0, "view matrix retained");
    check(std::memcmp(&constants.clipToCameraView, source.clip_to_camera_view, 64) == 0, "inverse projection retained");
    check(std::memcmp(&constants.clipToLensClip, source.clip_to_lens_clip, 64) == 0, "lens matrix retained");
    check(std::memcmp(&constants.clipToPrevClip, source.clip_to_prev_clip, 64) == 0, "previous transform retained");
    check(std::memcmp(&constants.prevClipToClip, source.prev_clip_to_clip, 64) == 0, "inverse previous retained");
    check(constants.depthInverted == sl::eTrue && constants.cameraMotionIncluded == sl::eTrue &&
        constants.motionVectorsDilated == sl::eTrue && constants.motionVectorsJittered == sl::eTrue, "guide flags retained");
    check(!unpack_constants(packet, 43, constants), "stale session rejected");
    auto invalid = packet;
    invalid.depth_frame = 99;
    check(!valid_packet(invalid, 42), "old depth rejected");
    invalid = packet;
    invalid.motion_frame = 101;
    check(!valid_packet(invalid, 42), "future motion rejected");
    invalid = packet;
    invalid.camera_frame = 99;
    check(!valid_packet(invalid, 42), "old camera rejected");
    invalid = packet;
    invalid.source_generated = 1;
    check(!valid_packet(invalid, 42), "upstream generated frame rejected");
    invalid = packet;
    invalid.neural_output = 0;
    check(!valid_packet(invalid, 42), "raw input cannot masquerade as NR");
    invalid = packet;
    invalid.camera.motion_scale[0] = 0;
    check(!valid_packet(invalid, 42), "zero motion scale rejected");
    invalid = packet;
    invalid.camera.view_to_clip[2] = std::numeric_limits<float>::quiet_NaN();
    check(!valid_packet(invalid, 42), "nonfinite matrix rejected");
    producer_schedule producer(42);
    consumer_schedule consumer(42);
    for (unsigned sequence = 1; sequence <= shared_slots; ++sequence) {
        packet.sequence = sequence;
        packet.color_frame = packet.depth_frame = packet.motion_frame = packet.camera_frame = 99 + sequence;
        packet.capture_ns = 1000000000ull + sequence;
        check(producer.reserve(packet, 0, 0), "unused slot reserved");
        check(!producer.reserve(packet, 0, 0), "double reserve rejected");
        check(producer.publish(), "publish ordered frame");
        check(consumer.acquire(packet), "consumer takes ordered frame");
        check(!consumer.release(sequence + 1), "out-of-order release rejected");
        check(consumer.release(sequence), "consumer releases in order");
    }
    packet.sequence = 4;
    packet.color_frame = packet.depth_frame = packet.motion_frame = packet.camera_frame = 103;
    packet.capture_ns = 1000000004ull;
    check(!producer.reserve(packet, 0, 3), "GPU release missing prevents overwrite");
    check(!producer.reserve(packet, 3, 0), "producer completion also required");
    check(!producer.reserve(packet, UINT64_MAX, 3), "device loss is not completion");
    check(producer.reserve(packet, 1, 3), "only completed slot can be recycled");
    producer.cancel_unsubmitted();
    check(producer.reserve(packet, 1, 3), "unsubmitted recording can retry");
    check(producer.publish(), "recycled frame published");
    check(!producer.idle(3, 4), "pending consumer lease not drained");
    check(producer.idle(4, 4), "bidirectional fences drained");
    packet.sequence = 5;
    packet.color_frame = packet.depth_frame = packet.motion_frame = packet.camera_frame = 105;
    packet.capture_ns++;
    check(!producer.reserve(packet, 4, 4), "source frame gap requires history reset");
    packet.camera.reset = 1;
    check(producer.reserve(packet, 4, 4), "explicit reset allows source discontinuity");
    producer.cancel_unsubmitted();
    producer.disconnect();
    check(!producer.reserve(packet, 4, 4), "dead consumer cannot silently recycle slots");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
