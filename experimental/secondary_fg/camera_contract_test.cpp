#include "camera_contract.hpp"

#include <cstdio>
#include <cstdlib>

using namespace mgpu::secondary_fg;

static unsigned checks = 0;

static void require(bool passed, const char *name)
{
    ++checks;
    if (!passed)
    {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}

int main()
{
    mgpu::fgmap::camera_snapshot camera{};
    camera.valid = 1;
    camera.frame_id = 9;
    camera.scene_sequence = 12;
    camera.capture_ns = 1000000000;
    camera.backbuffer = 101;
    camera.output_real = 102;
    camera.output_interpolated = 103;
    camera.depth = 201;
    camera.motion = 301;
    camera.mvec_subrect_width = camera.depth_subrect_width = 1920;
    camera.mvec_subrect_height = camera.depth_subrect_height = 1080;
    captured_tuple tuple{12, 101, 201, 301, 1016000000, 1920, 1080};
    require(matching_camera(camera, tuple), "exact real-frame tuple");
    auto changed = tuple;
    changed.color = 102;
    require(matching_camera(camera, changed), "explicit SDK real output");
    changed.color = 103;
    require(!matching_camera(camera, changed), "upstream generated texture rejected");
    changed = tuple; changed.scene_sequence = 13;
    require(matching_camera(camera, changed), "scene counter is diagnostic, not frame identity");
    changed = tuple; changed.scene_sequence = 0;
    require(matching_camera(camera, changed), "missing scene counter accepted");
    changed = tuple; changed.depth = 202;
    require(!matching_camera(camera, changed), "unrelated depth rejected");
    changed = tuple; changed.motion = 302;
    require(!matching_camera(camera, changed), "unrelated motion rejected");
    changed = tuple; changed.color = 104;
    require(!matching_camera(camera, changed), "unknown color identity rejected");
    changed = tuple;
    changed.color = 104;
    changed.depth = 202;
    changed.motion = 302;
    changed.aligned_inputs = true;
    require(matching_camera(camera, changed), "aligned bridge inputs accept distinct resources");
    changed = tuple; changed.capture_ns = camera.capture_ns - 1;
    require(!matching_camera(camera, changed), "future camera rejected");
    changed = tuple; changed.capture_ns = camera.capture_ns + 100000001;
    require(!matching_camera(camera, changed), "stale camera rejected");
    changed = tuple; changed.guide_width = 1280;
    require(!matching_camera(camera, changed), "guide extent mismatch rejected");
    auto invalid = camera; invalid.mvec_subrect_x = 1;
    require(!matching_camera(invalid, tuple), "motion crop rejected");
    invalid = camera; invalid.depth_subrect_y = 1;
    require(!matching_camera(invalid, tuple), "depth crop rejected");
    invalid = camera; invalid.mvec_jittered = 1;
    require(!matching_camera(invalid, tuple), "unsupported jittered motion rejected");
    invalid = camera; invalid.not_rendering_game_frames = 1;
    require(!matching_camera(invalid, tuple), "menu/nonrender frame rejected");
    invalid = camera; invalid.ortho_projection = 1;
    require(!matching_camera(invalid, tuple), "unsupported projection rejected");
    invalid = camera; invalid.valid = 0;
    require(!matching_camera(invalid, tuple), "missing camera rejected");
    std::printf("PASS: %u CPU-only camera binding checks; no GPU used\n", checks);
}
