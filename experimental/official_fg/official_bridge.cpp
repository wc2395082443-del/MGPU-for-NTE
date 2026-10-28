#define MGPU_PACKET_ONLY
#include "official_bridge.hpp"
#include "official_channel.hpp"
#include "shared_constants.hpp"
#include "../../src/diag.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <filesystem>
#include <cstdio>
#include <exception>
#include <string>

namespace mgpu::official_bridge
{
namespace
{
using namespace official_fg;
SRWLOCK lifetime_lock = SRWLOCK_INIT;
SRWLOCK markers_lock = SRWLOCK_INIT;
std::mutex producer_lock;
std::atomic<bool> requested{false};
std::atomic<unsigned> requested_multiplier{6};
std::atomic<bool> requested_visible{true};

struct context
{
    std::unique_ptr<shared_frame_transport> transport;
    std::unique_ptr<producer_schedule> schedule;
    official_channel *channel = nullptr;
    HANDLE mapping = nullptr, frame_event = nullptr, marker_event = nullptr, sleep_event = nullptr, stop_event = nullptr;
    HANDLE parent = nullptr, process = nullptr, job = nullptr;
    std::uint64_t next = 1;
    std::uint32_t previous_frame = 0;
    bool recorded = false, fault = false;
    unsigned report_reason = UINT_MAX;
    std::uint64_t report_tick = 0;
    frame_packet pending{};
    ~context()
    {
        if (channel) UnmapViewOfFile(channel);
        for (auto handle : {mapping, frame_event, marker_event, sleep_event, stop_event, parent, process, job})
            if (handle) CloseHandle(handle);
    }
};
context *active = nullptr;
bool attempted = false;

void report_failure(context *current, const char *message)
{
    if (current) {
        if (current->fault) return;
        current->fault = true;
        if (current->schedule) current->schedule->disconnect();
        if (current->channel) {
            InterlockedExchange(&current->channel->presenting, 0);
            InterlockedExchange(&current->channel->state, static_cast<LONG>(host_state::failed));
        }
        if (current->stop_event) SetEvent(current->stop_event);
    }
    char line[512];
    std::snprintf(line, sizeof line, "[MGPU][M349-HOST] %s; official output stopped, NR remains enabled", message);
    mgpu::diag::error(line);
}

std::filesystem::path addon_directory()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&addon_directory), &module)) return {};
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, 32768);
    return length && length < 32768 ? std::filesystem::path(path).parent_path() : std::filesystem::path{};
}

bool launch(context &current, ID3D12Device *device, HWND window,
    const std::array<plane_description, shared_planes> &descriptions)
{
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (!window || owner != GetCurrentProcessId()) return false;
    const auto directory = addon_directory() / L"mgpu-official";
    if (!directory.is_absolute()) return false;
    const auto executable = directory / L"mgpu_official_host.exe";
    if (!std::filesystem::is_regular_file(executable)) return false;
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const auto generation = static_cast<std::uint64_t>(counter.QuadPart);
    current.transport = std::make_unique<shared_frame_transport>();
    if (FAILED(current.transport->create(device, descriptions, generation))) return false;
    current.schedule = std::make_unique<producer_schedule>(generation);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    current.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(official_channel), nullptr);
    current.frame_event = CreateEventW(&security, FALSE, FALSE, nullptr);
    current.marker_event = CreateEventW(&security, FALSE, FALSE, nullptr);
    current.sleep_event = CreateEventW(&security, FALSE, FALSE, nullptr);
    current.stop_event = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!current.mapping || !current.frame_event || !current.marker_event || !current.sleep_event || !current.stop_event ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &current.parent,
            SYNCHRONIZE, TRUE, 0)) return false;
    current.channel = static_cast<official_channel *>(MapViewOfFile(current.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(official_channel)));
    if (!current.channel) return false;
    new (current.channel) official_channel{};
    auto &channel = *current.channel;
    channel.byte_size = sizeof(channel);
    channel.manifest = current.transport->manifest();
    channel.parent_pid = GetCurrentProcessId();
    channel.parent_window = channel_integer(window);
    channel.parent_process = channel_integer(current.parent);
    channel.frames_event = channel_integer(current.frame_event);
    channel.markers_event = channel_integer(current.marker_event);
    channel.sleep_event = channel_integer(current.sleep_event);
    channel.stop_event = channel_integer(current.stop_event);
    channel.multiplier = static_cast<LONG>(requested_multiplier.load());
    channel.visible = requested_visible.load() ? 1 : 0;
    auto handles = current.transport->inheritable_handles();
    handles.insert(handles.end(), {current.mapping, current.frame_event, current.marker_event,
        current.sleep_event, current.stop_event, current.parent});
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return false;
    const bool valid = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles.data(), handles.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    std::wstring command = L"\"" + executable.wstring() + L"\" --channel " + std::to_wstring(channel_integer(current.mapping));
    PROCESS_INFORMATION process{};
    const bool created = valid && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    if (!created) return false;
    current.process = process.hProcess;
    current.job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!current.job || !SetInformationJobObject(current.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(current.job, current.process)) {
        TerminateProcess(current.process, 91);
        WaitForSingleObject(current.process, 5000);
        CloseHandle(process.hThread);
        return false;
    }
    const DWORD resumed = ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    if (resumed == static_cast<DWORD>(-1)) return false;
    mgpu::diag::info("[MGPU][M349-HOST] isolated official helper launched; same-adapter GPU shared inputs; no independent top-level window");
    return true;
}

