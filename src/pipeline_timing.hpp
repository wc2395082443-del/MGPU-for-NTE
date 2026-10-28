#pragma once

#include <cmath>
#include <cstdint>

namespace mgpu::timing
{
struct calibration
{
    std::uint64_t gpu = 0, cpu = 0, gpu_frequency = 0, cpu_frequency = 0;
    std::uint64_t query_before = 0, query_after = 0;

    double cpu_ms(std::uint64_t timestamp) const
    {
        if (gpu_frequency == 0 || cpu_frequency == 0 || cpu == 0) return -1;
        const double delta = timestamp >= gpu ? double(timestamp - gpu) : -double(gpu - timestamp);
        return double(cpu) * 1000.0 / double(cpu_frequency) + delta * 1000.0 / double(gpu_frequency);
    }
};

struct span
{
    double gpu_ms = -1, start_ms = -1, end_ms = -1;
};

inline span decode(std::uint64_t first, std::uint64_t last, const calibration &clock)
{
    if (first == 0 || last < first || clock.gpu_frequency == 0) return {};
    return {double(last - first) * 1000.0 / double(clock.gpu_frequency),
            clock.cpu_ms(first), clock.cpu_ms(last)};
}

inline double interval(double first, double last)
{
    return first >= 0 && last >= first ? last - first : -1;
}

inline double overlap(const span &left, const span &right)
{
    if (!std::isfinite(left.start_ms) || !std::isfinite(left.end_ms) ||
        !std::isfinite(right.start_ms) || !std::isfinite(right.end_ms) ||
        left.start_ms < 0 || right.start_ms < 0 ||
        left.end_ms < left.start_ms || right.end_ms < right.start_ms) return -1;
    const double start = left.start_ms > right.start_ms ? left.start_ms : right.start_ms;
    const double end = left.end_ms < right.end_ms ? left.end_ms : right.end_ms;
    return end > start ? end - start : 0;
}

struct metric
{
    std::uint64_t count = 0, invalid = 0;
    double sum = 0, maximum = 0;

    void add(double value)
    {
        if (!std::isfinite(value) || value < 0) { ++invalid; return; }
        ++count;
        sum += value;
        if (value > maximum) maximum = value;
    }

    double mean() const { return count != 0 ? sum / double(count) : -1; }
};
}
