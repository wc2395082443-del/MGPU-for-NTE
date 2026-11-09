#pragma once

#include <cstdint>
#include <limits>

namespace mgpu::official_fg
{
class output_liveness
{
public:
    bool complete(std::uint64_t sequence, std::uint64_t now_ms)
    {
        if (!sequence || sequence <= sequence_) return false;
        if (!sequence_ || now_ms < completed_ms_ || now_ms - completed_ms_ > stale_ms)
            invalidate();
        sequence_ = sequence;
        completed_ms_ = now_ms;
        if (consecutive_ < recovery_frames) ++consecutive_;
        return true;
    }

    bool visible(std::uint64_t now_ms)
    {
        if (!sequence_ || now_ms < completed_ms_ || now_ms - completed_ms_ > stale_ms)
            invalidate();
        return consecutive_ >= recovery_frames;
    }

    void invalidate() { consecutive_ = 0; }
    std::uint64_t sequence() const { return sequence_; }
    std::uint64_t completed_ms() const { return completed_ms_; }
    std::uint64_t expires_ms() const
    {
        return completed_ms_ > std::numeric_limits<std::uint64_t>::max() - stale_ms
            ? std::numeric_limits<std::uint64_t>::max() : completed_ms_ + stale_ms;
    }
    static constexpr std::uint64_t stale_ms = 250;
    static constexpr unsigned recovery_frames = 3;

private:
    std::uint64_t sequence_ = 0, completed_ms_ = 0;
    unsigned consecutive_ = 0;
};
}
