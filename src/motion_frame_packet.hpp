#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mgpu::motion_packet
{
struct identity
{
    std::uint32_t frame = 0;
    std::uint32_t viewport = 0;
    bool frame_known = false;
    bool viewport_known = false;
};

struct packet
{
    std::uint64_t seal = 0;
    std::uint64_t source = 0;
    std::uintptr_t command = 0;
    identity engine{};
};

template <std::size_t Capacity>
class handoff
{
    static_assert(Capacity > 1);
    packet pending_{1};
    std::array<packet, Capacity> sealed_{};

public:
    void reset(std::uint64_t next_seal = 1)
    {
        pending_ = {next_seal};
        sealed_ = {};
    }

    const packet &pending() const { return pending_; }

    void recorded(std::uint64_t source, std::uintptr_t command, identity engine)
    {
        pending_.source = source;
        pending_.command = command;
        pending_.engine = engine;
    }

    packet close(std::uint64_t seal)
    {
        if (seal == 0 || seal != pending_.seal) return {};
        const packet result = pending_;
        sealed_[(seal - 1) % Capacity] = result;
        pending_ = {seal + 1};
        return result;
    }

    packet at(std::size_t slot, std::uint64_t seal) const
    {
        if (slot >= Capacity || seal == 0 || sealed_[slot].seal != seal) return {};
        return sealed_[slot];
    }

    bool valid(std::size_t slot, std::uint64_t seal) const
    {
        return at(slot, seal).source != 0;
    }
};
}
