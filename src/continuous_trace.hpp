#pragma once

#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <share.h>
#include <string>
#include <thread>
#include <type_traits>

namespace mgpu::trace
{
struct stream_header
{
    std::uint64_t magic = 0x325254535550474dull;
    std::uint64_t version = 1, record_bytes = 0, session = 0, frequency = 0;
    std::uint64_t begin = 0, end = 0, records = 0, dropped = 0, failures = 0;
};
static_assert(sizeof(stream_header) == 80);

inline std::uint64_t ticks()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

template<typename Record, unsigned Capacity = 8192>
class continuous_stream
{
    static_assert(std::is_trivially_copyable_v<Record>);
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
    struct cell
    {
        std::atomic<std::uint64_t> sequence{0};
        Record value{};
    };
    std::unique_ptr<std::array<cell, Capacity>> ring_;
    std::atomic<std::uint64_t> head_{0}, dropped_{0}, writers_{0}, written_{0}, failures_{0};
    std::atomic<bool> accepting_{false}, closing_{false}, done_{false}, attempted_{false};
    std::thread worker_;
    std::filesystem::path path_;
    stream_header header_{};

    void write_loop()
    {
        const auto partial = path_.wstring() + L".partial";
        FILE *file = _wfsopen(partial.c_str(), L"w+b", _SH_DENYNO);
        if (!file)
        {
            ++failures_;
            accepting_.store(false, std::memory_order_release);
            done_.store(true, std::memory_order_release);
            return;
        }
        char buffer[65536];
        std::setvbuf(file, buffer, _IOFBF, sizeof buffer);
        bool valid = std::fwrite(&header_, sizeof header_, 1, file) == 1;
        std::uint64_t tail = 0;
        while (valid)
        {
            unsigned batch = 0;
            while (batch < 1024)
            {
                auto &slot = (*ring_)[tail & (Capacity - 1)];
                if (slot.sequence.load(std::memory_order_acquire) != tail + 1) break;
                if (std::fwrite(&slot.value, sizeof(Record), 1, file) != 1)
                {
                    valid = false;
                    break;
                }
                slot.sequence.store(tail + Capacity, std::memory_order_release);
                ++tail;
                ++batch;
            }
            written_.store(tail, std::memory_order_release);
            if (!valid) break;
            if (closing_.load(std::memory_order_acquire) &&
                writers_.load(std::memory_order_acquire) == 0 &&
                head_.load(std::memory_order_acquire) == tail) break;
            if (batch == 0) Sleep(2);
        }
        accepting_.store(false, std::memory_order_release);
        while (writers_.load(std::memory_order_acquire) != 0) Sleep(1);
        if (!valid) ++failures_;
        header_.end = ticks();
        header_.records = tail;
        header_.dropped = dropped_.load(std::memory_order_acquire);
        header_.failures = failures_.load(std::memory_order_acquire);
        if (valid)
            valid = _fseeki64(file, 0, SEEK_SET) == 0 &&
                std::fwrite(&header_, sizeof header_, 1, file) == 1 && std::fflush(file) == 0;
        if (std::fclose(file) != 0) valid = false;
        if (valid && !MoveFileExW(partial.c_str(), path_.c_str(), 0)) valid = false;
        if (!valid) ++failures_;
        done_.store(true, std::memory_order_release);
    }

public:
    continuous_stream() = default;
    continuous_stream(const continuous_stream &) = delete;
    continuous_stream &operator=(const continuous_stream &) = delete;
    ~continuous_stream()
    {
        finish();
        if (worker_.joinable()) worker_.join();
    }

    bool start(const std::filesystem::path &path, std::uint64_t session)
    {
        if (attempted_.exchange(true, std::memory_order_acq_rel)) return false;
        try
        {
            if (path.empty() || !session || std::filesystem::exists(path) ||
                std::filesystem::exists(path.wstring() + L".partial")) return false;
            ring_ = std::make_unique<std::array<cell, Capacity>>();
            for (unsigned index = 0; index < Capacity; ++index)
                (*ring_)[index].sequence.store(index, std::memory_order_relaxed);
            path_ = path;
            header_.record_bytes = sizeof(Record);
            header_.session = session;
            header_.begin = ticks();
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            header_.frequency = frequency.QuadPart;
            accepting_.store(true, std::memory_order_release);
            worker_ = std::thread([this] { write_loop(); });
            return true;
        }
        catch (...)
        {
            ++failures_;
            accepting_.store(false, std::memory_order_release);
            done_.store(true, std::memory_order_release);
            return false;
        }
    }

    bool push(const Record &value)
    {
        if (!accepting_.load(std::memory_order_acquire)) return false;
        writers_.fetch_add(1, std::memory_order_acq_rel);
        if (!accepting_.load(std::memory_order_acquire))
        {
            writers_.fetch_sub(1, std::memory_order_release);
            return false;
        }
        auto position = head_.load(std::memory_order_relaxed);
        for (;;)
        {
            auto &slot = (*ring_)[position & (Capacity - 1)];
            const auto sequence = slot.sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::int64_t>(sequence - position);
            if (difference < 0)
            {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                writers_.fetch_sub(1, std::memory_order_release);
                return false;
            }
            if (difference == 0 && head_.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
            {
                slot.value = value;
                slot.sequence.store(position + 1, std::memory_order_release);
                writers_.fetch_sub(1, std::memory_order_release);
                return true;
            }
            position = head_.load(std::memory_order_relaxed);
        }
    }

    void finish()
    {
        accepting_.store(false, std::memory_order_release);
        closing_.store(true, std::memory_order_release);
    }
    bool done() const { return done_.load(std::memory_order_acquire); }
    bool accepting() const { return accepting_.load(std::memory_order_acquire); }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_acquire); }
    std::uint64_t written() const { return written_.load(std::memory_order_acquire); }
    std::uint64_t failures() const { return failures_.load(std::memory_order_acquire); }
    void reject() { dropped_.fetch_add(1, std::memory_order_relaxed); }
};
}
