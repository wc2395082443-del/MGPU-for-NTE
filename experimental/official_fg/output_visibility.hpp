#pragma once

namespace mgpu::official_fg
{
struct visible_routes { bool helper = false, fallback = false; };
constexpr visible_routes choose_visible_routes(bool overlay_closed, bool peek_hidden,
    unsigned view_mode, bool neural_picture_available, bool official_presenting)
{
    if (!overlay_closed || peek_hidden || !neural_picture_available || view_mode > 2) return {};
    return {view_mode == 0, view_mode != 0 || !official_presenting};
}
}