bool healthy(context &current)
{
    if (current.fault || !current.channel) return false;
    if (WaitForSingleObject(current.process, 0) == WAIT_OBJECT_0 ||
        read_atomic(current.channel->state) == static_cast<LONG>(host_state::failed)) {
        report_failure(&current, current.channel->error[0] ? current.channel->error : "helper exited");
        return false;
    }
    const auto heartbeat = static_cast<std::uint64_t>(read_atomic(current.channel->heartbeat_ms));
    if (heartbeat != 0 && GetTickCount64() > heartbeat + 5000) {
        report_failure(&current, "official helper heartbeat expired");
        return false;
    }
    return true;
}

void report_export_hold(context &current, unsigned reason, std::uint32_t frame, const char *description)
{
    const auto now = GetTickCount64();
    if (current.report_reason == reason && now - current.report_tick < 5000) return;
    current.report_reason = reason;
    current.report_tick = now;
    char line[420];
    std::snprintf(line, sizeof line,
        "[MGPU][M350-EXPORT-HOLD] reason=%u detail=%s frame=%u helper_state=%ld first_source=%ld published=%lld; NR remains independent of helper readiness",
        reason, description, frame, read_atomic(current.channel->state),
        read_atomic(current.channel->first_source_frame), read_atomic(current.channel->published));
    mgpu::diag::info(line);
}

std::uint64_t send_marker(context &current, std::uint32_t frame, unsigned kind,
    std::uint64_t native_begin_ns, std::uint64_t native_end_ns)
{
    auto &channel = *current.channel;
    AcquireSRWLockExclusive(&markers_lock);
    const auto written = read_atomic(channel.markers_written);
    const auto consumed = read_atomic(channel.markers_read);
    if (written - consumed >= marker_capacity) {
        ReleaseSRWLockExclusive(&markers_lock);
        InterlockedIncrement(&channel.marker_failures);
        SetEvent(current.stop_event);
        return 0;
    }
    const auto serial = static_cast<std::uint64_t>(written + 1);
    channel.markers[written % marker_capacity] = {frame, kind, serial,
        source_clock_ns(), native_begin_ns, native_end_ns};
    InterlockedExchange64(&channel.markers_written, written + 1);
    ReleaseSRWLockExclusive(&markers_lock);
    SetEvent(current.marker_event);
    return serial;
}
}

void request(bool enabled, unsigned multiplier)
{
    requested.store(enabled);
    requested_multiplier.store(multiplier);
}
bool selected() { return requested.load(); }

bool backpressured()
{
    std::lock_guard<std::mutex> guard(producer_lock);
    if (!active || !healthy(*active) || read_atomic(active->channel->state) != static_cast<LONG>(host_state::ready)) return false;
    const auto ready = active->transport->ready_fence()->GetCompletedValue();
    const auto released = active->transport->released_fence()->GetCompletedValue();
    if (ready == UINT64_MAX || released == UINT64_MAX) { report_failure(active, "shared fence device loss"); return false; }
    return active->next > shared_slots && (released < active->next - shared_slots || ready < active->next - shared_slots);
}

