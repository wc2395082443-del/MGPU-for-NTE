#include "fg_mode_policy.hpp"
#include <cstdio>

int main()
{
    if (mgpu::official_fg::mode_for_multiplier(2) != mgpu::official_fg::fg_mode::fixed) return 1;
    if (mgpu::official_fg::mode_for_multiplier(3) != mgpu::official_fg::fg_mode::fixed) return 2;
    if (mgpu::official_fg::mode_for_multiplier(4) != mgpu::official_fg::fg_mode::fixed) return 3;
    if (mgpu::official_fg::mode_for_multiplier(6) != mgpu::official_fg::fg_mode::dynamic) return 4;
    std::puts("PASS: 2x-4x fixed, 6x dynamic");
    return 0;
}
