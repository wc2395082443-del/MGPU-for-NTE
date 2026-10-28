#pragma once

#include <array>
#include <cstdint>

namespace mgpu::official_fg
{
constexpr std::uint32_t sleep_marker = 100;
constexpr std::uint32_t input_ping_marker = 8;

constexpr bool source_marker_supported(unsigned kind)
{
    return kind <= 3 || kind == input_ping_marker;
}

constexpr bool relay_marker_to_sdk(unsigned kind)
{
    return kind <= 1 || kind == input_ping_marker;
}

constexpr unsigned source_timing_index(unsigned kind)
{
    return kind == sleep_marker ? 0 : kind <= 3 ? kind + 1 : 5;
}

struct marker_timing
{
    std::uint64_t native_begin_ns = 0, native_end_ns = 0, queued_ns = 0;
    std::uint64_t dispatch_begin_ns = 0, dispatch_end_ns = 0;
};

struct source_reflex_timing
{
    std::array<marker_timing, 5> phases{};
};
}
