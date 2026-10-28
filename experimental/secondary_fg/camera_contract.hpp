#pragma once

#include "../../src/fg_map.hpp"

namespace mgpu::secondary_fg
{
struct captured_tuple
{
    std::uint64_t scene_sequence = 0;
    std::uint64_t color = 0;
    std::uint64_t depth = 0;
    std::uint64_t motion = 0;
    std::uint64_t capture_ns = 0;
    unsigned guide_width = 0;
    unsigned guide_height = 0;
    bool aligned_inputs = false;
};

enum class camera_match_failure
{
    none,
    invalid_camera,
    missing_frame_id,
    missing_capture_time,
    future_capture,
    stale_capture,
    not_rendering,
    ortho_projection,
    jittered_motion,
    color_identity,
    depth_identity,
    motion_identity,
    guide_extent,
    motion_crop,
    depth_crop,
};

struct camera_match_result
{
    camera_match_failure failure = camera_match_failure::none;
    bool matched() const { return failure == camera_match_failure::none; }
};

inline camera_match_result match_camera(const fgmap::camera_snapshot &camera,
                                        const captured_tuple &tuple)
{
    if (camera.valid == 0) return {camera_match_failure::invalid_camera};
    if (camera.frame_id == 0) return {camera_match_failure::missing_frame_id};
    if (camera.capture_ns == 0) return {camera_match_failure::missing_capture_time};
    if (tuple.capture_ns < camera.capture_ns) return {camera_match_failure::future_capture};
    if (tuple.capture_ns - camera.capture_ns > 100000000)
        return {camera_match_failure::stale_capture};
    if (camera.not_rendering_game_frames != 0)
        return {camera_match_failure::not_rendering};
    if (camera.ortho_projection != 0)
        return {camera_match_failure::ortho_projection};
    if (camera.mvec_jittered != 0)
        return {camera_match_failure::jittered_motion};
    if (tuple.color == 0 || tuple.depth == 0 || tuple.motion == 0)
        return {camera_match_failure::color_identity};
    if (!tuple.aligned_inputs)
    {
        if (tuple.color == camera.output_interpolated ||
            (tuple.color != camera.backbuffer && tuple.color != camera.output_real))
            return {camera_match_failure::color_identity};
        if (tuple.depth != camera.depth)
            return {camera_match_failure::depth_identity};
        if (tuple.motion != camera.motion)
            return {camera_match_failure::motion_identity};
    }
    if (tuple.guide_width == 0 || tuple.guide_height == 0)
        return {camera_match_failure::guide_extent};
    if (camera.mvec_subrect_width != tuple.guide_width ||
        camera.mvec_subrect_height != tuple.guide_height)
        return {camera_match_failure::guide_extent};
    if (camera.depth_subrect_width != tuple.guide_width ||
        camera.depth_subrect_height != tuple.guide_height)
        return {camera_match_failure::guide_extent};
    if (camera.mvec_subrect_x != 0 || camera.mvec_subrect_y != 0)
        return {camera_match_failure::motion_crop};
    if (camera.depth_subrect_x != 0 || camera.depth_subrect_y != 0)
        return {camera_match_failure::depth_crop};
    return {};
}

inline bool matching_camera(const fgmap::camera_snapshot &camera, const captured_tuple &tuple)
{
    return match_camera(camera, tuple).matched();
}

inline const char *camera_match_failure_name(camera_match_failure failure)
{
    switch (failure)
    {
    case camera_match_failure::none: return "none";
    case camera_match_failure::invalid_camera: return "invalid_camera";
    case camera_match_failure::missing_frame_id: return "missing_frame_id";
    case camera_match_failure::missing_capture_time: return "missing_capture_time";
    case camera_match_failure::future_capture: return "future_capture";
    case camera_match_failure::stale_capture: return "stale_capture";
    case camera_match_failure::not_rendering: return "not_rendering";
    case camera_match_failure::ortho_projection: return "ortho_projection";
    case camera_match_failure::jittered_motion: return "jittered_motion";
    case camera_match_failure::color_identity: return "color_identity";
    case camera_match_failure::depth_identity: return "depth_identity";
    case camera_match_failure::motion_identity: return "motion_identity";
    case camera_match_failure::guide_extent: return "guide_extent";
    case camera_match_failure::motion_crop: return "motion_crop";
    case camera_match_failure::depth_crop: return "depth_crop";
    }
    return "unknown";
}
}
