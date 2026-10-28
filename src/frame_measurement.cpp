#include "frame_measurement.hpp"
#include "game_work_trace.hpp"
#include "queue_dependency_trace.hpp"
#include "log_queue.hpp"
#include "continuous_trace.hpp"
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <cstring>

namespace mgpu::measurement
{
namespace
{
std::atomic<UINT64> active{0};
std::atomic<unsigned> emitted{0};
std::atomic<bool> extended{false}, native_drained{false};
struct text_record { char text[2048]{}; };
trace::continuous_stream<text_record> &stream() { static auto *value = new trace::continuous_stream<text_record>; return *value; }
}

void emit(const char *record)
{
    if (extended.load(std::memory_order_acquire) && stream().accepting())
    {
        text_record value;
        const auto length = std::snprintf(value.text, sizeof value.text, "%s", record);
        if (length < 0 || length >= sizeof value.text) stream().reject();
        else stream().push(value);
        if (std::strstr(record, "reason=submitted_jobs_drained")) native_drained.store(true, std::memory_order_release);
        if (std::strstr(record, "reason=trace_incomplete"))
        { stream().reject(); native_drained.store(true, std::memory_order_release); }
        return;
    }
    if (!mgpu::lq::running()) return;
    const auto index = emitted.fetch_add(1, std::memory_order_relaxed);
    if (index < 100000) mgpu::lq::post(mgpu::lq::TO_INFO, record);
    else if (index == 100000)
    {
        const auto previous = active.exchange(0, std::memory_order_relaxed);
        char line[180];
        std::snprintf(line, sizeof line, "[MGPU][M336] kind=truncated session=%llu line_limit=100000", previous);
        mgpu::lq::post(mgpu::lq::TO_INFO, line);
    }
}

UINT64 session() { return active.load(std::memory_order_relaxed); }
bool continuous() { return extended.load(std::memory_order_acquire); }

void poll()
{
    static HANDLE control = [] {
        wchar_t name[100];
        std::swprintf(name, sizeof name / sizeof *name, L"Local\\MGPUFrameTiming.%lu", GetCurrentProcessId());
        return CreateEventW(nullptr, TRUE, FALSE, name);
    }();
    static bool latched = false;
    static UINT64 deadline = 0;
    static HANDLE pipeline_control = [] {
        wchar_t name[100];
        std::swprintf(name, sizeof name / sizeof *name, L"Local\\MGPUFramePipeline.%lu", GetCurrentProcessId());
        return CreateEventW(nullptr, TRUE, FALSE, name);
    }();
    static bool pipeline_consumed = false;
    static bool bounded_consumed = false;
    static wchar_t pipeline_path[MAX_PATH]{};
    static bool pipeline_reported = false;
    if (control == nullptr) return;
    const bool pipeline_requested = pipeline_control && WaitForSingleObject(pipeline_control, 0) == WAIT_OBJECT_0;
    const bool requested = pipeline_requested || WaitForSingleObject(control, 0) == WAIT_OBJECT_0;
    const auto now = timing::cpu_ticks();
    if (!requested) latched = false;
    if (requested && !latched && mgpu::lq::running())
    {
        latched = true;
        if (pipeline_consumed) return;
        if (pipeline_requested && bounded_consumed)
        {
            mgpu::lq::post(mgpu::lq::TO_ERROR, "[MGPU][M361] legacy capture already consumed; restart process before continuous capture");
            return;
        }
        if (!pipeline_requested) bounded_consumed = true;
        extended.store(pipeline_requested, std::memory_order_release);
        if (pipeline_requested)
        {
            pipeline_consumed = true;
            wchar_t temporary[MAX_PATH]{};
            if (!GetTempPathW(MAX_PATH, temporary) ||
                _snwprintf_s(pipeline_path, MAX_PATH, _TRUNCATE, L"%sMGPUPipeline.%lu.%llu.bin", temporary, GetCurrentProcessId(), now) < 0 ||
                !stream().start(pipeline_path, now))
            {
                extended.store(false, std::memory_order_release);
                mgpu::lq::post(mgpu::lq::TO_ERROR, "[MGPU][M360] continuous trace failed to start; no measurement enabled");
                return;
            }
        }
        emitted.store(0, std::memory_order_relaxed);
        const auto limit = pipeline_requested ? 165u : 45u;
        deadline = now + limit * timing::cpu_frequency();
        char line[240];
        std::snprintf(line, sizeof line,
            "[MGPU][M336] kind=start session=%llu pid=%lu qpc_frequency=%llu start_ms=%.6f limit_s=%u continuous=%u",
            now, GetCurrentProcessId(), timing::cpu_frequency(), timing::cpu_ms(now), limit, unsigned(pipeline_requested));
        emit(line);
        active.store(now, std::memory_order_relaxed);
    }
    if (session() != 0 && (!requested || now >= deadline || !mgpu::lq::running() ||
        (continuous() && !stream().accepting())))
    {
        const auto previous = active.exchange(0, std::memory_order_relaxed);
        char line[180];
        std::snprintf(line, sizeof line, "[MGPU][M336] kind=stop session=%llu end_ms=%.6f reason=%s",
            previous, timing::cpu_ms(now), requested ? "limit_or_logger" : "controller");
        emit(line);
    }
    mgpu::queue_trace::poll(session(), continuous());
    mgpu::game_work::poll(session(), continuous());
    if (continuous() && session() == 0 && native_drained.load(std::memory_order_acquire) &&
        !mgpu::queue_trace::active()) stream().finish();
    if (continuous() && stream().done() && !pipeline_reported)
    {
        pipeline_reported = true;
        char line[700];
        std::snprintf(line, sizeof line, "[MGPU][M360] pipeline_export=%ls records=%llu dropped=%llu failures=%llu; event timestamps, not physical scanout",
                      pipeline_path, stream().written(), stream().dropped(), stream().failures());
        mgpu::lq::post(mgpu::lq::TO_INFO, line);
    }
    static bool queue_export_reported = false;
    if (!queue_export_reported && mgpu::queue_trace::export_path()[0] != L'\0')
    {
        queue_export_reported = true;
        char line[700];
        std::snprintf(line, sizeof line, "[MGPU][M343] queue_trace_export=%ls; API intervals, not GPU wait durations; validate binary header and ETW coverage", mgpu::queue_trace::export_path());
        emit(line);
    }
}

void queue(UINT64 session_id, const char *role, ID3D12CommandQueue *value)
{
    mgpu::queue_trace::observe_queue(value);
    if (session_id == 0 || value == nullptr) return;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(value->GetDevice(IID_PPV_ARGS(&device)))) return;
    const auto luid = device->GetAdapterLuid();
    char line[350];
    std::snprintf(line, sizeof line,
        "[MGPU][M336] kind=queue session=%llu role=%s queue=%p device=%p luid=%08lx:%08lx type=%u",
        session_id, role, value, device.Get(), static_cast<unsigned long>(luid.HighPart),
        luid.LowPart, unsigned(value->GetDesc().Type));
    emit(line);
}

