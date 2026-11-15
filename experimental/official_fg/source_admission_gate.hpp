#pragma once

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <algorithm>

namespace mgpu::official_fg
{
struct source_admission_result
{
    bool waited = false;
    bool timed_out = false;
    bool cancelled = false;
    bool ready = false;
    unsigned pending = 0;
    unsigned submitted = 0, reserved = 0;
    bool recovering = false;
    std::uint64_t wait_ms = 0;
    std::uint64_t waits = 0;
    std::uint32_t target = 0;
    std::uint32_t completed = 0;
};

class source_admission_gate
{
public:
    static constexpr unsigned capacity = 3;
    void reset(bool enabled)
    {
        {
            std::lock_guard<std::mutex> guard(lock_);
            enabled_ = enabled;
            timeout_suspended_ = false;
            completed_known_ = false;
            completed_frame_ = 0;
            waits_ = 0;
            pending_.clear();
            reserved_.clear();
            submitted_known_ = false;
            input_known_ = false;
            ++generation_;
        }
        wake_.notify_all();
    }

    source_admission_result wait_submitted(std::uint32_t frame,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(1000))
    {
        source_admission_result result{};
        std::unique_lock<std::mutex> lock(lock_);
        result.target = frame;
        result.completed = completed_frame_;
        result.ready = enabled_ && completed_known_;
        if (!result.ready) return result;
        reserved_.erase(std::remove(reserved_.begin(), reserved_.end(), frame), reserved_.end());
        if (reached(completed_frame_, frame) ||
            (submitted_known_ && !newer(frame, submitted_frame_))) {
            result.pending = outstanding();
            result.submitted = static_cast<unsigned>(pending_.size());
            result.reserved = static_cast<unsigned>(reserved_.size());
            return result;
        }
        pending_.push_back(frame);
        submitted_frame_ = frame;
        submitted_known_ = true;
        result.pending = outstanding();
        result.submitted = static_cast<unsigned>(pending_.size());
        result.reserved = static_cast<unsigned>(reserved_.size());
        if (pending_.size() < capacity) return result;
        result.target = pending_.front();
        const auto started = std::chrono::steady_clock::now();
        const auto generation = generation_;
        const auto deadline = started + timeout;
        while (enabled_ && generation == generation_ && pending_.size() >= capacity) {
            result.waited = true;
            if (!wake_.wait_until(lock, deadline, [&] {
                return !enabled_ || generation != generation_ || pending_.size() < capacity;
            })) {
                result.timed_out = true;
                enabled_ = false;
                timeout_suspended_ = true;
                wake_.notify_all();
                break;
            }
        }
        result.wait_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
        if (result.waited && generation == generation_) result.waits = ++waits_;
        result.completed = completed_frame_;
        result.pending = outstanding();
        result.cancelled = generation != generation_ || (!enabled_ && !result.timed_out);
        result.submitted = static_cast<unsigned>(pending_.size());
        result.reserved = static_cast<unsigned>(reserved_.size());
        return result;
    }

    void complete(std::uint32_t frame)
    {
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (!enabled_ && !timeout_suspended_) return;
            if (!completed_known_ || newer(frame, completed_frame_)) {
                completed_frame_ = frame;
                completed_known_ = true;
            }
            while (!pending_.empty() && reached(completed_frame_, pending_.front())) pending_.pop_front();
            reserved_.erase(std::remove_if(reserved_.begin(), reserved_.end(), [&](auto target) {
                return reached(completed_frame_, target);
            }), reserved_.end());
            if (timeout_suspended_ && outstanding() < capacity) {
                timeout_suspended_ = false;
                enabled_ = true;
                input_known_ = false;
                ++generation_;
            }
        }
        wake_.notify_all();
    }

    source_admission_result wait_before_input(std::uint32_t frame,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(1000))
    {
        source_admission_result result{};
        std::unique_lock<std::mutex> lock(lock_);
        result.target = frame;
        result.completed = completed_frame_;
        result.pending = outstanding();
        result.submitted = static_cast<unsigned>(pending_.size());
        result.reserved = static_cast<unsigned>(reserved_.size());
        result.recovering = timeout_suspended_;
        result.ready = enabled_ && completed_known_;
        if (!result.ready || (input_known_ && !newer(frame, input_frame_))) return result;
        input_known_ = true;
        input_frame_ = frame;
        const unsigned input_limit = capacity;
        if (outstanding() < input_limit) {
            if (!reached(completed_frame_, frame)) reserved_.push_back(frame);
            result.pending = outstanding();
            result.reserved = static_cast<unsigned>(reserved_.size());
            return result;
        }
        result.target = pending_.empty() ? reserved_.front() : pending_.front();
        const auto started = std::chrono::steady_clock::now();
        const auto generation = generation_;
        const auto deadline = started + timeout;
        while (enabled_ && generation == generation_ && outstanding() >= input_limit) {
            result.waited = true;
            if (!wake_.wait_until(lock, deadline, [&] {
                return !enabled_ || generation != generation_ || outstanding() < input_limit;
            })) {
                result.timed_out = true;
                enabled_ = false;
                timeout_suspended_ = true;
                wake_.notify_all();
                break;
            }
        }
        result.wait_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
        result.completed = completed_frame_;
        result.cancelled = generation != generation_ || (!enabled_ && !result.timed_out);
        if (!result.timed_out && !result.cancelled && !reached(completed_frame_, frame)) reserved_.push_back(frame);
        result.pending = outstanding();
        result.submitted = static_cast<unsigned>(pending_.size());
        result.reserved = static_cast<unsigned>(reserved_.size());
        return result;
    }

private:
    unsigned outstanding() const { return static_cast<unsigned>(pending_.size() + reserved_.size()); }
    static bool newer(std::uint32_t left, std::uint32_t right)
    {
        return left != right && static_cast<std::uint32_t>(left - right) < 0x80000000u;
    }

    static bool reached(std::uint32_t completed, std::uint32_t target)
    {
        return completed == target || newer(completed, target);
    }

    std::mutex lock_;
    std::condition_variable wake_;
    bool enabled_ = false;
    bool timeout_suspended_ = false;
    bool completed_known_ = false;
    std::uint32_t completed_frame_ = 0;
    std::uint64_t waits_ = 0;
    std::uint64_t generation_ = 0;
    std::deque<std::uint32_t> pending_;
    std::deque<std::uint32_t> reserved_;
    bool submitted_known_ = false;
    std::uint32_t submitted_frame_ = 0;
    bool input_known_ = false;
    std::uint32_t input_frame_ = 0;
};
}
