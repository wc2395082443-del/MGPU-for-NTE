#pragma once

#include "shared_frame_transport.hpp"
#include "../../src/fg_map.hpp"
#include "official_metrics.hpp"
#include "reflex_contract.hpp"

namespace mgpu::official_bridge
{
struct status
{
    bool requested = false, ready = false, presenting = false, failed = false, visible = true;
    unsigned maximum_multiplier = 0;
    std::uint64_t exported = 0, real_presents = 0, sdk_presents = 0;
    official_fg::rate_snapshot current_rates{}, visible_rates{};
};
void request(bool enabled, unsigned multiplier);
bool selected();
bool backpressured();
bool record(ID3D12GraphicsCommandList *commands, HWND game_window,
    const std::array<official_fg::texture_binding, official_fg::shared_planes> &sources,
    std::uint32_t source_frame, std::uint32_t viewport,
    const fgmap::camera_snapshot &camera, float min_depth_separation, bool reset_history);
bool submitted(ID3D12CommandQueue *queue);
void submission_failed(bool executed);
bool shutdown();
void set_visible(bool visible);
status inspect();
std::uint64_t source_clock_ns();
void source_sleep(std::uint32_t frame, std::uint64_t native_begin_ns = 0, std::uint64_t native_end_ns = 0);
void source_marker(std::uint32_t frame, unsigned marker, std::uint64_t native_begin_ns = 0, std::uint64_t native_end_ns = 0);
}