cpu_scope::cpu_scope(const char *phase, unsigned frame, unsigned component)
    : session_(session()), phase_(phase), frame_(frame), component_(component)
{
    if (session_ != 0) started_ = timing::cpu_ticks();
}

cpu_scope::~cpu_scope()
{
    if (session_ == 0) return;
    const auto finished = timing::cpu_ticks();
    char line[350];
    std::snprintf(line, sizeof line,
        "[MGPU][M336] kind=cpu session=%llu phase=%s tid=%lu engine_frame=%u component=%u start_ms=%.6f end_ms=%.6f",
        session_, phase_, GetCurrentThreadId(), frame_, component_, timing::cpu_ms(started_), timing::cpu_ms(finished));
    emit(line);
}

scoped_lock::scoped_lock(std::mutex &mutex, const char *phase, unsigned frame)
    : lock_(mutex, std::defer_lock), session_(session()), phase_(phase), frame_(frame)
{
    if (session_ != 0) started_ = timing::cpu_ticks();
    lock_.lock();
    if (session_ != 0) acquired_ = timing::cpu_ticks();
}

scoped_lock::~scoped_lock()
{
    const auto finished = session_ != 0 ? timing::cpu_ticks() : 0;
    lock_.unlock();
    if (session_ == 0) return;
    char line[380];
    std::snprintf(line, sizeof line,
        "[MGPU][M336] kind=lock session=%llu phase=%s tid=%lu engine_frame=%u start_ms=%.6f acquired_ms=%.6f end_ms=%.6f",
        session_, phase_, GetCurrentThreadId(), frame_, timing::cpu_ms(started_),
        timing::cpu_ms(acquired_), timing::cpu_ms(finished));
    emit(line);
}
}
