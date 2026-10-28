#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mgpu::tagged_motion
{
constexpr std::uint32_t pcl_cache_rva = 0xfd376b0;
constexpr std::uint32_t pcl_resolver_rva = 0x678ccc8;
constexpr std::uint32_t pcl_caller_rva = 0x678ce69;
constexpr unsigned char pcl_resolver_bytes[] = {0x4c, 0x8d, 0x05, 0xe1, 0xa9, 0x5a, 0x09};
constexpr unsigned char pcl_caller_bytes[] = {0xff, 0x15, 0x41, 0xa8, 0x5a, 0x09};

inline bool pcl_marker_route_matches(std::uint32_t timestamp, std::uint32_t image_size,
                                     const void *resolver, const void *caller)
{
    return timestamp == 0xa463911e && image_size == 0x10d43000 && resolver && caller &&
        std::memcmp(resolver, pcl_resolver_bytes, sizeof pcl_resolver_bytes) == 0 &&
        std::memcmp(caller, pcl_caller_bytes, sizeof pcl_caller_bytes) == 0;
}

inline void **select_cached_slot(void **unique, void **verified, void *target)
{
    if (verified && target && *verified == target) return verified;
    return unique;
}
}
