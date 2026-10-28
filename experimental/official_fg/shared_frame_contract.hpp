#pragma once

#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include <type_traits>

namespace mgpu::official_fg
{
constexpr unsigned shared_slots = 3;
constexpr unsigned shared_planes = 3;
constexpr std::uint32_t transport_magic = 0x3847504d;
constexpr std::uint32_t transport_version = 1;

struct plane_description
{
    std::uint32_t width = 0, height = 0, format = 0;
};

struct camera_packet
{
    std::array<float, 16> view_to_clip{}, clip_to_view{}, clip_to_lens{}, clip_to_previous{}, previous_to_clip{};
    std::array<float, 3> position{}, up{}, right{}, forward{};
    std::array<float, 2> jitter{}, motion_scale{}, pinhole{};
    float near_plane = 0, far_plane = 0, fov = 0, aspect = 0;
    float invalid_motion = 0, min_depth_separation = 40;
    std::uint32_t depth_inverted = 0, camera_motion_included = 0, motion_dilated = 0;
    std::uint32_t motion_jittered = 0, orthographic = 0, reset = 0;
};

struct frame_packet
{
    std::uint64_t generation = 0, sequence = 0, capture_ns = 0;
    std::uint32_t color_frame = 0, depth_frame = 0, motion_frame = 0, camera_frame = 0;
    std::uint32_t viewport = 0, neural_output = 0, camera_valid = 0, source_generated = 0;
    camera_packet camera{};
};

struct shared_manifest
{
    std::uint32_t magic = transport_magic, version = transport_version;
    std::uint32_t byte_size = 0, slots = shared_slots;
    std::uint64_t generation = 0;
    std::uint32_t adapter_low = 0;
    std::int32_t adapter_high = 0;
    std::array<plane_description, shared_planes> descriptions{};
    std::array<std::array<std::uint64_t, shared_planes>, shared_slots> resources{};
    std::uint64_t ready_fence = 0, released_fence = 0;
};

static_assert(std::is_trivially_copyable_v<frame_packet>);
static_assert(std::is_standard_layout_v<frame_packet>);
static_assert(std::is_trivially_copyable_v<shared_manifest>);

inline bool valid_camera(const camera_packet &camera)
{
    const auto finite = [](const auto &values) {
        for (float value : values) if (!std::isfinite(value)) return false;
        return true;
    };
    if (!finite(camera.view_to_clip) || !finite(camera.clip_to_view) || !finite(camera.clip_to_lens) ||
        !finite(camera.clip_to_previous) || !finite(camera.previous_to_clip) ||
        !finite(camera.position) || !finite(camera.up) || !finite(camera.right) || !finite(camera.forward) ||
        !finite(camera.jitter) || !finite(camera.motion_scale) || !finite(camera.pinhole)) return false;
    if (!std::isfinite(camera.near_plane) || !std::isfinite(camera.far_plane) ||
        !std::isfinite(camera.fov) || !std::isfinite(camera.aspect) || !std::isfinite(camera.invalid_motion) ||
        !std::isfinite(camera.min_depth_separation)) return false;
    return camera.near_plane > 0 && camera.far_plane > camera.near_plane && camera.fov > 0 &&
        camera.aspect > 0 && camera.motion_scale[0] != 0 && camera.motion_scale[1] != 0 &&
        camera.depth_inverted <= 1 && camera.camera_motion_included <= 1 && camera.motion_dilated <= 1 &&
        camera.motion_jittered <= 1 && camera.orthographic <= 1 && camera.reset <= 1;
}

inline bool valid_packet(const frame_packet &packet, std::uint64_t generation)
{
    return generation != 0 && packet.generation == generation && packet.sequence != 0 &&
        packet.sequence != UINT64_MAX && packet.capture_ns != 0 && packet.color_frame != 0 &&
        packet.color_frame == packet.depth_frame && packet.color_frame == packet.motion_frame &&
        packet.color_frame == packet.camera_frame && packet.neural_output == 1 &&
        packet.camera_valid == 1 && packet.source_generated == 0 && valid_camera(packet.camera);
}

constexpr unsigned slot_for(std::uint64_t sequence)
{
    return static_cast<unsigned>((sequence - 1) % shared_slots);
}

class producer_schedule
{
public:
    explicit producer_schedule(std::uint64_t generation) : generation_(generation) {}
    bool reserve(const frame_packet &packet, std::uint64_t released, std::uint64_t ready)
    {
        if (disconnected_ || reserved_ || next_ == UINT64_MAX || packet.sequence != next_ ||
            !valid_packet(packet, generation_) || released == UINT64_MAX || ready == UINT64_MAX) return false;
        if (previous_frame_ != 0) {
            const std::uint32_t distance = packet.color_frame - previous_frame_;
            if (distance == 0 || distance >= 0x80000000u || packet.capture_ns <= previous_capture_) return false;
            if (distance != 1 && packet.camera.reset == 0) return false;
        }
        const auto previous = slots_[slot_for(next_)];
        if (previous != 0 && (released < previous || ready < previous)) return false;
        reserved_ = true;
        packet_ = packet;
        return true;
    }
    bool publish()
    {
        if (!reserved_ || disconnected_) return false;
        slots_[slot_for(next_)] = next_;
        previous_frame_ = packet_.color_frame;
        previous_capture_ = packet_.capture_ns;
        ++next_;
        reserved_ = false;
        return true;
    }
    void cancel_unsubmitted() { reserved_ = false; }
    void disconnect() { disconnected_ = true; }
    bool idle(std::uint64_t released, std::uint64_t ready) const
    {
        if (reserved_ || released == UINT64_MAX || ready == UINT64_MAX) return false;
        for (auto value : slots_) if (released < value || ready < value) return false;
        return true;
    }
private:
    std::uint64_t generation_ = 0, next_ = 1, previous_capture_ = 0;
    std::uint32_t previous_frame_ = 0;
    std::array<std::uint64_t, shared_slots> slots_{};
    frame_packet packet_{};
    bool reserved_ = false, disconnected_ = false;
};

class consumer_schedule
{
public:
    explicit consumer_schedule(std::uint64_t generation) : generation_(generation) {}
    bool acquire(const frame_packet &packet)
    {
        if (held_ || packet.sequence != next_ || !valid_packet(packet, generation_)) return false;
        held_ = true;
        return true;
    }
    bool release(std::uint64_t sequence)
    {
        if (!held_ || sequence != next_) return false;
        held_ = false;
        ++next_;
        return true;
    }
private:
    std::uint64_t generation_ = 0, next_ = 1;
    bool held_ = false;
};
}