static bool record_impl(ID3D12GraphicsCommandList *commands, HWND game_window,
    const std::array<texture_binding, shared_planes> &sources, std::uint32_t source_frame,
    std::uint32_t viewport, const fgmap::camera_snapshot &camera, float min_depth_separation, bool reset_history)
{
    if (!selected()) return false;
    std::lock_guard<std::mutex> guard(producer_lock);
    std::array<plane_description, shared_planes> descriptions{};
    for (unsigned plane = 0; plane < shared_planes; ++plane) {
        if (!sources[plane].texture) return false;
        const auto description = sources[plane].texture->GetDesc();
        descriptions[plane] = {static_cast<unsigned>(description.Width), description.Height, static_cast<unsigned>(description.Format)};
    }
    if (!active && !attempted) {
        attempted = true;
        auto candidate = std::make_unique<context>();
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (FAILED(commands->GetDevice(IID_PPV_ARGS(&device))) || !launch(*candidate, device.Get(), game_window, descriptions)) {
            report_failure(candidate.get(), "helper startup failed; check mgpu-official package and game HWND");
            return false;
        }
        AcquireSRWLockExclusive(&lifetime_lock);
        active = candidate.release();
        ReleaseSRWLockExclusive(&lifetime_lock);
    }
    if (!active || !healthy(*active) || active->recorded) return false;
    auto &current = *active;
    if (read_atomic(current.channel->state) != static_cast<LONG>(host_state::ready)) {
        report_export_hold(current, 1, source_frame, "helper_initializing");
        return false;
    }
    const auto first_source = static_cast<std::uint32_t>(read_atomic(current.channel->first_source_frame));
    if (!first_source || source_frame - first_source >= 0x80000000u) {
        report_export_hold(current, 2, source_frame, "waiting_for_live_source_frame");
        return false;
    }
    const auto &manifest = current.transport->manifest();
    for (unsigned plane = 0; plane < shared_planes; ++plane) {
        const auto expected = manifest.descriptions[plane];
        if (expected.width != descriptions[plane].width || expected.height != descriptions[plane].height ||
            expected.format != descriptions[plane].format) {
            report_failure(&current, "resource resize requires a fresh drained helper generation");
            return false;
        }
    }
    frame_packet packet{};
    if (!pack_frame(packet, manifest.generation, current.next, source_frame, source_frame, source_frame,
        viewport, camera, min_depth_separation, descriptions)) {
        report_export_hold(current, 3, source_frame, "frame_camera_or_extent_contract");
        return false;
    }
    packet.camera.reset |= reset_history || (current.previous_frame && source_frame - current.previous_frame != 1);
    if (!current.schedule->reserve(packet, current.transport->released_fence()->GetCompletedValue(),
        current.transport->ready_fence()->GetCompletedValue())) {
        report_export_hold(current, 4, source_frame, "slot_not_retired_or_nonmonotonic_source");
        return false;
    }
    if (FAILED(current.transport->record_write(commands, slot_for(packet.sequence), sources))) {
        current.schedule->cancel_unsubmitted();
        report_failure(&current, "NR tuple export rejected");
        return false;
    }
    current.pending = packet;
    current.recorded = true;
    return true;
}

bool record(ID3D12GraphicsCommandList *commands, HWND game_window,
    const std::array<texture_binding, shared_planes> &sources, std::uint32_t source_frame,
    std::uint32_t viewport, const fgmap::camera_snapshot &camera, float min_depth_separation, bool reset_history)
{
    try { return record_impl(commands, game_window, sources, source_frame, viewport, camera, min_depth_separation, reset_history); }
    catch (const std::exception &error) {
        std::lock_guard<std::mutex> guard(producer_lock);
        attempted = true;
        report_failure(active, error.what());
        return false;
    }
}

bool submitted(ID3D12CommandQueue *queue)
{
    std::lock_guard<std::mutex> guard(producer_lock);
    if (!active || !active->recorded) return true;
    auto &current = *active;
    if (FAILED(current.transport->signal_ready(queue, current.pending.sequence)) || !current.schedule->publish()) {
        report_failure(&current, "export GPU submission uncertain; shared slots retained");
        return false;
    }
    auto &channel = *current.channel;
    channel.frames[slot_for(current.pending.sequence)] = current.pending;
    InterlockedExchange64(&channel.published, current.pending.sequence);
    InterlockedExchange(&channel.multiplier, requested_multiplier.load());
    SetEvent(current.frame_event);
    current.previous_frame = current.pending.color_frame;
    ++current.next;
    current.recorded = false;
    if (current.pending.sequence <= 3 || current.pending.sequence % 120 == 0) {
        char line[280];
        std::snprintf(line, sizeof line, "[MGPU][M349-EXPORT] sequence=%llu source_frame=%u slot=%u; NR/depth/MV and camera submitted together",
            static_cast<unsigned long long>(current.pending.sequence), current.pending.color_frame, slot_for(current.pending.sequence));
        mgpu::diag::info(line);
    }
    return true;
}

