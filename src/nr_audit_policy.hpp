#pragma once

#include <array>
#include <cstdint>

namespace mgpu::nr_audit
{
class budget
{
public:
    bool observe(std::uint64_t token, std::uint64_t now_ms)
    {
        if (!token || token == token_) return false;
        token_ = token;
        started_ms_ = now_ms;
        recorded_ = 0;
        active_ = true;
        return true;
    }
    bool active(std::uint64_t now_ms)
    {
        if (now_ms < started_ms_ || now_ms - started_ms_ >= duration_ms || recorded_ >= max_frames)
            active_ = false;
        return active_;
    }
    bool take(std::uint64_t now_ms)
    {
        if (!active(now_ms)) return false;
        ++recorded_;
        return true;
    }
    unsigned recorded() const { return recorded_; }
    std::uint64_t token() const { return token_; }
    static constexpr unsigned max_frames = 600;
    static constexpr std::uint64_t duration_ms = 20000;
private:
    std::uint64_t token_ = 0, started_ms_ = 0;
    unsigned recorded_ = 0;
    bool active_ = false;
};

inline std::array<double, 3> rgb(std::uint32_t pixel, unsigned bits, bool blue_first)
{
    const auto mask = (1u << bits) - 1u;
    const auto first = (pixel & mask) / double(mask);
    const auto green = ((pixel >> bits) & mask) / double(mask);
    const auto last = ((pixel >> (bits * 2)) & mask) / double(mask);
    return blue_first ? std::array<double, 3>{last, green, first} : std::array<double, 3>{first, green, last};
}

inline double brightness(std::uint32_t pixel, unsigned bits, bool blue_first)
{
    const auto values = rgb(pixel, bits, blue_first);
    return 0.2126 * values[0] + 0.7152 * values[1] + 0.0722 * values[2];
}
}
