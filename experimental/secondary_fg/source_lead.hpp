#pragma once

#include <cstdint>
#include "frame_contract.hpp"

namespace mgpu::secondary_fg
{
inline constexpr unsigned source_lead_limit = 2;
inline constexpr unsigned source_wait_limit_ms = 50;
inline constexpr std::uint64_t known_frame = 1ull << 32;

constexpr bool source_needs_wait(std::uint64_t submitted, std::uint64_t completed)
{
    if ((submitted & known_frame) == 0 || (completed & known_frame) == 0) return false;
    const auto distance = static_cast<std::uint32_t>(submitted) - static_cast<std::uint32_t>(completed);
    return distance < 0x80000000u && distance >= source_lead_limit;
}

struct completed_output
{
    std::uint64_t seal = 0;
    std::uint64_t camera_frame = 0;
    std::uint64_t capture_ns = 0;
    std::uint64_t completed_ns = 0;
    decision generation{};
    bool generated_valid = false;
};
}
