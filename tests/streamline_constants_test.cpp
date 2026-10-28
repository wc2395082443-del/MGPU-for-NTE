#include "../src/streamline_frame_identity.hpp"
#include <cstdio>
#include <limits>

int main()
{
    sl::Constants input{};
    input.mvecScale = {1.0f / 2562.0f, 1.0f / 1441.0f};
    input.cameraMotionIncluded = sl::Boolean::eTrue;
    input.motionVectors3D = sl::Boolean::eFalse;
    input.motionVectorsJittered = sl::Boolean::eFalse;
    input.motionVectorsDilated = sl::Boolean::eTrue;
    input.depthInverted = sl::Boolean::eTrue;
    input.reset = sl::Boolean::eFalse;
    input.jitterOffset = {0.25f, -0.375f};
    std::array<float, 16> matrix{};
    for (unsigned index = 0; index != 16; ++index) matrix[index] = float(index) / 16.0f;
    std::memcpy(&input.clipToPrevClip, matrix.data(), sizeof(input.clipToPrevClip));
    std::memcpy(&input.prevClipToClip, matrix.data(), sizeof(input.prevClipToClip));
    std::memcpy(&input.clipToCameraView, matrix.data(), sizeof(input.clipToCameraView));
    mgpu::motion_packet::identity engine{};
    engine.frame = 120;
    engine.viewport = 3;
    engine.frame_known = engine.viewport_known = true;
    auto result = mgpu::sltags::read_constants(&input, engine);
    if (!result.valid || !result.spatial_valid || !result.motion_dilated || result.jitter_x != 0.25f ||
        result.jitter_y != -0.375f || result.clip_to_previous != matrix || result.previous_to_clip != matrix ||
        result.clip_to_view != matrix || result.key.frame != 120 || result.key.viewport != 3) return 1;
    const auto pixel_scale = result.motion_subrect_scale(2562, 1441);
    if (std::abs(pixel_scale[0] - 1.0f) > 0.000001f ||
        std::abs(pixel_scale[1] - 1.0f) > 0.000001f) return 10;
    const float raw_displacement = 12.0f;
    const float output_width = 3840.0f;
    const float nr_output_displacement = raw_displacement * pixel_scale[0] / 2562.0f * output_width;
    const float streamline_output_displacement = raw_displacement * result.motion_scale_x * output_width;
    if (std::abs(nr_output_displacement - streamline_output_displacement) > 0.00001f) return 11;
    const float doubled_scale_displacement = raw_displacement * result.motion_scale_x * output_width / 2562.0f * output_width;
    if (std::abs(doubled_scale_displacement - streamline_output_displacement) < 8.0f) return 12;
    auto normalized_field = result;
    normalized_field.motion_scale_x = 1.0f;
    normalized_field.motion_scale_y = -1.0f;
    const auto normalized_scale = normalized_field.motion_subrect_scale(1920, 1080);
    if (normalized_scale[0] != 1920.0f || normalized_scale[1] != -1080.0f) return 13;
    const auto native_scale = result.motion_subrect_scale(3840, 2160);
    if (std::abs(native_scale[0] - result.motion_scale_x * 3840.0f) > 0.000001f) return 14;
    if (result.jitter_x != 0.25f || result.jitter_y != -0.375f) return 15;
    std::puts("PASS: NR motion uses guide-subrect units, preserves sign and avoids double upscaling");
    input.jitterOffset.x = std::numeric_limits<float>::quiet_NaN();
    result = mgpu::sltags::read_constants(&input, engine);
    if (!result.valid || result.spatial_valid) return 2;
    input.jitterOffset.x = sl::INVALID_FLOAT;
    if (mgpu::sltags::read_constants(&input, engine).spatial_valid) return 3;
    input.motionVectorsJittered = sl::Boolean::eTrue;
    if (mgpu::sltags::read_constants(&input, engine).valid) return 4;
    std::puts("PASS: frame-scoped spatial metadata preserved; unknown jitter never silently becomes zero");
    return 0;
}
