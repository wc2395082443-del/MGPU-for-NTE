#pragma once

#include <string_view>

namespace mgpu::official_fg
{
enum class probe_mode { invalid, verify, initialize, hidden_swapchain };

constexpr probe_mode parse_mode(std::wstring_view argument)
{
    if (argument == L"--verify") return probe_mode::verify;
    if (argument == L"--initialize-5080") return probe_mode::initialize;
    if (argument == L"--hidden-swapchain-5080") return probe_mode::hidden_swapchain;
    return probe_mode::invalid;
}

constexpr bool matches_adapter(unsigned vendor, bool software, std::wstring_view name)
{
    return vendor == 0x10de && !software && name == L"NVIDIA GeForce RTX 5080";
}

constexpr bool supports_multiplier(unsigned requested, unsigned maximum_generated)
{
    return requested >= 2 && requested <= 6 && maximum_generated >= requested - 1;
}
}
