#pragma once

#include <cstdint>
#include <array>
#include <string_view>

namespace mgpu::native_reflex
{
constexpr std::uint32_t set_by_code = 0x0d000000;
constexpr std::uint32_t priority_mask = 0xff000000;

struct variable
{
    int value = 0;
    std::uint32_t flags = 0;
};

enum class request_result { unchanged, applied, refused, rolled_back, rollback_failed };

inline int configured_mode(std::wstring_view enable, std::wstring_view mode, std::wstring_view automatic = {})
{
    if (enable == L"1" && mode == L"0" && automatic == L"0") return 0;
    if (enable == L"1" && mode == L"1") return 1;
    return -1;
}

inline bool compatible(variable value, int maximum, int desired, std::uint32_t flags = 0x20)
{
    return value.value >= 0 && value.value <= maximum &&
        (value.flags & ~priority_mask) == flags &&
        ((value.flags & priority_mask) <= set_by_code || value.value == desired);
}

template <typename Read, typename Write>
request_result request_off(Read read, Write write)
{
    const std::array<variable, 3> before{read(0), read(1), read(2)};
    constexpr std::array<int, 3> desired{1, 0, 0};
    constexpr std::array<unsigned, 3> order{2, 1, 0};
    if (!compatible(before[0], 1, 1) || !compatible(before[1], 2, 0) || !compatible(before[2], 1, 0, 4))
        return request_result::refused;
    const auto satisfied = [&](unsigned index, variable value)
    {
        return value.value == desired[index] && (value.flags & priority_mask) >= set_by_code;
    };
    std::array<bool, 3> changed{};
    bool any_change = false;
    for (unsigned index : order) {
        changed[index] = !satisfied(index, before[index]);
        if (changed[index]) { write(index, desired[index]); any_change = true; }
    }
    if (!any_change) return request_result::unchanged;
    if (satisfied(0, read(0)) && satisfied(1, read(1)) && satisfied(2, read(2))) return request_result::applied;
    for (unsigned index : {0u, 1u, 2u})
        if (changed[index]) write(index, before[index].value);
    return read(0).value == before[0].value && read(1).value == before[1].value && read(2).value == before[2].value
        ? request_result::rolled_back : request_result::rollback_failed;
}

template <typename Read, typename Write>
request_result request_mode(int desired, Read read, Write write)
{
    if (desired == 0) return request_off(read, write);
    if (desired != 0 && desired != 1) return request_result::refused;
    const variable before_enable = read(0);
    const variable before_mode = read(1);
    if (!compatible(before_enable, 1, desired) || !compatible(before_mode, 2, desired))
        return request_result::refused;
    const auto satisfied = [desired](variable value)
    {
        return value.value == desired && (value.flags & priority_mask) >= set_by_code;
    };
    const bool change_mode = !satisfied(before_mode);
    const bool change_enable = !satisfied(before_enable);
    if (!change_mode && !change_enable) return request_result::unchanged;
    if (change_mode) write(1, desired);
    if (change_enable) write(0, desired);
    if (satisfied(read(0)) && satisfied(read(1))) return request_result::applied;
    if (change_enable) write(0, before_enable.value);
    if (change_mode) write(1, before_mode.value);
    return read(0).value == before_enable.value && read(1).value == before_mode.value
        ? request_result::rolled_back : request_result::rollback_failed;
}

template <typename Read, typename Write>
request_result request_on(Read read, Write write)
{
    return request_mode(1, read, write);
}
}
