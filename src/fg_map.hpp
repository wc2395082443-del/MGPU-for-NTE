// MGPU Bridge - D2.0 NR step one: the frame-gen vector map (0.3.0 R&D branch).
//
// WHY. With frame gen on, NR sizzles and jitters (operator). From the code:
// the stream copies the game's vectors once per RENDERED frame, into the slot
// the NEXT present seals (stream_mvec_copy: produced % RING). Frame gen
// presents twice per rendered frame (x2), so one present of each pair is
// sealed without vectors and NR runs on it with DLSSNR.MVec = nullptr (the R61
// rule): NR's history is reprojected on only half the frames. And R87 counts
// presents as game frames, so the frame that does carry vectors gets a whole
// rendered step of motion against a predecessor half a step away.
//
// THE SOURCE, AS FOR SR AND NR (operator, 2026-10-05 12:37: "the path should
// be the same we do with regular SR/NR vectors ... no reason to believe that
// NGX does not expose them too, only if that fails we go down to a probe like
// solution ... no hook just a copy memory pointer"). It does expose them. The
// NGX SDK at the pin has nvsdk_ngx_defs_dlssg.h: frame generation (feature id
// 11) evaluates through the same NVSDK_NGX_D3D12_EvaluateFeature the
// calibrator's pointer swap already sees, with a parameter block whose public
// names include DLSSG.MVecs, DLSSG.MvecScaleX/Y, DLSSG.Depth,
// DLSSG.Backbuffer, DLSSG.OutputInterpolated, DLSSG.OutputReal,
// DLSSG.ClipToPrevClip / PrevClipToClip (camera), DLSSG.MultiFrameCount /
// MultiFrameIndex (which generated frame), DLSSG.BackbufferFrameID,
// DLSSG.Reset and DLSSG.NotRenderingGameFrames. R135 skips this feature for
// the COPY on purpose; reading it is a different thing.
//
// WHAT THIS DOES - READ ONLY. For every evaluate the calibrator's tap sees, it
// reads the public names (generic ones for the scene feature, DLSSG ones for
// frame generation) and records, in one ordered stream with QPC and thread:
// scene evaluates, frame-gen evaluates, our vector copies (with the slot) and
// our seals (one per captured present: the frame index, whether the slot had
// vectors, and the back buffer we captured). Every 10 s: summary lines and a
// burst of the last 96 events. Nothing is copied, bound, skipped or changed.
//
// WHAT IT ANSWERS:
//   1. What frame gen's block carries for motion: DLSSG.MVecs (the same
//      resource the scene feature gets, or its own), its scale, the camera
//      matrices, and which generated frame (MultiFrameIndex) an evaluate is.
//   2. Which present of each pair is which, read rather than inferred: the
//      back buffer we capture against DLSSG.OutputInterpolated /
//      DLSSG.OutputReal / DLSSG.Backbuffer.
//   3. Which present of each pair receives our copied vectors today.
//      "Frames without vectors = generated" was a match of totals (50.1% vs
//      50%) and may be inverted.
//
// KEY. NRFrameFilter=0 (or absent): nothing; every call site is one load.
// NRFrameFilter=1: this map.
//
// D2.0 NR2 / NR3 (2026-10-05, after the N1 map run answered the questions:
// fg reads the SAME MotionVectors resource as the scene feature, there is no
// second field; a generated frame cannot be told by resource identity; the
// only per-seal fact that tells it apart is "no fresh vector copy"):
// R313: NRFrameFilter=2 only applies the fraction/reuse policy to captures
//   matched to distinct outputs of a successful, identified FG evaluate.
//   A matching interpolated capture must precede its matching real capture.
//   Unmatched, aliased, failed or expired records keep motion fraction 1 and
//   cannot reuse FG vectors. This is conservative for copy-based integrations:
//   they remain unknown until their resource lineage can be established.
// NRFrameFilter=3: as 2, and only 1 generated frame in N (NRFrameKeep, 2..8,
//   default 2) is evaluated; the rest re-present the last output. THE DEFAULT
//   (key absent) since 2026-10-05 22:01: Dawnwalker's sizzle on skin went
//   with mode 3 at N=2 and not with mode 2. Both modes and N are live on the
//   ReShade panel; the ini is the default at launch; NRFrameFilter=0 is off.
// R313: the first seal after a call is NOT automatically generated. Only a
//   matched interpolated capture may be thinned. Recent API calls are retained
//   separately for diagnostics and the secondary-FG exclusion guard.
//
// Ledger: Claude outputs\0.3.0-Discovery\D2_0_LEDGER.md, section 10.
#pragma once

#include <cstdint>
#include "fg_present_evidence.hpp"

