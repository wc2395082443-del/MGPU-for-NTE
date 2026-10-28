#pragma once

#include <array>
#include <cstdint>

namespace mgpu::neural_history
{
constexpr unsigned max_passes = 2;
enum change : unsigned { none = 0, initial = 1, passes = 2, intensity = 4, tuning = 8 };

struct configuration
{
    unsigned active_passes = 1;
    std::array<float, max_passes> strengths{};
    bool tuning_enabled = false;
    float tone = 1, structure = 1, skin = 1, style = 0;
    bool auto_mask = false;
};

class history
{
public:
    unsigned changes(const configuration &next) const
    {
        if (!valid_) return initial;
        unsigned reason = next.active_passes != completed_.active_passes ? passes : none;
        for (unsigned index = 0; index < next.active_passes && index < max_passes; ++index)
            if (next.strengths[index] != completed_.strengths[index]) reason |= intensity;
        if (next.tuning_enabled != completed_.tuning_enabled ||
            (next.tuning_enabled && (next.tone != completed_.tone ||
             next.structure != completed_.structure || next.skin != completed_.skin ||
             next.style != completed_.style || next.auto_mask != completed_.auto_mask))) reason |= tuning;
        return reason;
    }

    void complete(const configuration &finished)
    {
        if (changes(finished) != none) ++revision_;
        completed_ = finished;
        valid_ = true;
    }

    std::uint64_t revision() const { return revision_; }

private:
    configuration completed_{};
    bool valid_ = false;
    std::uint64_t revision_ = 0;
};
}
