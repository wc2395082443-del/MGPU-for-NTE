#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace mgpu::secondary_fg
{
inline constexpr unsigned min_multiplier = 2;
inline constexpr unsigned max_multiplier = 6;
inline constexpr unsigned max_generated_frames = max_multiplier - 1;

constexpr unsigned clamp_multiplier(unsigned multiplier)
{
    return multiplier < min_multiplier ? min_multiplier
         : multiplier > max_multiplier ? max_multiplier
                                       : multiplier;
}

constexpr unsigned generated_frame_count(unsigned multiplier)
{
    return clamp_multiplier(multiplier) - 1;
}

constexpr unsigned supported_generated_frames(bool reported, unsigned maximum)
{
    return !reported || maximum < 1u ? 1u
        : maximum > max_generated_frames ? max_generated_frames : maximum;
}

constexpr bool supports_multiplier(unsigned multiplier, unsigned maximum)
{
    return multiplier >= min_multiplier && multiplier <= max_multiplier &&
        multiplier - 1u <= maximum;
}

enum class rejection
{
    none,
    source_frame_generation,
    missing_neural_output,
    missing_guides,
    mismatched_frame,
    invalid_motion_scale,
    missing_camera,
    invalid_camera,
    invalid_timestamp,
    non_monotonic_frame
};

struct frame_contract
{
    std::uint64_t frame_id = 0;
    std::uint64_t depth_frame_id = 0;
    std::uint64_t motion_frame_id = 0;
    std::uint64_t camera_frame_id = 0;
    std::uint64_t capture_ns = 0;
    bool source_frame_generation = false;
    bool neural_output = false;
    bool depth_valid = false;
    bool motion_valid = false;
    bool camera_valid = false;
    bool camera_values_valid = false;
    bool reset = false;
    float normalized_motion_scale_x = 0.0f;
    float normalized_motion_scale_y = 0.0f;
};

inline rejection validate(const frame_contract &frame)
{
    if (frame.source_frame_generation) return rejection::source_frame_generation;
    if (!frame.neural_output) return rejection::missing_neural_output;
    if (!frame.depth_valid || !frame.motion_valid) return rejection::missing_guides;
    if (frame.frame_id == 0 || frame.frame_id != frame.depth_frame_id ||
        frame.frame_id != frame.motion_frame_id)
        return rejection::mismatched_frame;
    if (frame.camera_valid && frame.frame_id != frame.camera_frame_id)
        return rejection::mismatched_frame;
    if (!std::isfinite(frame.normalized_motion_scale_x) ||
        !std::isfinite(frame.normalized_motion_scale_y) ||
        frame.normalized_motion_scale_x == 0.0f || frame.normalized_motion_scale_y == 0.0f)
        return rejection::invalid_motion_scale;
    if (frame.camera_valid && !frame.camera_values_valid) return rejection::invalid_camera;
    if (frame.capture_ns == 0) return rejection::invalid_timestamp;
    return rejection::none;
}

struct decision
{
    rejection reason = rejection::none;
    bool evaluate = false;
    bool reset = true;
    bool allow_generated = false;
    std::uint64_t frame_interval_ns = 0;
};

class frame_history
{
public:
    decision inspect(const frame_contract &frame) const
    {
        decision result;
        result.reason = validate(frame);
        if (result.reason != rejection::none) return result;
        if (last_frame_ != 0 &&
            (frame.frame_id <= last_frame_ || frame.capture_ns <= last_timestamp_))
        {
            result.reason = rejection::non_monotonic_frame;
            return result;
        }
        result.evaluate = true;
        if (last_frame_ == 0 || frame.reset || frame.frame_id - last_frame_ != 1) return result;
        const auto interval = frame.capture_ns - last_timestamp_;
        if (interval < 1000000 || interval > 100000000) return result;
        result.reset = false;
        result.allow_generated = true;
        result.frame_interval_ns = interval;
        return result;
    }

    void complete(const frame_contract &frame, bool succeeded)
    {
        if (!succeeded || validate(frame) != rejection::none)
        {
            clear();
            return;
        }
        last_frame_ = frame.frame_id;
        last_timestamp_ = frame.capture_ns;
    }

    void clear()
    {
        last_frame_ = 0;
        last_timestamp_ = 0;
    }

private:
    std::uint64_t last_frame_ = 0;
    std::uint64_t last_timestamp_ = 0;
};

enum class picture { none, interpolated, real };

class presentation_pair
{
public:
    bool enqueue(std::uint64_t frame_id, std::uint64_t now_ns,
                 const decision &result, bool gpu_completed, bool interpolation_disabled,
                 unsigned multiplier)
    {
        if (pending_ != picture::none || frame_id == 0 || now_ns == 0 || !gpu_completed ||
            multiplier < min_multiplier || multiplier > max_multiplier)
            return false;
        frame_id_ = frame_id;
        const bool generated = result.evaluate && result.allow_generated && !interpolation_disabled &&
                               result.frame_interval_ns != 0;
        generated_count_ = generated ? generated_frame_count(multiplier) : 0;
        frame_interval_ns_ = generated ? result.frame_interval_ns / (generated_count_ + 1u) : 0;
        if (generated && frame_interval_ns_ == 0) frame_interval_ns_ = 1;
        generated_index_ = 0;
        pending_ = generated ? picture::interpolated : picture::real;
        next_due_ns_ = generated && timeline_next_ns_ > now_ns ? timeline_next_ns_ : now_ns;
        const auto duration = generated ? result.frame_interval_ns : 0ull;
        if (next_due_ns_ > std::numeric_limits<std::uint64_t>::max() - duration)
        {
            clear();
            return false;
        }
        timeline_next_ns_ = next_due_ns_ + duration;
        return true;
    }

    picture next(std::uint64_t now_ns)
    {
        if (now_ns < next_due_ns_) return picture::none;
        while (pending_ == picture::interpolated && frame_interval_ns_ != 0 &&
               now_ns - next_due_ns_ >= frame_interval_ns_)
        {
            advance();
            ++dropped_generated_;
        }
        return pending_;
    }

    bool presented(picture shown, bool succeeded, std::uint64_t now_ns = 0)
    {
        if (!succeeded)
        {
            clear();
            return false;
        }
        if (shown == picture::none || shown != pending_) return false;
        if (shown == picture::interpolated)
        {
            if (now_ns != 0 && now_ns > next_due_ns_)
                late_ns_ += now_ns - next_due_ns_;
            advance();
            return true;
        }
        pending_ = picture::none;
        return true;
    }

    std::uint64_t real_due_ns() const { return next_due_ns_; }
    std::uint64_t frame_id() const { return frame_id_; }
    unsigned generated_index() const { return generated_index_; }
    unsigned generated_count() const { return generated_count_; }
    std::uint64_t dropped_generated() const { return dropped_generated_; }
    std::uint64_t late_ns() const { return late_ns_; }
    unsigned finish_with_real(std::uint64_t now_ns)
    {
        if (pending_ != picture::interpolated) return 0;
        const unsigned skipped = generated_count_ - generated_index_;
        dropped_generated_ += skipped;
        pending_ = picture::real;
        next_due_ns_ = now_ns;
        timeline_next_ns_ = now_ns;
        return skipped;
    }
    bool pending() const { return pending_ != picture::none; }
    void clear()
    {
        pending_ = picture::none;
        frame_id_ = 0;
        next_due_ns_ = 0;
        frame_interval_ns_ = 0;
        timeline_next_ns_ = 0;
        dropped_generated_ = 0;
        late_ns_ = 0;
        generated_index_ = 0;
        generated_count_ = 0;
    }

private:
    void advance()
    {
        if (generated_index_ + 1u < generated_count_) ++generated_index_;
        else pending_ = picture::real;
        next_due_ns_ += frame_interval_ns_;
    }
    picture pending_ = picture::none;
    std::uint64_t frame_id_ = 0;
    std::uint64_t next_due_ns_ = 0;
    std::uint64_t frame_interval_ns_ = 0;
    std::uint64_t timeline_next_ns_ = 0;
    std::uint64_t dropped_generated_ = 0;
    std::uint64_t late_ns_ = 0;
    unsigned generated_index_ = 0;
    unsigned generated_count_ = 0;
};
}
