#pragma once

#include <array>
#include <cstdint>

namespace mgpu::admission
{
enum reason : unsigned
{
    no_command_record = 1u << 0, invalid_resource = 1u << 1, invalid_descriptor = 1u << 2,
    failed_context = 1u << 3, wrong_device = 1u << 4, wrong_viewport = 1u << 5,
    already_delivered = 1u << 6, reserved_tuple = 1u << 7, no_capture_slot = 1u << 8,
    allocation_failed = 1u << 9, missing_constants = 1u << 10, missing_submission = 1u << 11,
    missing_normalization = 1u << 12, waiting_color = 1u << 13,
    copy_queue_busy = 1u << 14, transfer_failed = 1u << 15,
    incomplete_reclaimed = 1u << 16, stale_complete_reclaimed = 1u << 17, invalid_tag = 1u << 18,
};

struct row
{
    std::uint32_t frame = 0, viewport = 0;
    unsigned seen = 0, recorded = 0, reasons = 0, held_calls = 0;
    bool constants_seen = false, constants_valid_seen = false, acquired = false, delivered = false;
    std::uint64_t seal = 0, first_ms = 0, last_ms = 0;
};

class trace
{
public:
    static constexpr unsigned capacity = 2048;
    static constexpr std::uint64_t duration_ms = 20000;
    bool begin(std::uint64_t token, std::uint64_t now)
    {
        if (!token || token == token_ || active(now) || flushing_) return false;
        token_ = token;
        started_ = now;
        rows_ = {};
        count_ = cursor_ = overflow_ = 0;
        active_ = true;
        return true;
    }
    bool active(std::uint64_t now)
    {
        if (active_ && (now < started_ || now - started_ >= duration_ms)) {
            active_ = false;
            flushing_ = true;
        }
        return active_;
    }
    row *note(std::uint32_t frame, std::uint32_t viewport, std::uint64_t now)
    {
        if (!active(now) || !frame) return nullptr;
        auto &entry = rows_[frame % capacity];
        if (entry.frame && (entry.frame != frame || entry.viewport != viewport)) {
            ++overflow_;
            return nullptr;
        }
        if (!entry.frame) {
            entry.frame = frame;
            entry.viewport = viewport;
            entry.first_ms = now;
            ++count_;
        }
        entry.last_ms = now;
        return &entry;
    }
    bool pop(row &result)
    {
        if (!flushing_) return false;
        while (cursor_ < capacity) {
            const auto &entry = rows_[cursor_++];
            if (entry.frame) { result = entry; return true; }
        }
        flushing_ = false;
        return false;
    }
    bool flushing() const { return flushing_; }
    unsigned count() const { return count_; }
    unsigned overflow() const { return overflow_; }
    std::uint64_t token() const { return token_; }
private:
    std::array<row, capacity> rows_{};
    std::uint64_t token_ = 0, started_ = 0;
    unsigned count_ = 0, cursor_ = 0, overflow_ = 0;
    bool active_ = false, flushing_ = false;
};
}
