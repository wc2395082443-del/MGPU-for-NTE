#pragma once

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

namespace mgpu::official_fg
{
struct source_admission_result
{
    bool waited = false;
    bool timed_out = false;
    bool cancelled = false;
    bool ready = false;
    unsigned pending = 0;
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
            completed_known_ = false;
            completed_frame_ = 0;
            waits_ = 0;
            pending_.clear();
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
        if (reached(completed_frame_, frame) ||
            (submitted_known_ && !newer(frame, submitted_frame_))) {
            result.pending = static_cast<unsigned>(pending_.size());
            return result;
        }
        pending_.push_back(frame);
        submitted_frame_ = frame;
        submitted_known_ = true;
        result.pending = static_cast<unsigned>(pending_.size());
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
                wake_.notify_all();
                break;
            }
        }
        result.wait_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
        if (result.waited && generation == generation_) result.waits = ++waits_;
        result.completed = completed_frame_;
        result.pending = static_cast<unsigned>(pending_.size());
        result.cancelled = generation != generation_ || (!enabled_ && !result.timed_out);
        return result;
    }

    void complete(std::uint32_t frame)
    {
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (!enabled_) return;
            if (!completed_known_ || newer(frame, completed_frame_)) {
                completed_frame_ = frame;
                completed_known_ = true;
            }
            while (!pending_.empty() && reached(completed_frame_, pending_.front())) pending_.pop_front();
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
        result.pending = static_cast<unsigned>(pending_.size());
        result.ready = enabled_ && completed_known_;
        if (!result.ready || (input_known_ && !newer(frame, input_frame_))) return result;
        input_known_ = true;
        input_frame_ = frame;
        const unsigned input_limit = capacity > 1 ? capacity - 1 : 1;
        if (pending_.size() < input_limit) return result;
        result.target = pending_.front();
        const auto started = std::chrono::steady_clock::now();
        const auto generation = generation_;
        const auto deadline = started + timeout;
        while (enabled_ && generation == generation_ && pending_.size() >= input_limit) {
            result.waited = true;
            if (!wake_.wait_until(lock, deadline, [&] {
                return !enabled_ || generation != generation_ || pending_.size() < input_limit;
            })) {
                result.timed_out = true;
                enabled_ = false;
                wake_.notify_all();
                break;
            }
        }
        result.wait_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
        result.completed = completed_frame_;
        result.pending = static_cast<unsigned>(pending_.size());
        result.cancelled = generation != generation_ || (!enabled_ && !result.timed_out);
        return result;
    }

private:
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
    bool completed_known_ = false;
    std::uint32_t completed_frame_ = 0;
    std::uint64_t waits_ = 0;
    std::uint64_t generation_ = 0;
    std::deque<std::uint32_t> pending_;
    bool submitted_known_ = false;
    std::uint32_t submitted_frame_ = 0;
    bool input_known_ = false;
    std::uint32_t input_frame_ = 0;
};
}
