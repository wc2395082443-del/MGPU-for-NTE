#pragma once

#include <cstdint>

namespace mgpu::fgmap
{
struct captured_policy
{
    bool verified = false;
    bool generated = false;
    float motion_fraction = 1.0f;
    unsigned reuse_seals = 0;
};

struct evaluated_outputs
{
    std::uint64_t input = 0;
    std::uint64_t real = 0;
    std::uint64_t interpolated = 0;
    unsigned count = 0;
    unsigned index = 0;
    bool succeeded = false;
    bool rendering = false;
};

class present_evidence
{
public:
    void reset()
    {
        outputs_ = {};
        age_ = 0;
        generated_seen_ = false;
    }

    void evaluated(const evaluated_outputs &outputs)
    {
        reset();
        outputs_ = outputs;
    }

    captured_policy capture(std::uint64_t resource)
    {
        if (age_ < 32u) ++age_;
        const unsigned window = outputs_.count <= 8u ? 2u * (outputs_.count + 1u) : 0u;
        if (!outputs_.succeeded || !outputs_.rendering || outputs_.count == 0u ||
            outputs_.index == 0u || outputs_.index > outputs_.count || age_ > window ||
            resource == 0 || outputs_.interpolated == 0 ||
            outputs_.interpolated == outputs_.input || outputs_.interpolated == outputs_.real)
        {
            generated_seen_ = false;
            return {};
        }

        const bool generated = resource == outputs_.interpolated;
        const bool real = resource == outputs_.real || resource == outputs_.input;
        if (!generated && !real)
        {
            generated_seen_ = false;
            return {};
        }
        if (generated) generated_seen_ = true;
        if (!generated_seen_) return {};
        return {true, generated, 1.0f / static_cast<float>(outputs_.count + 1u), window};
    }

private:
    evaluated_outputs outputs_{};
    unsigned age_ = 0;
    bool generated_seen_ = false;
};
}
