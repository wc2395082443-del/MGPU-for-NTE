#pragma once

#include "source_lead.hpp"
#include <windows.h>
#include <atomic>
#include <chrono>

namespace mgpu::secondary_fg
{
class source_gate
{
public:
    bool initialize()
    {
        if (event_ == nullptr) event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event_ != nullptr;
    }
    void reset()
    {
        disable();
        completed_.store(0);
        presented_.store(0);
        stalled_.store(false);
        consecutive_stalls_ = 0;
    }
    void disable()
    {
        enabled_.store(false, std::memory_order_release);
        if (event_ != nullptr) SetEvent(event_);
    }
    void complete(unsigned frame)
    {
        completed_.store(known_frame | frame, std::memory_order_release);
        if (event_ != nullptr) SetEvent(event_);
    }
    void enable()
    {
        if (event_ != nullptr && !stalled_.load()) enabled_.store(true, std::memory_order_release);
    }
    bool enabled() const { return enabled_.load(std::memory_order_acquire); }
    std::uint64_t completed() const { return completed_.load(std::memory_order_acquire); }
    std::uint64_t presented() const { return presented_.load(std::memory_order_acquire); }
    void presented(unsigned frame)
    {
        presented_.store(known_frame | frame, std::memory_order_release);
        if (event_ != nullptr) SetEvent(event_);
    }
    std::uint64_t progress() const
    {
        return completed();
    }
    unsigned wait(std::uint64_t submitted)
    {
        if (!enabled() || !source_needs_wait(submitted, progress())) return 0;
        if (waiting_.test_and_set(std::memory_order_acquire)) return 0;
        struct wait_scope
        {
            std::atomic_flag &flag;
            ~wait_scope() { flag.clear(std::memory_order_release); }
        } scope{waiting_};
        const auto start = std::chrono::steady_clock::now();
        const auto elapsed_ms = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
        };
        ++waits_;
        while (enabled() && source_needs_wait(submitted, progress()))
        {
            const auto elapsed = elapsed_ms();
            if (elapsed >= source_wait_limit_ms)
            {
                ++timeouts_;
                ++consecutive_stalls_;
                if (consecutive_stalls_ >= 3)
                {
                    stalled_.store(true);
                    disable();
                }
                break;
            }
            const auto remaining = static_cast<DWORD>(source_wait_limit_ms - elapsed);
            if (WaitForSingleObject(event_, remaining) == WAIT_FAILED)
            {
                stalled_.store(true);
                disable();
                break;
            }
        }
        if (!source_needs_wait(submitted, progress())) consecutive_stalls_ = 0;
        const auto elapsed = elapsed_ms();
        wait_ms_.fetch_add(elapsed);
        return static_cast<unsigned>(elapsed);
    }
    std::uint64_t waits() const { return waits_.load(); }
    std::uint64_t wait_ms() const { return wait_ms_.load(); }
    std::uint64_t timeouts() const { return timeouts_.load(); }
    bool stalled() const { return stalled_.load(); }
private:
    HANDLE event_ = nullptr;
    std::atomic<bool> enabled_{false}, stalled_{false};
    std::atomic<std::uint64_t> completed_{0}, presented_{0}, waits_{0}, wait_ms_{0}, timeouts_{0};
    std::atomic<unsigned> consecutive_stalls_{0};
    std::atomic_flag waiting_ = ATOMIC_FLAG_INIT;
};
}
