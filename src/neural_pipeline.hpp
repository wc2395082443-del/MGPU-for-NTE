#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include "neural_history.hpp"
#include "pipeline_timing.hpp"

namespace mgpu::neural_pipeline
{
enum class completion { idle, waiting, ready, failed };
struct job
{
    std::uint64_t seal = 0, fence = 0, submitted_ms = 0;
    unsigned ring_slot = 0, input_index = 0, snapshot = 0;
    unsigned engine_frame = 0;
    bool copy_queue = false;
    neural_history::configuration settings{};
    unsigned settings_change = 0, reset_pass_mask = 0, bound_strength_mask = 0;
    std::array<float, neural_history::max_passes> bound_strengths{};
    std::array<float, 2> bound_motion_scale{};
    unsigned motion_width = 0, motion_height = 0;
    bool motion_binding_valid = false;
    timing::calibration copy_clock{}, neural_clock{};
    std::uint64_t measurement_session = 0;
    double measurement_submitted_ms = -1;
};

class schedule
{
public:
    static constexpr unsigned SLOTS = 3;
    bool pending() const { return count_ != 0; }
    bool ready() const { return ready_count_ != 0; }
    unsigned ready_count() const { return ready_count_; }
    const job &ready_job() const { return ready_jobs_[ready_head_]; }
    const job &newest_ready_job() const { return ready_jobs_[(ready_head_ + ready_count_ - 1u) % SLOTS]; }
    bool discard_oldest_ready()
    {
        if (ready_count_ < 2) return false;
        slot_busy_[ready_job().snapshot] = false;
        ready_jobs_[ready_head_] = {};
        ready_head_ = (ready_head_ + 1u) % SLOTS;
        --ready_count_;
        return true;
    }
    bool has_capacity() const { return count_ < SLOTS && write_slot() < SLOTS; }
    unsigned inflight() const { return count_; }
    bool displayed() const { return display_seal_ != 0; }
    const job &current() const { return jobs_[head_]; }
    const job &latest() const { return jobs_[(head_ + count_ - 1u) % SLOTS]; }
    bool all_complete(std::uint64_t fence) const
    {
        return fence != UINT64_MAX && (!pending() || fence >= latest().fence);
    }
    bool owns_input(unsigned index) const
    {
        for (unsigned offset = 0; offset < count_; ++offset)
            if (jobs_[(head_ + offset) % SLOTS].input_index == index) return true;
        return false;
    }
    unsigned display_slot() const { return display_slot_; }
    std::uint64_t display_seal() const { return display_seal_; }
    std::uint64_t scheduled_seal() const { return scheduled_seal_; }
    unsigned write_slot() const
    {
        if (reserved_slot_ < SLOTS && !slot_busy_[reserved_slot_] &&
            (!displayed() || reserved_slot_ != display_slot_))
            return reserved_slot_;
        for (unsigned slot = 0; slot < SLOTS; ++slot)
            if (!slot_busy_[slot] && (!displayed() || slot != display_slot_)) return slot;
        return SLOTS;
    }
    std::uint64_t reserved_seal() const { return reserved_seal_; }
    bool reserve(unsigned slot, std::uint64_t seal)
    {
        if (reserved_slot_ != SLOTS || slot >= SLOTS || slot != write_slot() ||
            seal == 0 || seal == UINT64_MAX || seal != scheduled_seal_ + 1u) return false;
        reserved_slot_ = slot;
        reserved_seal_ = seal;
        return true;
    }
    void cancel_reservation()
    {
        reserved_slot_ = SLOTS;
        reserved_seal_ = 0;
    }
    bool begin(const job &next)
    {
        if (!has_capacity() || next.seal == 0 || next.fence == 0 || next.fence == UINT64_MAX ||
            next.seal <= scheduled_seal_ || next.fence <= scheduled_fence_ ||
            next.snapshot >= SLOTS || next.input_index != next.snapshot || owns_input(next.input_index) ||
            slot_busy_[next.snapshot] || (displayed() && next.snapshot == display_slot_) ||
            next.snapshot != write_slot() ||
            (reserved_seal_ != 0 && next.seal != reserved_seal_)) return false;
        if (count_ != 0 && next.seal <= jobs_[(head_ + count_ - 1u) % SLOTS].seal) return false;
        jobs_[(head_ + count_) % SLOTS] = next;
        slot_busy_[next.snapshot] = true;
        cancel_reservation();
        scheduled_seal_ = next.seal;
        scheduled_fence_ = next.fence;
        ++count_;
        return true;
    }
    completion check(std::uint64_t completed_fence) const
    {
        if (!pending()) return completion::idle;
        if (completed_fence == std::numeric_limits<std::uint64_t>::max()) return completion::failed;
        return completed_fence >= current().fence ? completion::ready : completion::waiting;
    }
    bool retire(std::uint64_t completed_fence)
    {
        if (check(completed_fence) != completion::ready || ready_count_ >= SLOTS) return false;
        const job finished = current();
        ready_jobs_[(ready_head_ + ready_count_) % SLOTS] = finished;
        ++ready_count_;
        jobs_[head_] = {};
        head_ = (head_ + 1u) % SLOTS;
        --count_;
        return true;
    }
    bool publish_ready()
    {
        if (!ready()) return false;
        const job finished = ready_job();
        if (displayed()) slot_busy_[display_slot_] = false;
        display_seal_ = finished.seal;
        display_slot_ = finished.snapshot;
        slot_busy_[display_slot_] = true;
        ready_jobs_[ready_head_] = {};
        ready_head_ = (ready_head_ + 1u) % SLOTS;
        --ready_count_;
        return true;
    }
    bool publish(std::uint64_t completed_fence)
    {
        return !ready() && retire(completed_fence) && publish_ready();
    }
private:
    std::array<job, SLOTS> jobs_{};
    std::array<job, SLOTS> ready_jobs_{};
    std::array<bool, SLOTS> slot_busy_{};
    unsigned head_ = 0, count_ = 0;
    unsigned ready_head_ = 0, ready_count_ = 0;
    unsigned reserved_slot_ = SLOTS;
    std::uint64_t reserved_seal_ = 0;
    std::uint64_t scheduled_seal_ = 0;
    std::uint64_t scheduled_fence_ = 0;
    std::uint64_t display_seal_ = 0;
    unsigned display_slot_ = 0;
};
}
