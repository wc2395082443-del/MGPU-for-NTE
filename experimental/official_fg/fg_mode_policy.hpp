#pragma once

namespace mgpu::official_fg
{
enum class fg_mode
{
    fixed,
    dynamic
};

constexpr fg_mode mode_for_multiplier(unsigned multiplier)
{
    return multiplier == 6u ? fg_mode::dynamic : fg_mode::fixed;
}
}
