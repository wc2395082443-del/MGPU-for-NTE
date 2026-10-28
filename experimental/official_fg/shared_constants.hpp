#pragma once

#include "shared_frame_contract.hpp"
#include "../../src/fg_map.hpp"
#ifndef MGPU_PACKET_ONLY
#include <sl.h>
#endif
#include <algorithm>
#include <cstring>

namespace mgpu::official_fg
{
inline bool pack_frame(frame_packet &packet, std::uint64_t generation, std::uint64_t sequence,
    std::uint32_t color_frame, std::uint32_t depth_frame, std::uint32_t motion_frame,
    std::uint32_t viewport, const fgmap::camera_snapshot &source, float min_depth_separation,
    const std::array<plane_description, shared_planes> &descriptions)
{
    packet = {};
    if (!source.valid || source.frame_id > UINT32_MAX || source.not_rendering_game_frames) return false;
    if (!descriptions[0].width || !descriptions[0].height ||
        !descriptions[1].width || !descriptions[1].height ||
        !descriptions[2].width || !descriptions[2].height ||
        source.depth_subrect_x || source.depth_subrect_y || source.mvec_subrect_x || source.mvec_subrect_y ||
        source.depth_subrect_width != descriptions[1].width || source.depth_subrect_height != descriptions[1].height ||
        source.mvec_subrect_width != descriptions[2].width || source.mvec_subrect_height != descriptions[2].height)
        return false;
    packet.generation = generation;
    packet.sequence = sequence;
    packet.capture_ns = source.capture_ns;
    packet.color_frame = color_frame;
    packet.depth_frame = depth_frame;
    packet.motion_frame = motion_frame;
    packet.camera_frame = static_cast<std::uint32_t>(source.frame_id);
    packet.viewport = viewport;
    packet.neural_output = 1;
    packet.camera_valid = 1;
    auto &camera = packet.camera;
    std::copy_n(source.camera_view_to_clip, 16, camera.view_to_clip.begin());
    std::copy_n(source.clip_to_camera_view, 16, camera.clip_to_view.begin());
    std::copy_n(source.clip_to_lens_clip, 16, camera.clip_to_lens.begin());
    std::copy_n(source.clip_to_prev_clip, 16, camera.clip_to_previous.begin());
    std::copy_n(source.prev_clip_to_clip, 16, camera.previous_to_clip.begin());
    std::copy_n(source.camera_pos, 3, camera.position.begin());
    std::copy_n(source.camera_up, 3, camera.up.begin());
    std::copy_n(source.camera_right, 3, camera.right.begin());
    std::copy_n(source.camera_fwd, 3, camera.forward.begin());
    std::copy_n(source.jitter, 2, camera.jitter.begin());
    std::copy_n(source.mvec_scale, 2, camera.motion_scale.begin());
    std::copy_n(source.pinhole_offset, 2, camera.pinhole.begin());
    camera.near_plane = source.camera_near;
    camera.far_plane = source.camera_far;
    camera.fov = source.camera_fov;
    camera.aspect = source.camera_aspect;
    camera.invalid_motion = source.motion_vectors_invalid_value;
    camera.min_depth_separation = min_depth_separation;
    camera.depth_inverted = source.depth_inverted;
    camera.camera_motion_included = source.camera_motion_included;
    camera.motion_dilated = source.motion_vectors_dilated;
    camera.motion_jittered = source.mvec_jittered;
    camera.orthographic = source.ortho_projection;
    camera.reset = source.reset || source.automode_override_reset;
    return valid_packet(packet, generation);
}

#ifndef MGPU_PACKET_ONLY
inline bool unpack_constants(const frame_packet &packet, std::uint64_t generation, sl::Constants &target)
{
    target = {};
    if (!valid_packet(packet, generation)) return false;
    const auto &camera = packet.camera;
    std::memcpy(&target.cameraViewToClip, camera.view_to_clip.data(), sizeof target.cameraViewToClip);
    std::memcpy(&target.clipToCameraView, camera.clip_to_view.data(), sizeof target.clipToCameraView);
    std::memcpy(&target.clipToLensClip, camera.clip_to_lens.data(), sizeof target.clipToLensClip);
    std::memcpy(&target.clipToPrevClip, camera.clip_to_previous.data(), sizeof target.clipToPrevClip);
    std::memcpy(&target.prevClipToClip, camera.previous_to_clip.data(), sizeof target.prevClipToClip);
    target.jitterOffset = {camera.jitter[0], camera.jitter[1]};
    target.mvecScale = {camera.motion_scale[0], camera.motion_scale[1]};
    target.cameraPinholeOffset = {camera.pinhole[0], camera.pinhole[1]};
    target.cameraPos = {camera.position[0], camera.position[1], camera.position[2]};
    target.cameraUp = {camera.up[0], camera.up[1], camera.up[2]};
    target.cameraRight = {camera.right[0], camera.right[1], camera.right[2]};
    target.cameraFwd = {camera.forward[0], camera.forward[1], camera.forward[2]};
    target.cameraNear = camera.near_plane;
    target.cameraFar = camera.far_plane;
    target.cameraFOV = camera.fov;
    target.cameraAspectRatio = camera.aspect;
    target.motionVectorsInvalidValue = camera.invalid_motion;
    target.minRelativeLinearDepthObjectSeparation = camera.min_depth_separation;
    target.depthInverted = camera.depth_inverted ? sl::eTrue : sl::eFalse;
    target.cameraMotionIncluded = camera.camera_motion_included ? sl::eTrue : sl::eFalse;
    target.motionVectors3D = sl::eFalse;
    target.motionVectorsDilated = camera.motion_dilated ? sl::eTrue : sl::eFalse;
    target.motionVectorsJittered = camera.motion_jittered ? sl::eTrue : sl::eFalse;
    target.orthographicProjection = camera.orthographic ? sl::eTrue : sl::eFalse;
    target.reset = camera.reset ? sl::eTrue : sl::eFalse;
    return true;
}
#endif
}
