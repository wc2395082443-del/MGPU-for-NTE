#pragma once

#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>
#include <share.h>
#include <string>
#include "reflex_contract.hpp"
#include "shared_frame_contract.hpp"
#include "../../src/continuous_trace.hpp"

namespace mgpu::official_fg
{
enum class output_stage : unsigned
{
    acquired, markers_ready, slot_ready, osd_begin, osd_end, recorded, submitted, tagged,
    present_returned, state_read, finished, count
};

struct output_frame_trace
{
    std::uint64_t sequence = 0, source = 0, capture_ns = 0, backlog = 0;
    std::int64_t input_poll_wait_ticks = 0;
    unsigned visible = 0, requested = 0;
    source_reflex_timing reflex{};
    std::uint64_t helper_render_begin_ns = 0, helper_render_end_ns = 0, source_pings = 0;
    std::array<std::int64_t, static_cast<unsigned>(output_stage::count)> ticks{};
    camera_packet camera{};
    std::uint64_t sdk_reported = 0, sdk_status = 0, present_count = 0, present_result = 0;
    void mark(output_stage stage)
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        ticks[static_cast<unsigned>(stage)] = now.QuadPart;
    }
    bool ordered() const
    {
        if (!sequence || !ticks[0]) return false;
        for (unsigned index = 1; index < ticks.size(); ++index)
            if (ticks[index] < ticks[index - 1]) return false;
        return true;
    }
};

class output_trace
{
public:
    static constexpr unsigned capacity = 4096;
    static std::wstring trigger_name()
    {
        return L"Local\\MGPU-OutputTrace-" + std::to_wstring(GetCurrentProcessId());
    }
    static std::wstring pipeline_trigger_name()
    {
        return L"Local\\MGPU-OutputPipeline-" + std::to_wstring(GetCurrentProcessId());
    }
    explicit output_trace(const std::filesystem::path &path)
        : frames_(std::make_unique<std::array<output_frame_trace, capacity>>())
    {
        pipeline_path_ = path.wstring() + L".pipeline.bin";
        pipeline_trigger_ = CreateEventW(nullptr, TRUE, FALSE, pipeline_trigger_name().c_str());
        trigger_ = CreateEventW(nullptr, FALSE, FALSE, trigger_name().c_str());
        std::printf("[M354-TRACE] event=%ls available=%u; no recording until requested\n", trigger_name().c_str(), trigger_ ? 1u : 0u);
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        worker_ = std::thread([this, path, frequency] {
            FILE *file = _wfsopen(path.c_str(), L"wb", _SH_DENYNO);
            if (!file) { std::printf("[M354-TRACE] file unavailable; NR/FG unaffected\n"); return; }
            char buffer[32768];
            std::setvbuf(file, buffer, _IOFBF, sizeof(buffer));
            std::fprintf(file, "sequence,source,capture_ns,backlog,visible,requested,qpc_frequency,input_poll_wait_ticks,acquired,markers_ready,slot_ready,osd_begin,osd_end,recorded,submitted,tagged,present_returned,state_read,finished");
            for (const auto *phase : {"sleep", "sim_start", "sim_end", "source_render_start", "source_render_end"})
                for (const auto *stamp : {"native_begin_ns", "native_end_ns", "queued_ns", "dispatch_begin_ns", "dispatch_end_ns"})
                    std::fprintf(file, ",%s_%s", phase, stamp);
            std::fprintf(file, ",helper_render_begin_ns,helper_render_end_ns,source_pings\n");
            unsigned consumed = 0;
            for (;;) {
                const auto committed = committed_.load(std::memory_order_acquire);
                while (consumed < committed) {
                    const auto &frame = (*frames_)[consumed++];
                    std::fprintf(file, "%llu,%llu,%llu,%llu,%u,%u,%lld,%lld",
                        frame.sequence, frame.source, frame.capture_ns, frame.backlog,
                        frame.visible, frame.requested, frequency.QuadPart, frame.input_poll_wait_ticks);
                    for (auto tick : frame.ticks) std::fprintf(file, ",%lld", tick);
                    for (const auto &phase : frame.reflex.phases)
                        std::fprintf(file, ",%llu,%llu,%llu,%llu,%llu", phase.native_begin_ns, phase.native_end_ns,
                            phase.queued_ns, phase.dispatch_begin_ns, phase.dispatch_end_ns);
                    std::fprintf(file, ",%llu,%llu,%llu", frame.helper_render_begin_ns, frame.helper_render_end_ns, frame.source_pings);
                    std::fputc('\n', file);
                }
                std::fflush(file);
                if (consumed == capacity || (stop_.load(std::memory_order_acquire) &&
                    consumed == committed_.load(std::memory_order_acquire))) break;
                Sleep(100);
            }
            std::fclose(file);
        });
    }
    ~output_trace()
    {
        pipeline_.finish();
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
        if (trigger_) CloseHandle(trigger_);
        if (pipeline_trigger_) CloseHandle(pipeline_trigger_);
    }
    void poll()
    {
        if (pipeline_capturing_ && (WaitForSingleObject(pipeline_trigger_, 0) != WAIT_OBJECT_0 ||
            GetTickCount64() >= pipeline_deadline_))
        {
            pipeline_.finish();
            pipeline_capturing_ = false;
        }
    }
    bool pipeline_requested() const
    {
        return pipeline_capturing_ || (pipeline_trigger_ && WaitForSingleObject(pipeline_trigger_, 0) == WAIT_OBJECT_0);
    }
    bool record(const output_frame_trace &frame)
    {
        if (!frame.ordered()) return false;
        if (!pipeline_attempted_ && pipeline_trigger_ && WaitForSingleObject(pipeline_trigger_, 0) == WAIT_OBJECT_0)
        {
            pipeline_attempted_ = true;
            pipeline_deadline_ = GetTickCount64() + 135000;
            pipeline_capturing_ = pipeline_.start(pipeline_path_, mgpu::trace::ticks());
        }
        poll();
        if (pipeline_capturing_) return pipeline_.push(frame);
        if (pipeline_attempted_) return false;
        const auto next = committed_.load(std::memory_order_relaxed);
        if (next >= capacity || !frame.ordered()) return false;
        if (!capturing_) {
            if (!trigger_ || WaitForSingleObject(trigger_, 0) != WAIT_OBJECT_0) return false;
            capturing_ = true;
        }
        (*frames_)[next] = frame;
        committed_.store(next + 1, std::memory_order_release);
        return true;
    }
private:
    std::unique_ptr<std::array<output_frame_trace, capacity>> frames_;
    std::atomic<unsigned> committed_{0};
    std::atomic<bool> stop_{false};
    std::thread worker_;
    HANDLE trigger_ = nullptr;
    bool capturing_ = false;
    mgpu::trace::continuous_stream<output_frame_trace> pipeline_;
    std::filesystem::path pipeline_path_;
    HANDLE pipeline_trigger_ = nullptr;
    bool pipeline_attempted_ = false, pipeline_capturing_ = false;
    ULONGLONG pipeline_deadline_ = 0;
};
}
