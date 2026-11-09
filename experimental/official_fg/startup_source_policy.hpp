#pragma once

#include <cstdint>

namespace mgpu::official_fg
{
inline bool startup_source_expired(std::uint32_t frame, std::uint32_t cached_owner,
    unsigned history_size, std::uint64_t real_presents, unsigned skipped, std::uint64_t elapsed_ms)
{
    if (!frame || !cached_owner || !history_size || real_presents != 0 || skipped >= 8 || elapsed_ms >= 20000)
        return false;
    const auto age = cached_owner - frame;
    return age >= history_size && age < 0x80000000u && frame % history_size == cached_owner % history_size;
}
}