namespace mgpu::fgmap
{
    struct camera_snapshot
    {
        std::uint64_t frame_id = 0;
        std::uint64_t capture_ns = 0;
        std::uint64_t scene_sequence = 0;
        std::uint64_t backbuffer = 0;
        std::uint64_t output_real = 0;
        std::uint64_t output_interpolated = 0;
        std::uint64_t depth = 0;
        std::uint64_t motion = 0;
        float camera_view_to_clip[16] = {};
        float clip_to_camera_view[16] = {};
        float clip_to_lens_clip[16] = {};
        float clip_to_prev_clip[16] = {};
        float prev_clip_to_clip[16] = {};
        float jitter[2] = {};
        float mvec_scale[2] = {};
        float pinhole_offset[2] = {};
        float camera_pos[3] = {};
        float camera_up[3] = {};
        float camera_right[3] = {};
        float camera_fwd[3] = {};
        float camera_near = 0.0f;
        float camera_far = 0.0f;
        float camera_fov = 0.0f;
        float camera_aspect = 0.0f;
        unsigned color_buffers_hdr = 0;
        unsigned depth_inverted = 0;
        unsigned camera_motion_included = 0;
        unsigned reset = 0;
        unsigned automode_override_reset = 0;
        unsigned not_rendering_game_frames = 0;
        unsigned ortho_projection = 0;
        float motion_vectors_invalid_value = 0.0f;
        unsigned motion_vectors_dilated = 0;
        unsigned menu_detection_enabled = 0;
        unsigned mvec_subrect_width = 0;
        unsigned mvec_subrect_height = 0;
        unsigned depth_subrect_width = 0;
        unsigned depth_subrect_height = 0;
        unsigned mvec_subrect_x = 0;
        unsigned mvec_subrect_y = 0;
        unsigned depth_subrect_x = 0;
        unsigned depth_subrect_y = 0;
        unsigned mvec_jittered = 0;
        unsigned valid = 0;
    };

    // What one evaluate's parameter block said. Resources by identity only;
    // the only call made on one is GetDesc, on a handle seen for the first
    // time (the game handed it to NGX in this same call).
    struct eval_rec
    {
        // Generic NGX names (the scene feature: SR / RR). have: bit 0 Color,
        // 1 Depth, 2 MotionVectors, 3 Output, 4 MV scale, 5 jitter, 6 Reset.
        unsigned long long color, depth, mvec, output;
        float mv_scale_x, mv_scale_y, jitter_x, jitter_y;
        unsigned int reset;
        unsigned int have;

        // DLSSG names (frame generation, nvsdk_ngx_defs_dlssg.h). Read only
        // when the evaluating handle is not the scene feature. g_have bits:
        //  0 Backbuffer  1 MVecs  2 Depth  3 HUDLess  4 OutputInterpolated
        //  5 OutputReal  6 MvecScale  7 Reset  8 CameraMotionIncluded
        //  9 MvecJittered  10 MultiFrameCount  11 MultiFrameIndex
        //  12 NotRenderingGameFrames  13 BackbufferFrameID  14 MVecs subrect
        //  15 ClipToPrevClip (16 floats copied)
        unsigned long long g_backbuffer, g_mvecs, g_depth, g_hudless, g_out_interp, g_out_real;
        float g_mv_scale_x, g_mv_scale_y;
        unsigned int g_reset, g_cam_motion, g_mv_jittered, g_mf_count, g_mf_index, g_not_rendering;
        unsigned long long g_bb_frame_id;
        unsigned int g_mv_sub_w, g_mv_sub_h;
        float g_clip_to_prev[16];
        unsigned int g_have;

        bool scene;   // the calibrator's latched scene feature (SR / RR)
        bool succeeded;
        unsigned camera_valid;
        camera_snapshot camera;
    };

    // on_init_swapchain, never DllMain: reads NRFrameFilter once.
    void init();
    bool on();      // mode >= 1: the map runs (it is also the fg detector for the acting modes)

    // D2.0 NR2 / NR3, read by the stream consumer on the bridge thread.
    bool act();                 // mode >= 2
    bool fg_active();           // recent API calls only; not proof of displayed interpolation
    bool fg_presented_active(); // recent API calls plus a verified interpolated Present
    captured_policy policy_for_seal(unsigned slot, unsigned long long frame_index);
    unsigned reuse_window();    // 2 x (MultiFrameCount + 1) presents
    float step_fraction();      // 1 / (MultiFrameCount + 1)
    bool thin_this();           // mode 3: counts a generated seal; true = do not evaluate it
    bool seal_generated(unsigned slot, unsigned long long frame_index);
    void on_reuse();            // a generated frame was evaluated with the kept vectors

    // Panel (dllmain). Live; the ini stays the launch default.
    int ui_mode();
    void ui_set_mode(int m);
    unsigned ui_keep();
    void ui_set_keep(unsigned n);
    void ui_counters(bool &fg_now, unsigned long long &reused, unsigned long long &thinned);

    // calibrator hook_create, after the real create succeeded.
    void on_create(unsigned int feature_id, unsigned long long handle);
    void on_release(unsigned long long handle);

    // Called after the real evaluate returns; succeeded records its result.
    void on_evaluate(unsigned long long handle, const eval_rec &r);

    bool read_camera(camera_snapshot &out);
    std::uint64_t scene_sequence();

    // gpu1 stream_mvec_copy, after the copy is recorded into `slot`.
    void on_mvec_copy(unsigned int slot);

    // gpu1 stream_on_finish_effects, where the seal decides mvec_valid: one
    // call per captured present of the game, with the back buffer captured.
    void on_seal(unsigned long long frame_index, unsigned int slot, unsigned int mvec_valid,
                 unsigned long long back_buffer);
}
