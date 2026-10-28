#pragma once

#include <windows.h>
#include <array>
#include <cmath>
#include <cstdint>

namespace mgpu::official_fg
{
struct rate_snapshot
{
    std::uint64_t begin_ms = 0, end_ms = 0, real_delta = 0, sdk_delta = 0;
    double real_fps = 0, sdk_fps = 0, multiplier = 0, source_age_ms = -1;
    bool valid = false, visible = false;
};

class rate_meter
{
public:
    bool update(std::uint64_t now, std::uint64_t real, std::uint64_t sdk, bool visible, rate_snapshot &output)
    {
        if (!initialized_ || now < begin_ || real < real_ || sdk < sdk_ || visible != visible_) {
            begin_ = now;
            real_ = real;
            sdk_ = sdk;
            visible_ = visible;
            age_sum_ = 0;
            age_count_ = 0;
            initialized_ = true;
            output = {};
            return false;
        }
        const auto elapsed = now - begin_;
        if (elapsed < 1000) return false;
        output = {};
        output.begin_ms = begin_;
        output.end_ms = now;
        output.real_delta = real - real_;
        output.sdk_delta = sdk - sdk_;
        output.visible = visible;
        output.valid = elapsed <= 5000 && output.real_delta > 0;
        if (output.valid) {
            output.real_fps = double(output.real_delta) * 1000.0 / double(elapsed);
            output.sdk_fps = double(output.sdk_delta) * 1000.0 / double(elapsed);
            output.multiplier = double(output.sdk_delta) / double(output.real_delta);
            if (age_count_) output.source_age_ms = age_sum_ / double(age_count_);
        }
        begin_ = now;
        real_ = real;
        sdk_ = sdk;
        age_sum_ = 0;
        age_count_ = 0;
        return true;
    }
    void source_age(double milliseconds)
    {
        if (std::isfinite(milliseconds) && milliseconds >= 0 && milliseconds <= 60000) {
            age_sum_ += milliseconds;
            ++age_count_;
        }
    }
private:
    std::uint64_t begin_ = 0, real_ = 0, sdk_ = 0, age_count_ = 0;
    double age_sum_ = 0;
    bool initialized_ = false, visible_ = false;
};

struct metric_block
{
    volatile LONG64 sequence = 0;
    volatile LONG64 values[10]{};
};

inline void publish_metrics(metric_block &block, const rate_snapshot &sample)
{
    const std::array<LONG64, 10> values{{static_cast<LONG64>(sample.begin_ms), static_cast<LONG64>(sample.end_ms),
        static_cast<LONG64>(sample.real_delta), static_cast<LONG64>(sample.sdk_delta),
        static_cast<LONG64>(std::llround(sample.real_fps * 1000)), static_cast<LONG64>(std::llround(sample.sdk_fps * 1000)),
        static_cast<LONG64>(std::llround(sample.multiplier * 1000)),
        sample.source_age_ms < 0 ? -1 : static_cast<LONG64>(std::llround(sample.source_age_ms * 1000)),
        sample.valid ? 1 : 0, sample.visible ? 1 : 0}};
    InterlockedIncrement64(&block.sequence);
    for (unsigned index = 0; index < values.size(); ++index) InterlockedExchange64(&block.values[index], values[index]);
    InterlockedIncrement64(&block.sequence);
}

inline bool read_metrics(metric_block &block, rate_snapshot &sample)
{
    sample = {};
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto before = InterlockedCompareExchange64(&block.sequence, 0, 0);
        if (!before || (before & 1)) continue;
        std::array<LONG64, 10> values{};
        for (unsigned index = 0; index < values.size(); ++index) values[index] = InterlockedCompareExchange64(&block.values[index], 0, 0);
        if (InterlockedCompareExchange64(&block.sequence, 0, 0) != before) continue;
        if (values[0] < 0 || values[1] < values[0] || values[2] < 0 || values[3] < 0) return false;
        sample.begin_ms = values[0];
        sample.end_ms = values[1];
        sample.real_delta = values[2];
        sample.sdk_delta = values[3];
        sample.real_fps = double(values[4]) / 1000.0;
        sample.sdk_fps = double(values[5]) / 1000.0;
        sample.multiplier = double(values[6]) / 1000.0;
        sample.source_age_ms = values[7] < 0 ? -1 : double(values[7]) / 1000.0;
        sample.valid = values[8] == 1;
        sample.visible = values[9] == 1;
        return true;
    }
    return false;
}

inline bool fresh_metrics(const rate_snapshot &sample, std::uint64_t now, std::uint64_t maximum_age)
{
    return sample.valid && sample.end_ms && now >= sample.end_ms && now - sample.end_ms <= maximum_age;
}
}
