#pragma once

#include "../../src/aligned_frame_policy.hpp"
#include "../../src/fg_map.hpp"
#include <cstring>

namespace mgpu::secondary_fg
{
inline bool aligned_camera(const aligned::guide_constants &source, aligned::frame_key expected,
    unsigned width, unsigned height, fgmap::camera_snapshot &target)
{
    target = {};
    if (!source.valid || !source.spatial_valid || !source.camera.valid ||
        source.key.frame != expected.frame || source.key.viewport != expected.viewport ||
        expected.frame == 0 || width == 0 || height == 0 || source.camera.capture_ns == 0) return false;
    target.frame_id = expected.frame;
    target.capture_ns = source.camera.capture_ns;
    std::memcpy(target.camera_view_to_clip, source.camera.view_to_clip.data(), sizeof target.camera_view_to_clip);
    std::memcpy(target.clip_to_camera_view, source.clip_to_view.data(), sizeof target.clip_to_camera_view);
    std::memcpy(target.clip_to_lens_clip, source.camera.clip_to_lens.data(), sizeof target.clip_to_lens_clip);
    std::memcpy(target.clip_to_prev_clip, source.clip_to_previous.data(), sizeof target.clip_to_prev_clip);
    std::memcpy(target.prev_clip_to_clip, source.previous_to_clip.data(), sizeof target.prev_clip_to_clip);
    std::memcpy(target.camera_pos, source.camera.position.data(), sizeof target.camera_pos);
    std::memcpy(target.camera_up, source.camera.up.data(), sizeof target.camera_up);
    std::memcpy(target.camera_right, source.camera.right.data(), sizeof target.camera_right);
    std::memcpy(target.camera_fwd, source.camera.forward.data(), sizeof target.camera_fwd);
    std::memcpy(target.pinhole_offset, source.camera.pinhole.data(), sizeof target.pinhole_offset);
    target.jitter[0] = source.jitter_x;
    target.jitter[1] = source.jitter_y;
    target.mvec_scale[0] = source.motion_scale_x;
    target.mvec_scale[1] = source.motion_scale_y;
    target.camera_near = source.camera_near;
    target.camera_far = source.camera_far;
    target.camera_fov = source.camera_fov;
    target.camera_aspect = source.camera.aspect;
    target.depth_inverted = source.depth_inverted ? 1u : 0u;
    target.reset = source.reset ? 1u : 0u;
    target.camera_motion_included = 1u;
    target.motion_vectors_dilated = source.motion_dilated ? 1u : 0u;
    target.motion_vectors_invalid_value = source.camera.invalid_motion;
    target.mvec_subrect_width = target.depth_subrect_width = width;
    target.mvec_subrect_height = target.depth_subrect_height = height;
    target.valid = 1u;
    return true;
}
}
