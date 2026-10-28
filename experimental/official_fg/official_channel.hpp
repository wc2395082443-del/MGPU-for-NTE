#pragma once

#include <windows.h>
#include "shared_frame_contract.hpp"
#include "official_metrics.hpp"
#include "reflex_contract.hpp"

namespace mgpu::official_fg
{
constexpr unsigned marker_capacity = 256;
constexpr unsigned marker_history = 64;
constexpr std::uint32_t channel_version = 3;

enum class host_state : LONG { starting, ready, failed, stopped };

struct source_marker
{
    std::uint32_t frame = 0, kind = 0;
    std::uint64_t serial = 0, capture_ns = 0;
    std::uint64_t native_begin_ns = 0, native_end_ns = 0;
};

struct source_frame_state
{
    std::uint32_t frame = 0, bits = 0;
    source_reflex_timing timing{};
};

class source_history
{
public:
    void retain(const std::array<std::uint32_t, shared_slots> &frames)
    {
        std::array<source_frame_state, shared_slots> next{};
        for (unsigned index = 0; index < frames.size(); ++index) {
            if (!frames[index]) continue;
            const auto previous = find(frames[index]);
            next[index] = previous.frame == frames[index] ? previous : source_frame_state{frames[index], 0};
        }
        retained_ = next;
    }
    bool note(std::uint32_t frame, std::uint32_t kind)
    {
        if (!frame || (kind > 3 && kind != sleep_marker)) return false;
        auto *selected = &entries_[frame % marker_history];
        for (auto &retained : retained_)
            if (retained.frame == frame) { selected = &retained; break; }
        auto &entry = *selected;
        if (entry.frame != frame) entry = {frame, 0};
        const unsigned bit = kind == sleep_marker ? 16u : 1u << kind;
        if ((entry.bits & bit) != 0) return false;
        if (kind == 0 && (entry.bits & 16u) == 0) return false;
        if (kind == 1 && (entry.bits & 1u) == 0) return false;
        if (kind == 2 && (entry.bits & 1u) == 0) return false;
        if (kind == 3 && (entry.bits & 4u) == 0) return false;
        entry.bits |= bit;
        if (entries_[frame % marker_history].frame == frame)
            entries_[frame % marker_history].bits = entry.bits;
        return true;
    }
    bool complete(std::uint32_t frame) const
    {
        const auto entry = find(frame);
        return entry.frame == frame && entry.bits == 31u;
    }
    bool simulation_ready(std::uint32_t frame) const
    {
        const auto entry = find(frame);
        return entry.frame == frame && (entry.bits & 19u) == 19u;
    }
    unsigned bits(std::uint32_t frame) const
    {
        const auto entry = find(frame);
        return entry.frame == frame ? entry.bits : 0;
    }
    unsigned cached_frame(std::uint32_t frame) const { return entries_[frame % marker_history].frame; }
    bool record_timing(std::uint32_t frame, unsigned kind, const marker_timing &timing)
    {
        const auto index = source_timing_index(kind);
        if (!frame || index >= 5) return false;
        auto *entry = &entries_[frame % marker_history];
        for (auto &retained : retained_)
            if (retained.frame == frame) { entry = &retained; break; }
        const unsigned bit = kind == sleep_marker ? 16u : 1u << kind;
        if (entry->frame != frame || !(entry->bits & bit)) return false;
        entry->timing.phases[index] = timing;
        if (entries_[frame % marker_history].frame == frame)
            entries_[frame % marker_history].timing = entry->timing;
        return true;
    }
    source_reflex_timing timing(std::uint32_t frame) const
    {
        const auto entry = find(frame);
        return entry.frame == frame ? entry.timing : source_reflex_timing{};
    }
    bool retained(std::uint32_t frame) const
    {
        for (const auto &entry : retained_) if (frame && entry.frame == frame) return true;
        return false;
    }
private:
    source_frame_state find(std::uint32_t frame) const
    {
        for (const auto &entry : retained_) if (frame && entry.frame == frame) return entry;
        return entries_[frame % marker_history];
    }
    std::array<source_frame_state, marker_history> entries_{};
    std::array<source_frame_state, shared_slots> retained_{};
};

struct official_channel
{
    std::uint32_t version = channel_version, byte_size = 0;
    shared_manifest manifest{};
    std::uint64_t parent_window = 0, parent_process = 0;
    std::uint64_t frames_event = 0, markers_event = 0, sleep_event = 0, stop_event = 0;
    std::uint32_t parent_pid = 0;
    volatile LONG state = static_cast<LONG>(host_state::starting);
    volatile LONG visible = 1, presenting = 0, multiplier = 6, maximum_multiplier = 0;
    alignas(8) volatile LONG64 published = 0, markers_written = 0, markers_read = 0, sleep_completed = 0;
    alignas(8) volatile LONG64 real_presents = 0, sdk_presents = 0, last_source_frame = 0, heartbeat_ms = 0;
    alignas(8) volatile LONG64 source_pings = 0;
    volatile LONG sdk_status = 0, marker_failures = 0, first_source_frame = 0;
    std::array<frame_packet, shared_slots> frames{};
    std::array<source_marker, marker_capacity> markers{};
    char error[256]{};
    metric_block current_metrics{}, last_visible_metrics{};
};

inline LONG read_atomic(volatile LONG &value) { return InterlockedCompareExchange(&value, 0, 0); }
inline LONG64 read_atomic(volatile LONG64 &value) { return InterlockedCompareExchange64(&value, 0, 0); }
inline HANDLE channel_handle(std::uint64_t value) { return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value)); }
inline std::uint64_t channel_integer(HANDLE value) { return reinterpret_cast<std::uintptr_t>(value); }

inline bool published_source_frames(official_channel &channel, std::array<std::uint32_t, shared_slots> &output)
{
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto latest = read_atomic(channel.published);
        if (latest < 0) return false;
        std::array<std::uint32_t, shared_slots> frames{};
        for (unsigned distance = 0; distance < shared_slots && static_cast<LONG64>(distance) < latest; ++distance) {
            const auto sequence = static_cast<std::uint64_t>(latest) - distance;
            const auto slot = slot_for(sequence);
            const auto packet = channel.frames[slot];
            if (packet.sequence == sequence && packet.generation == channel.manifest.generation)
                frames[slot] = packet.color_frame;
        }
        if (latest == read_atomic(channel.published)) { output = frames; return true; }
    }
    return false;
}
}