void submission_failed(bool executed)
{
    std::lock_guard<std::mutex> guard(producer_lock);
    if (!active || !active->recorded) return;
    if (!executed) { active->schedule->cancel_unsubmitted(); active->recorded = false; }
    report_failure(active, "parent NR submission failed");
}

std::uint64_t source_clock_ns()
{
    static const auto frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart; }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<std::uint64_t>(double(now.QuadPart) * 1e9 / double(frequency));
}

void source_sleep(std::uint32_t frame, std::uint64_t native_begin_ns, std::uint64_t native_end_ns)
{
    if (!selected() || !frame || !TryAcquireSRWLockShared(&lifetime_lock)) return;
    if (active && active->channel && read_atomic(active->channel->state) == static_cast<LONG>(host_state::ready)) {
        const auto serial = send_marker(*active, frame, sleep_marker, native_begin_ns, native_end_ns);
        const auto deadline = GetTickCount64() + 100;
        while (serial && read_atomic(active->channel->sleep_completed) < static_cast<LONG64>(serial)) {
            HANDLE waits[] = {active->sleep_event, active->stop_event, active->process};
            const auto now = GetTickCount64();
            if (now >= deadline || WaitForMultipleObjects(3, waits, FALSE, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0) {
                InterlockedIncrement(&active->channel->marker_failures);
                InterlockedExchange(&active->channel->state, static_cast<LONG>(host_state::failed));
                InterlockedExchange(&active->channel->presenting, 0);
                SetEvent(active->stop_event);
                break;
            }
        }
    }
    ReleaseSRWLockShared(&lifetime_lock);
}

void source_marker(std::uint32_t frame, unsigned marker, std::uint64_t native_begin_ns, std::uint64_t native_end_ns)
{
    if (!selected() || !frame || !source_marker_supported(marker) || !TryAcquireSRWLockShared(&lifetime_lock)) return;
    if (active && active->channel && read_atomic(active->channel->state) == static_cast<LONG>(host_state::ready))
        (void)send_marker(*active, frame, marker, native_begin_ns, native_end_ns);
    ReleaseSRWLockShared(&lifetime_lock);
}

void set_visible(bool visible)
{
    requested_visible.store(visible);
    AcquireSRWLockShared(&lifetime_lock);
    if (active && active->channel) InterlockedExchange(&active->channel->visible, visible ? 1 : 0);
    ReleaseSRWLockShared(&lifetime_lock);
}

status inspect()
{
    status result{};
    result.requested = selected();
    result.visible = requested_visible.load();
    std::lock_guard<std::mutex> guard(producer_lock);
    if (!active || !healthy(*active)) { result.failed = attempted; return result; }
    auto &channel = *active->channel;
    result.ready = read_atomic(channel.state) == static_cast<LONG>(host_state::ready);
    result.presenting = read_atomic(channel.presenting) != 0;
    result.maximum_multiplier = static_cast<unsigned>(read_atomic(channel.maximum_multiplier));
    result.exported = read_atomic(channel.published);
    result.real_presents = read_atomic(channel.real_presents);
    result.sdk_presents = read_atomic(channel.sdk_presents);
    (void)read_metrics(channel.current_metrics, result.current_rates);
    (void)read_metrics(channel.last_visible_metrics, result.visible_rates);
    return result;
}

bool shutdown()
{
    std::lock_guard<std::mutex> guard(producer_lock);
    AcquireSRWLockExclusive(&lifetime_lock);
    bool stopped = true;
    if (active) {
        SetEvent(active->stop_event);
        stopped = !active->recorded && WaitForSingleObject(active->process, 3000) == WAIT_OBJECT_0;
        if (stopped) {
            const auto completed = active->transport->ready_fence()->GetCompletedValue();
            stopped = completed != UINT64_MAX && completed >= active->next - 1;
        }
        if (stopped) { delete active; active = nullptr; }
    }
    ReleaseSRWLockExclusive(&lifetime_lock);
    if (!stopped) mgpu::diag::error("[MGPU][M349-HOST] helper/export not retired; retaining shared allocations");
    return stopped;
}
}
