#include "aligned_input.hpp"
#include "tagged_resource_validation.hpp"
#include "tagged_motion_policy.hpp"
#include "aligned_texture_policy.hpp"
#include "d3d12_device_identity.hpp"
#include "depth_normalizer.hpp"
#include "producer_copy.hpp"
#include "diag.hpp"
#include "frame_measurement.hpp"
#include "game_work_trace.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <mutex>

namespace mgpu::aligned
{
namespace
{
constexpr unsigned capacity = 4;
constexpr unsigned queue_capacity = 8;
constexpr unsigned execute_slot = 10;
std::atomic<std::uint64_t> latest_color_submission{0};
const GUID command_guid = {0xa091f3d1, 0xefc8, 0x48d1, {0xa8, 0xca, 0x23, 0x53, 0x93, 0x08, 0x16, 0x33}};
using execute_fn = void (STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);

struct command_record
{
    std::uint64_t serial = 0;
    bool submitted = false;
    ID3D12Fence *arrival = nullptr;
    UINT64 arrival_value = 0;
    motion_packet::identity identity{};
    bool submitting = false;
};
struct queue_record
{
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 next = 0;
    timing::queue_clock_cache clock;
    UINT64 measurement_session = 0;
};
struct frame_record
{
    frame_state state{};
    std::array<plane, 3> planes{};
    tagged_resource depth_region{};
    plane normalized_depth{};
    depth_job normalization{};
    std::array<timing::gpu_timestamp_span, 3> capture_timing{};
    double acquired_ms = -1, transfer_submitted_ms = -1;
    UINT64 seal = 0;
    UINT64 measurement_session = 0;
    bool timing_collected = true;
};
struct hook_record
{
    void **entry = nullptr;
    execute_fn original = nullptr;
    std::recursive_mutex submission_mutex;
};
struct state
{
    std::mutex mutex;
    std::mutex hook_mutex;
    std::atomic<bool> active{false};
    std::array<frame_record, capacity> frames{};
    std::array<queue_record, queue_capacity> queues{};
    std::array<command_record, 64> commands{};
    std::array<guide_constants, 64> constants{};
    std::array<hook_record, 8> hooks{};
    std::uint64_t next_frame = 0, next_command = 0, accepted = 0, held = 0;
    frame_key last_delivered{};
    bool delivered = false;
    bool failed = false;
    device_identity game_device;
    depth_normalizer depth_converter;
    mgpu::producer_copy::transfer_queue outgoing;
    unsigned outgoing_queue = queue_capacity;
    bool viewport_known = false;
    unsigned viewport = 0;
    unsigned rejects = 0;
    std::array<unsigned, 3> input_observations{};
    std::array<unsigned, 3> input_failures{};
    ULONGLONG last_wait_log = 0;
    std::array<timing::metric, 12> timing_metrics{};
    unsigned timing_frames = 0;
    bool ready_dispatch = false;
    HANDLE source_event = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> wake_fence;
    UINT64 wake_value = 0;
    unsigned wake_slot = capacity;
    std::uint64_t wake_serial = 0, wake_registrations = 0;
    bool wake_failed = false;
};
state &data()
{
    static state *instance = new state;
    return *instance;
}

void release_plane(plane &target)
{
    if (target.buffer != nullptr) target.buffer->Release();
    target.buffer = nullptr;
    target.description = {};
    target.footprint = {};
    target.bytes = 0;
}

void reject(state &context, const char *reason)
{
    if (context.rejects++ < 12)
    {
        char line[220];
        std::snprintf(line, sizeof line, "[MGPU][R323-ALIGN] capture held: %s", reason);
        mgpu::diag::warn(line);
    }
}

void report_input(state &context, unsigned component, unsigned type, const tagged_resource &tag,
                  const D3D12_RESOURCE_DESC &description, const char *reason)
{
    auto &count = reason == nullptr ? context.input_observations[component] : context.input_failures[component];
    if (count++ >= 4) return;
    char line[540];
    std::snprintf(line, sizeof line,
        "[MGPU][R324-INPUT] type=%u resource=0x%llx dim=%u size=%llux%u array=%u mips=%u "
        "samples=%u format=%u flags=0x%x extent=(%u,%u,%u,%u) status=%s",
        type, tag.native, unsigned(description.Dimension), description.Width, description.Height,
        unsigned(description.DepthOrArraySize), unsigned(description.MipLevels), description.SampleDesc.Count,
        unsigned(description.Format), unsigned(description.Flags), tag.left, tag.top, tag.width, tag.height,
        reason != nullptr ? reason : "descriptor accepted; plane0/mip0");
    mgpu::diag::info(line);
}

UINT64 completed(state &context, unsigned index)
{
    return index < queue_capacity && context.queues[index].fence != nullptr
        ? context.queues[index].fence->GetCompletedValue() : 0;
}

void collect_timing(state &context, frame_record &frame)
{
    const auto done = [&](unsigned index) { return completed(context, index); };
    if (frame.timing_collected || !frame.state.reclaimable(done)) return;
    std::array<timing::span, 3> captures{};
    for (unsigned component = 0; component != 3; ++component)
    {
        const auto &timer = frame.capture_timing[component];
        captures[component] = timer.read(frame.state.commands[component], true);
        context.timing_metrics[component].add(captures[component].gpu_ms);
        context.timing_metrics[3].add(timing::interval(timer.recorded_ms(), timer.submitted_ms()));
        context.timing_metrics[4].add(timing::interval(timer.submitted_ms(), captures[component].start_ms));
    }
    const auto depth = frame.normalization.timing.read(frame.state.serial, true);
    const auto depth_submit_ms = frame.normalization.timing.submitted_ms();
    context.timing_metrics[5].add(depth.gpu_ms);
    context.timing_metrics[6].add(timing::interval(captures[1].end_ms, depth.start_ms));
    context.timing_metrics[7].add(timing::interval(depth_submit_ms, depth.start_ms));
    context.timing_metrics[8].add(timing::interval(depth_submit_ms, depth.end_ms));
    context.timing_metrics[9].add(timing::interval(frame.acquired_ms, frame.transfer_submitted_ms));
    context.timing_metrics[10].add(timing::interval(depth_submit_ms, frame.acquired_ms));
    const auto ready_ms = captures[0].end_ms >= 0 && captures[2].end_ms >= 0 && depth.end_ms >= 0
        ? (std::max)({captures[0].end_ms, captures[2].end_ms, depth.end_ms}) : -1;
    context.timing_metrics[11].add(ready_ms >= 0 && frame.acquired_ms >= 0
        ? (std::max)(0.0, ready_ms - frame.acquired_ms) : -1);
    if (frame.measurement_session != 0)
    {
        const auto transfer = context.outgoing.sample(frame.state.released.value);
        char report[1800];
        std::snprintf(report, sizeof report,
            "[MGPU][M336] kind=producer session=%llu seal=%llu serial=%llu engine_frame=%u viewport=%u "
            "color_start=%.6f color_end=%.6f depth_start=%.6f depth_end=%.6f motion_start=%.6f motion_end=%.6f "
            "normalize_start=%.6f normalize_end=%.6f ready_ms=%.6f acquire_ms=%.6f "
            "out_submit=%.6f out_start=%.6f out_end=%.6f retirement=%llu held=%llu "
            "color_submit=%.6f depth_submit=%.6f motion_submit=%.6f "
            "color_queue=%u depth_queue=%u motion_queue=%u",
            frame.measurement_session, frame.seal, frame.state.serial, frame.state.key.frame, frame.state.key.viewport,
            captures[0].start_ms, captures[0].end_ms, captures[1].start_ms, captures[1].end_ms,
            captures[2].start_ms, captures[2].end_ms, depth.start_ms, depth.end_ms, ready_ms,
            frame.acquired_ms, transfer.submitted_ms, transfer.execution.start_ms, transfer.execution.end_ms,
            frame.state.released.value, context.held, frame.capture_timing[0].submitted_ms(),
            frame.capture_timing[1].submitted_ms(), frame.capture_timing[2].submitted_ms(),
            frame.state.copied[0].queue, frame.state.copied[1].queue, frame.state.copied[2].queue);
        mgpu::measurement::emit(report);
        if (transfer.color_execution.start_ms >= 0 && transfer.depth_execution.start_ms < 0)
        {
            std::snprintf(report, sizeof report,
                "[MGPU][M340-COPY] kind=producer session=%llu seal=%llu engine_frame=%u "
                "color_start=%.6f color_end=%.6f guides_start=%.6f guides_end=%.6f retirement=%llu; "
                "full retirement follows both stages; combined span includes between-stage wait",
                frame.measurement_session, frame.seal, frame.state.key.frame,
                transfer.color_execution.start_ms, transfer.color_execution.end_ms,
                transfer.guides_execution.start_ms, transfer.guides_execution.end_ms, frame.state.released.value);
            mgpu::measurement::emit(report);
        }
        if (transfer.depth_execution.start_ms >= 0)
        {
            std::snprintf(report, sizeof report,
                "[MGPU][M341-COPY] kind=producer session=%llu seal=%llu engine_frame=%u "
                "color_start=%.6f color_end=%.6f depth_start=%.6f depth_end=%.6f "
                "motion_start=%.6f motion_end=%.6f retirement=%llu; full retirement follows all three stages",
                frame.measurement_session, frame.seal, frame.state.key.frame,
                transfer.color_execution.start_ms, transfer.color_execution.end_ms,
                transfer.depth_execution.start_ms, transfer.depth_execution.end_ms,
                transfer.motion_execution.start_ms, transfer.motion_execution.end_ms, frame.state.released.value);
            mgpu::measurement::emit(report);
        }
    }
    if (frame.seal <= 3 || frame.seal % 120ull == 0)
    {
        char report[800];
        std::snprintf(report, sizeof report,
            "[MGPU][R334-PRODUCER-TIMELINE] seal=%llu engine_frame=%u "
            "color_qpc_ms=(%.6f,%.6f) depth_qpc_ms=(%.6f,%.6f) motion_qpc_ms=(%.6f,%.6f) "
            "normalize_qpc_ms=(%.6f,%.6f) acquire_cpu_ms=%.6f transfer_submit_cpu_ms=%.6f; "
            "retired snapshots only, -1 means unavailable; capture is not whole-game render time",
            frame.seal, frame.state.key.frame, captures[0].start_ms, captures[0].end_ms,
            captures[1].start_ms, captures[1].end_ms, captures[2].start_ms, captures[2].end_ms,
            depth.start_ms, depth.end_ms, frame.acquired_ms, frame.transfer_submitted_ms);
        mgpu::diag::info(report);
    }
    if (++context.timing_frames == 120)
    {
        const char *names[] = {"color_capture_gpu", "depth_capture_gpu", "motion_capture_gpu",
            "capture_record_to_submit", "capture_submit_to_gpu", "depth_normalize_gpu",
            "depth_capture_end_to_normalize_start", "depth_submit_to_gpu", "depth_submit_to_gpu_end",
            "acquire_to_transfer_call", "depth_submit_to_acquire", "acquire_lead_before_texture_ready"};
        for (unsigned index = 0; index != context.timing_metrics.size(); ++index)
        {
            const auto &metric = context.timing_metrics[index];
            char report[320];
            std::snprintf(report, sizeof report,
                "[MGPU][R334-TIMING] seal=%llu engine_frame=%u stage=%s n=%llu invalid=%llu mean_ms=%.6f max_ms=%.6f; capture spans include barriers; intervals may overlap",
                frame.seal, frame.state.key.frame, names[index], metric.count, metric.invalid,
                metric.mean(), metric.maximum);
            mgpu::diag::info(report);
        }
        context.timing_metrics = {};
        context.timing_frames = 0;
    }
    frame.timing_collected = true;
}

command_record *find_command(state &context, std::uint64_t serial)
{
    for (auto &record : context.commands)
        if (serial != 0 && record.serial == serial) return &record;
    return nullptr;
}

command_record *record_command(state &context, ID3D12GraphicsCommandList *command)
{
    std::uint64_t serial = 0;
    UINT size = sizeof serial;
    if (SUCCEEDED(command->GetPrivateData(command_guid, &size, &serial)) && size == sizeof serial)
        if (auto *record = find_command(context, serial); record != nullptr && !record->submitted)
            return record->submitting ? nullptr : record;
    for (auto &record : context.commands)
    {
        if (record.serial != 0 && !record.submitted) continue;
        serial = ++context.next_command;
        if (FAILED(command->SetPrivateData(command_guid, sizeof serial, &serial))) return nullptr;
        if (record.arrival != nullptr) record.arrival->Release();
        record = {serial};
        return &record;
    }
    return nullptr;
}

void after_submit(ID3D12CommandQueue *queue, const std::array<std::uint64_t, 64> &serials, unsigned serial_count,
                  UINT64 submitted_at)
{
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (context.failed) return;
    unsigned queue_index = queue_capacity;
    for (unsigned index = 0; index != queue_capacity; ++index)
        if (context.queues[index].queue == queue) { queue_index = index; break; }
    if (queue_index == queue_capacity)
    {
        for (unsigned index = 0; index != queue_capacity; ++index)
        {
            auto &record = context.queues[index];
            if (record.queue != nullptr) continue;
            ID3D12Device *device = nullptr;
            if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) break;
            const HRESULT result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&record.fence));
            device->Release();
            if (FAILED(result)) break;
            record.queue = queue;
            queue->AddRef();
            queue_index = index;
            break;
        }
    }
    if (queue_index == queue_capacity)
    {
        context.failed = true;
        reject(context, "cannot fence the actual submission queue");
        return;
    }
    auto &timeline = context.queues[queue_index];
    const auto measurement_session = mgpu::measurement::session();
    if (measurement_session != 0 && timeline.measurement_session != measurement_session)
    {
        timeline.measurement_session = measurement_session;
        mgpu::measurement::queue(measurement_session, "tagged_submission", queue);
    }
    const completion ticket{queue_index, ++timeline.next};
    if (FAILED(queue->Signal(timeline.fence, ticket.value)))
    {
        context.failed = true;
        reject(context, "post-submit fence signal failed");
        return;
    }
    for (unsigned index = 0; index != serial_count; ++index)
    {
        const auto serial = serials[index];
        auto *command = find_command(context, serial);
        if (command == nullptr || command->submitted) continue;
        command->submitting = false;
        command->submitted = true;
        for (auto &frame : context.frames)
        {
            for (unsigned component = 0; component != 3; ++component)
                if (frame.state.commands[component] == serial)
                {
                    frame.state.copied[component] = ticket;
                    if (component == 0)
                    {
                        const auto previous = latest_color_submission.load(std::memory_order_relaxed);
                        if (previous == 0 || newer_frame(frame.state.key.frame, static_cast<unsigned>(previous)))
                            latest_color_submission.store((1ull << 32) | frame.state.key.frame, std::memory_order_release);
                    }
                    frame.capture_timing[component].submitted(queue, submitted_at, &timeline.clock);
                    if (frame.measurement_session != 0)
                    {
                        char report[500];
                        std::snprintf(report, sizeof report,
                            "[MGPU][M336] kind=submission session=%llu serial=%llu engine_frame=%u component=%u "
                            "command_serial=%llu queue_index=%u queue=%p fence=%p fence_value=%llu submit_ms=%.6f",
                            frame.measurement_session, frame.state.serial, frame.state.key.frame, component,
                            serial, queue_index, queue, timeline.fence, ticket.value, timing::cpu_ms(submitted_at));
                        mgpu::measurement::emit(report);
                    }
                }
            if (frame.state.release_command == serial) frame.state.released = ticket;
        }
        if (command->arrival != nullptr &&
            FAILED(queue->Signal(command->arrival, command->arrival_value)))
        {
            context.failed = true;
            reject(context, "cross-adapter arrival signal failed");
        }
    }
}

template <unsigned Hook>
void STDMETHODCALLTYPE execute(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    mgpu::measurement::cpu_scope measurement("execute_hook");
    auto &context = data();
    auto &hook = context.hooks[Hook];
    std::unique_lock<std::recursive_mutex> submission_lock(hook.submission_mutex, std::defer_lock);
    std::array<std::uint64_t, 64> serials{};
    unsigned serial_count = 0;
    for (UINT index = 0; lists != nullptr && index != count; ++index)
    {
        std::uint64_t serial = 0;
        UINT size = sizeof serial;
        if (lists[index] != nullptr &&
            SUCCEEDED(lists[index]->GetPrivateData(command_guid, &size, &serial)) &&
            size == sizeof serial && serial != 0 && serial_count < serials.size() &&
            std::find(serials.begin(), serials.begin() + serial_count, serial) == serials.begin() + serial_count)
            serials[serial_count++] = serial;
    }
    if (serial_count != 0)
    {
        submission_lock.lock();
        std::lock_guard<std::mutex> lock(context.mutex);
        for (unsigned index = 0; index != serial_count; ++index)
            if (auto *record = find_command(context, serials[index]); record != nullptr && !record->submitted)
                record->submitting = true;
    }
    const auto submitted_at = serial_count != 0 ? timing::cpu_ticks() : 0;
    mgpu::game_work::before_submit(queue, count, lists);
    { mgpu::measurement::cpu_scope original("execute_original"); hook.original(queue, count, lists); }
    mgpu::game_work::after_submit(queue, count, lists);
    if (serial_count != 0) after_submit(queue, serials, serial_count, submitted_at);
}

bool allocate_plane(plane &target, ID3D12Device *device, const D3D12_RESOURCE_DESC &description)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    if (bytes == 0 || bytes == UINT64_MAX || bytes > 128ull * 1024 * 1024) return false;
    if (target.buffer != nullptr && target.bytes != bytes) release_plane(target);
    if (target.buffer == nullptr)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&target.buffer)))) return false;
    }
    target.description = description;
    target.footprint = footprint;
    target.bytes = bytes;
    return true;
}

}

void enable()
{
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return;
    const auto *name = std::wcsrchr(path, L'\\');
    if (name == nullptr || _wcsicmp(name + 1, L"HTGame.exe") != 0) return;
    data().active.store(true, std::memory_order_release);
    mgpu::diag::info("[MGPU][R323-ALIGN] enabled for HTGame: exact frame/viewport tuples; missing inputs hold neural output");
    mgpu::diag::info("[MGPU][R324-DEVICE] wrapper-aware device identity; same-frame owned snapshots with submission-fence handoffs");
    mgpu::diag::info("[MGPU][R325-DEPTH] full native plane capture, bounded active-region extraction to R32_FLOAT; normalization fence gates outgoing COPY");
    mgpu::diag::info("[MGPU][R329-TIMING] capture/depth GPU timestamps collected only after outgoing retirement; aggregates every 120 retired frames");
}
bool enabled() { return data().active.load(std::memory_order_acquire); }

void set_game_device(ID3D12Device *device)
{
    if (device == nullptr) return;
    mgpu::game_work::bind_device(device);
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    const bool was_bound = bool(context.game_device);
    if (!context.game_device.bind(device, device->GetAdapterLuid()))
    {
        if (was_bound)
        {
            context.failed = true;
            reject(context, "game device changed; existing staging cannot cross D3D12 devices");
        }
        return;
    }
    if (!was_bound)
    {
        char line[220];
        std::snprintf(line, sizeof line, "[MGPU][R324-DEVICE] presentation=%p identity=%p private_marker=%u",
                      device, context.game_device.canonical(), unsigned(context.game_device.marked()));
        mgpu::diag::info(line);
    }
}

void reset_command(ID3D12GraphicsCommandList *command)
{
    if (!enabled() || command == nullptr) return;
    std::uint64_t serial = 0;
    UINT size = sizeof serial;
    if (FAILED(command->GetPrivateData(command_guid, &size, &serial)) || size != sizeof serial) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    auto *record = find_command(context, serial);
    if (record != nullptr && !record->submitted && !record->submitting)
    {
        for (auto &frame : context.frames)
        {
            if (!frame.state.cancel(serial))
            {
                context.failed = true;
                reject(context, "transfer command reset before submission; output held");
            }
            for (unsigned component = 0; component != 3; ++component)
                if (frame.state.commands[component] == 0 && frame.state.copied[component].value == 0)
                {
                    release_plane(frame.planes[component]);
                }
        }
        record->submitted = true;
    }
    (void)command->SetPrivateData(command_guid, 0, nullptr);
}

DXGI_FORMAT depth_format(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    default: return format;
    }
}

void observe_queue(ID3D12CommandQueue *queue)
{
    if (!enabled() || queue == nullptr) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.hook_mutex);
    void **entry = *reinterpret_cast<void ***>(queue) + execute_slot;
    for (const auto &hook : context.hooks) if (hook.entry == entry) return;
    static const execute_fn thunks[] = {execute<0>, execute<1>, execute<2>, execute<3>,
                                       execute<4>, execute<5>, execute<6>, execute<7>};
    for (unsigned index = 0; index != context.hooks.size(); ++index)
    {
        auto &hook = context.hooks[index];
        if (hook.entry != nullptr) continue;
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(thunks[index]), &pinned)) return;
        DWORD protection = 0;
        if (!VirtualProtect(entry, sizeof(void *), PAGE_READWRITE, &protection)) return;
        hook.entry = entry;
        hook.original = reinterpret_cast<execute_fn>(*entry);
        InterlockedExchangePointer(entry, reinterpret_cast<void *>(thunks[index]));
        DWORD ignored = 0;
        VirtualProtect(entry, sizeof(void *), protection, &ignored);
        mgpu::diag::info("[MGPU][R323-ALIGN] submission observer installed; arrival signals run after ExecuteCommandLists returns");
        return;
    }
}

void capture(void *command_pointer, unsigned buffer_type, const tagged_resource &tag,
             motion_packet::identity engine)
{
    if (!enabled() || !engine.viewport_known || command_pointer == nullptr ||
        tag.lifecycle != 0 || tag.state == UINT_MAX || tag.native == 0) return;
    const unsigned component = buffer_type == 53 ? 0 : buffer_type == 0 ? 1 : buffer_type == 1 ? 2 : 3;
    if (component == 3) return;
    mgpu::measurement::cpu_scope measurement("capture_callback", engine.frame, component);
    if (engine.frame_known)
        mgpu::game_work::tag(static_cast<ID3D12GraphicsCommandList *>(command_pointer), engine.frame,
            engine.viewport, buffer_type, tag.width, tag.height);
    auto &context = data();
    auto *command = static_cast<ID3D12GraphicsCommandList *>(command_pointer);
    {
        mgpu::measurement::scoped_lock lock(context.mutex, "capture_track", engine.frame);
        static unsigned observations[3]{};
        if (observations[component]++ < 4)
        {
            char line[260];
            std::snprintf(line, sizeof line, "[MGPU][R323-TAG] type=%u cmd=%p frame=%u known=%u viewport=%u state=0x%x",
                          buffer_type, command, engine.frame, unsigned(engine.frame_known), engine.viewport, tag.state);
            mgpu::diag::info(line);
        }
        auto *record = record_command(context, command);
        if (record == nullptr) return;
        if (engine.frame_known) record->identity = engine;
        else
        {
            // Legacy tags may inherit only the explicit frame on this recording list and viewport.
            if (!record->identity.frame_known || record->identity.viewport != engine.viewport)
            {
                reject(context, "color/depth/motion has no explicit frame on this command list; no cross-list guess");
                return;
            }
            engine = record->identity;
        }
    }
    ID3D12Resource *resource = nullptr;
    D3D12_RESOURCE_DESC description{};
    if (!mgpu::tagged_motion::query_resource(reinterpret_cast<void *>(tag.native), resource, description))
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        report_input(context, component, buffer_type, tag, description, "ID3D12Resource query failed");
        return;
    }
    ID3D12Device *device = nullptr;
    const char *reason = texture_rejection(tag, description, component == 1);
    if (reason == nullptr && FAILED(resource->GetDevice(IID_PPV_ARGS(&device)))) reason = "GetDevice failed";
    if (reason != nullptr)
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        report_input(context, component, buffer_type, tag, description, reason);
        resource->Release();
        return;
    }
    mgpu::measurement::scoped_lock lock(context.mutex, "capture_record", engine.frame);
    report_input(context, component, buffer_type, tag, description, nullptr);
    const frame_key key{engine.frame, engine.viewport};
    frame_record *selected = nullptr;
    const auto done = [&](unsigned index) { return completed(context, index); };
    const LUID adapter = device->GetAdapterLuid();
    const bool matching_adapter = context.game_device.matches(device, adapter);
    if (context.game_device && !matching_adapter)
    {
        report_input(context, component, buffer_type, tag, description, "device identity mismatch");
        reject(context, "tagged texture and presentation do not share one D3D12 device");
    }
    if (matching_adapter && !context.viewport_known)
    {
        context.viewport = engine.viewport;
        context.viewport_known = true;
    }
    const bool newer = !context.delivered || newer_frame(key.frame, context.last_delivered.frame);
    if (!context.failed && matching_adapter && context.viewport == key.viewport && newer)
    {
        bool reserved = false;
        for (auto &frame : context.frames)
            if (frame.state.serial != 0 && frame.state.key == key)
            {
                reserved = frame.state.leased || frame.state.release_command != 0;
                if (!reserved) selected = &frame;
                break;
            }
        if (selected == nullptr && !reserved)
            for (auto &frame : context.frames)
                if (context.depth_converter.complete(frame.normalization) && (frame.state.reclaimable(done) ||
                    (frame.state.key.viewport == key.viewport &&
                     newer_frame(key.frame, frame.state.key.frame) &&
                     key.frame - frame.state.key.frame >= capacity &&
                     frame.state.abandoned_complete(done))))
                {
                    collect_timing(context, frame);
                    frame.state = {};
                    frame.state.key = key;
                    frame.state.serial = ++context.next_frame;
                    frame.measurement_session = mgpu::measurement::session();
                    selected = &frame;
                    break;
                }
    }
    if (selected == nullptr) ++context.held;
    else if (selected->state.commands[component] == 0)
    {
        auto *record = record_command(context, command);
        auto &target = selected->planes[component];
        if (record != nullptr && allocate_plane(target, device, description))
        {
            selected->capture_timing[component].begin(device, command, record->serial);
            const bool transition_source = (tag.state & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
            D3D12_RESOURCE_BARRIER transition{};
            transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Transition.pResource = resource;
            transition.Transition.Subresource = 0;
            transition.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(tag.state);
            transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            if (transition_source) command->ResourceBarrier(1, &transition);
            D3D12_RESOURCE_BARRIER target_transition{};
            target_transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            target_transition.Transition = {target.buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
            command->ResourceBarrier(1, &target_transition);
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = resource;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.pResource = target.buffer;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = target.footprint;
            command->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            std::swap(target_transition.Transition.StateBefore, target_transition.Transition.StateAfter);
            command->ResourceBarrier(1, &target_transition);
            std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
            if (transition_source) command->ResourceBarrier(1, &transition);
            selected->state.commands[component] = record->serial;
            if (component == 1) selected->depth_region = tag;
            selected->capture_timing[component].end(command);
        }
        else reject(context, "command tracking or staging allocation failed");
    }
    device->Release();
    resource->Release();
}

void set_ready_dispatch(bool enabled)
{
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (context.ready_dispatch == enabled) return;
    context.ready_dispatch = enabled;
    context.wake_slot = capacity;
    mgpu::diag::info(enabled
        ? "[MGPU][M342-DISPATCH] color_ready=1; worker event wake; all GPU dependencies retained; performance unverified"
        : "[MGPU][M342-DISPATCH] color_ready=0; original prequeue route");
}

HANDLE dispatch_event()
{
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (!enabled() || context.failed || !context.ready_dispatch || context.wake_failed ||
        context.wake_slot >= capacity) return nullptr;
    const auto &frame = context.frames[context.wake_slot];
    const auto ticket = frame.state.copied[0];
    if (frame.state.serial != context.wake_serial || !frame.state.recorded() ||
        ticket.queue >= queue_capacity || ticket.value != context.wake_value ||
        context.queues[ticket.queue].fence != context.wake_fence.Get()) return nullptr;
    return context.source_event;
}

bool acquire(lease &result)
{
    auto &context = data();
    mgpu::measurement::scoped_lock lock(context.mutex, "aligned_acquire");
    context.wake_slot = capacity;
    if (context.failed || !enabled()) return false;
    unsigned selected = capacity;
    for (unsigned index = 0; index != capacity; ++index)
    {
        auto &frame = context.frames[index];
        collect_timing(context, frame);
        if (!frame.state.recorded()) continue;
        if (context.delivered && !newer_frame(frame.state.key.frame, context.last_delivered.frame))
            continue;
        if (selected == capacity || newer_frame(context.frames[selected].state.key.frame, frame.state.key.frame))
            selected = index;
        const auto depth_ticket = frame.state.copied[1];
        if (depth_ticket.value == 0 || depth_ticket.value == UINT64_MAX ||
            depth_ticket.queue >= queue_capacity || context.queues[depth_ticket.queue].fence == nullptr)
            continue;
        if (frame.normalization.serial != frame.state.serial)
        {
            const auto status = context.depth_converter.submit(frame.planes[1], frame.depth_region,
                frame.normalized_depth, frame.normalization, frame.state.serial,
                context.queues[depth_ticket.queue].fence, depth_ticket.value);
            if (FAILED(status))
            {
                char line[180];
                std::snprintf(line, sizeof line, "[MGPU][R325-DEPTH] normalization failed: hr=0x%08lx frame=%u",
                              static_cast<unsigned long>(status), frame.state.key.frame);
                mgpu::diag::warn(line);
                context.failed = true;
                return false;
            }
            static unsigned conversions = 0;
            if (conversions++ < 4)
            {
                const auto &raw = frame.planes[1];
                char line[340];
                std::snprintf(line, sizeof line,
                    "[MGPU][R325-DEPTH] submitted frame=%u raw=%llux%u resource_format=%u plane_format=%u "
                    "crop=(%u,%u,%u,%u) output=%llux%u/R32_FLOAT fence=%llu",
                    frame.state.key.frame, raw.description.Width, raw.description.Height,
                    unsigned(raw.description.Format), unsigned(raw.footprint.Footprint.Format),
                    frame.depth_region.left, frame.depth_region.top, frame.depth_region.width, frame.depth_region.height,
                    frame.normalized_depth.description.Width, frame.normalized_depth.description.Height, frame.normalization.value);
                mgpu::diag::info(line);
            }
        }
    }
    if (selected != capacity)
    {
        const auto &frame = context.frames[selected];
        const auto &constants = context.constants[frame.state.key.frame % context.constants.size()];
        if (!frame.state.schedulable() || !constants.valid || !(constants.key == frame.state.key) ||
            frame.normalization.serial != frame.state.serial || frame.normalization.value == 0 ||
            frame.normalization.value == UINT64_MAX) selected = capacity;
    }
    if (selected == capacity)
    {
        const auto now = GetTickCount64();
        if (now - context.last_wait_log >= 5000)
        {
            context.last_wait_log = now;
            for (const auto &frame : context.frames)
            {
                unsigned recorded = 0, retired = 0;
                for (unsigned index = 0; index != 3; ++index)
                {
                    if (frame.state.commands[index] != 0) recorded |= 1u << index;
                    const auto ticket = frame.state.copied[index];
                    const auto finish = completed(context, ticket.queue);
                    if (ticket.value != 0 && finish != UINT64_MAX && finish >= ticket.value)
                        retired |= 1u << index;
                }
                const auto &constants = context.constants[frame.state.key.frame % context.constants.size()];
                char line[420];
                std::snprintf(line, sizeof line,
                    "[MGPU][R323-WAIT] serial=%llu frame=%u viewport=%u recorded_mask=%u gpu_complete_mask=%u constants=%u transfer=%llu depth_conversion=%llu conversion_complete=%u; bits color=1 depth=2 motion=4",
                    frame.state.serial, frame.state.key.frame, frame.state.key.viewport, recorded, retired,
                    unsigned(constants.valid && constants.key == frame.state.key), frame.state.release_command,
                    frame.normalization.serial, unsigned(context.depth_converter.complete(frame.normalization)));
                mgpu::diag::info(line);
            }
        }
        return false;
    }
    auto &frame = context.frames[selected];
    if (context.ready_dispatch)
    {
        const auto ticket = frame.state.copied[0];
        auto *fence = ticket.queue < queue_capacity ? context.queues[ticket.queue].fence : nullptr;
        const auto finished = fence != nullptr ? fence->GetCompletedValue() : UINT64_MAX;
        if (finished == UINT64_MAX)
        {
            context.failed = true;
            mgpu::diag::warn("[MGPU][M342-DISPATCH] invalid source fence/device removed; refusing source reuse");
            return false;
        }
        if (finished < ticket.value)
        {
            context.wake_slot = selected;
            context.wake_serial = frame.state.serial;
            if (!context.wake_failed &&
                (context.wake_fence.Get() != fence || context.wake_value != ticket.value))
            {
                if (context.source_event == nullptr)
                    context.source_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                const auto status = context.source_event != nullptr
                    ? fence->SetEventOnCompletion(ticket.value, context.source_event) : E_OUTOFMEMORY;
                if (FAILED(status))
                {
                    context.wake_failed = true;
                    mgpu::diag::warn("[MGPU][M342-DISPATCH] event registration failed; bounded worker polling retained");
                }
                else
                {
                    context.wake_fence = fence;
                    context.wake_value = ticket.value;
                    ++context.wake_registrations;
                }
            }
            return false;
        }
    }
    frame.acquired_ms = timing::cpu_ms(timing::cpu_ticks());
    frame.state.leased = true;
    result.slot = selected;
    result.serial = frame.state.serial;
    result.measurement_session = frame.measurement_session;
    result.key = frame.state.key;
    result.constants = context.constants[frame.state.key.frame % context.constants.size()];
    result.planes = frame.planes;
    result.planes[1] = frame.normalized_depth;
    if (context.ready_dispatch && (context.accepted < 3 || context.accepted % 120 == 0))
    {
        char report[320];
        std::snprintf(report, sizeof report,
            "[MGPU][M342-DISPATCH] serial=%llu engine_frame=%u color_ready=1 acquire_ms=%.6f registrations=%llu wake_failed=%u",
            frame.state.serial, frame.state.key.frame, frame.acquired_ms,
            context.wake_registrations, unsigned(context.wake_failed));
        mgpu::diag::info(report);
    }
    return true;
}

lease::~lease()
{
    if (serial == 0) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (slot < capacity && context.frames[slot].state.serial == serial)
        context.frames[slot].state.leased = false;
}

void commit_transfer(lease &source)
{
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (!source || source.slot >= capacity) return;
    context.last_delivered = source.key;
    context.delivered = true;
    context.frames[source.slot].state.leased = false;
    if (++context.accepted <= 6 || context.accepted % 300 == 0)
    {
        char line[780];
        const auto &color = source.planes[0].description;
        const auto &depth = source.planes[1].description;
        const auto &motion = source.planes[2].description;
        std::snprintf(line, sizeof line,
            "[MGPU][R323-ALIGN] tuple=%llu engine_frame=%u viewport=%u all_inputs_submitted=1 "
            "color=%llux%u/%u depth=%llux%u/%u motion=%llux%u/%u held=%llu "
            "normalized_motion_scale=(%.8f,%.8f) depth_inverted=%u engine_reset=%u "
            "jitter=(%.6f,%.6f) motion_dilated=%u spatial_constants=%u",
            context.accepted, source.key.frame, source.key.viewport,
            color.Width, color.Height, unsigned(color.Format), depth.Width, depth.Height,
            unsigned(depth.Format), motion.Width, motion.Height, unsigned(motion.Format), context.held,
            source.constants.motion_scale_x, source.constants.motion_scale_y,
            unsigned(source.constants.depth_inverted), unsigned(source.constants.reset),
            source.constants.jitter_x, source.constants.jitter_y,
            unsigned(source.constants.motion_dilated), unsigned(source.constants.spatial_valid));
        mgpu::diag::info(line);
    }
    source.serial = 0;
}

HRESULT submit_transfer(lease &source, ID3D12Resource *seal_upload, UINT64 seal_source,
    ID3D12Resource *target, UINT64 seal_target, UINT64 seal_bytes, const std::array<UINT64, 3> &offsets,
    ID3D12Fence *arrival, UINT64 arrival_value, ID3D12Fence *color_arrival, ID3D12Fence *depth_arrival)
{
    auto &context = data();
    mgpu::measurement::scoped_lock lock(context.mutex, "transfer_submit", source.key.frame);
    if (context.failed || !source || source.slot >= capacity || target == nullptr) return E_INVALIDARG;
    auto &frame = context.frames[source.slot];
    if (frame.state.serial != source.serial || !frame.state.leased || frame.state.release_command != 0) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    HRESULT result = target->GetDevice(IID_PPV_ARGS(&device));
    if (SUCCEEDED(result)) result = context.outgoing.initialize(device.Get());
    if (FAILED(result)) return result;
    if (context.outgoing_queue == queue_capacity)
    {
        for (unsigned index = 0; index < queue_capacity; ++index)
            if (context.queues[index].queue == nullptr)
            {
                context.queues[index] = {context.outgoing.queue(), context.outgoing.fence(), 0};
                context.queues[index].queue->AddRef();
                context.queues[index].fence->AddRef();
                context.outgoing_queue = index;
                break;
            }
        if (context.outgoing_queue == queue_capacity) return E_OUTOFMEMORY;
        mgpu::diag::info("[MGPU][R328-TRANSFER] dedicated GPU0 COPY queue; same-frame payload and seal; no cross-adapter copy on the game's DIRECT list");
    }
    std::array<mgpu::producer_copy::range, 4> copies{};
    copies[0] = {seal_upload, seal_source, seal_target, seal_bytes};
    std::array<mgpu::producer_copy::dependency, 3> dependencies{};
    for (unsigned index = 0; index < 3; ++index)
    {
        copies[index + 1] = {source.planes[index].buffer, 0, offsets[index], source.planes[index].bytes};
        const auto ticket = frame.state.copied[index];
        if (ticket.queue >= queue_capacity || ticket.value == 0) return E_INVALIDARG;
        dependencies[index] = {context.queues[ticket.queue].fence, ticket.value};
        if (dependencies[index].fence == nullptr) return E_INVALIDARG;
    }
    dependencies[1] = {context.depth_converter.fence(), frame.normalization.value};
    if (dependencies[1].fence == nullptr || dependencies[1].value == 0) return E_INVALIDARG;
    UINT64 retirement = 0;
    const auto transfer_submit_ms = timing::cpu_ms(timing::cpu_ticks());
    result = context.outgoing.submit(copies, static_cast<unsigned>(copies.size()), target, arrival,
        arrival_value, retirement, dependencies, color_arrival, depth_arrival);
    if (FAILED(result))
    {
        if (result != DXGI_ERROR_WAS_STILL_DRAWING) context.failed = true;
        return result;
    }
    frame.state.release_command = ++context.next_command;
    frame.state.released = {context.outgoing_queue, retirement};
    frame.transfer_submitted_ms = transfer_submit_ms;
    frame.seal = arrival_value;
    frame.timing_collected = false;
    if (frame.measurement_session != 0)
    {
        static UINT64 last_session = 0;
        if (last_session != frame.measurement_session)
        {
            last_session = frame.measurement_session;
            mgpu::measurement::queue(last_session, "producer_outgoing", context.outgoing.queue());
        }
        char report[900];
        std::snprintf(report, sizeof report,
            "[MGPU][M336] kind=handoff session=%llu seal=%llu serial=%llu engine_frame=%u "
            "out_queue=%p out_fence=%p retirement=%llu arrival_fence=%p arrival_value=%llu "
            "color_fence=%p color_value=%llu depth_fence=%p depth_value=%llu motion_fence=%p motion_value=%llu",
            frame.measurement_session, arrival_value, frame.state.serial, source.key.frame,
            context.outgoing.queue(), context.outgoing.fence(), retirement, arrival, arrival_value,
            dependencies[0].fence, dependencies[0].value, dependencies[1].fence, dependencies[1].value,
            dependencies[2].fence, dependencies[2].value);
        mgpu::measurement::emit(report);
    }
    if (arrival_value <= 3 || arrival_value % 120ull == 0)
    {
        UINT64 bytes = 0;
        for (const auto &copy : copies) bytes += copy.bytes;
        char line[400];
        std::snprintf(line, sizeof line,
            "[MGPU][R334-TRANSFER] seal=%llu engine_frame=%u bytes=%llu dependency_count=%u copy_gpu_samples=%llu copy_gpu_ms_mean=%.4f "
            "source_release=%llu arrival_after_execute=1; GPU copy time excludes capture/normalization/queue waits",
            arrival_value, source.key.frame, bytes, unsigned(dependencies.size()), context.outgoing.measured(),
            context.outgoing.measured() != 0 ? context.outgoing.gpu_ms() / double(context.outgoing.measured()) : 0.0,
            retirement);
        mgpu::diag::info(line);
        const auto &wait = context.outgoing.queue_wait();
        std::snprintf(line, sizeof line,
            "[MGPU][R329-COPY] seal=%llu engine_frame=%u queue_wait_n=%llu invalid=%llu queue_wait_sum_ms=%.6f queue_wait_max_ms=%.6f; cumulative submit-to-GPU-start, separate from copy execution",
            arrival_value, source.key.frame, wait.count, wait.invalid, wait.sum, wait.maximum);
        mgpu::diag::info(line);
    }
    return S_OK;
}

void capture_constants(const guide_constants &constants)
{
    if (!enabled()) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    context.constants[constants.key.frame % context.constants.size()] = constants;
    static unsigned reports = 0;
    if (reports++ < 4)
    {
        char line[260];
        std::snprintf(line, sizeof line,
            "[MGPU][R324-CONSTANTS] frame=%u viewport=%u valid=%u scale=(%.8f,%.8f) inverted=%u reset=%u",
            constants.key.frame, constants.key.viewport, unsigned(constants.valid), constants.motion_scale_x,
            constants.motion_scale_y, unsigned(constants.depth_inverted), unsigned(constants.reset));
        mgpu::diag::info(line);
    }
}

bool outstanding()
{
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (context.outgoing.pending()) return true;
    const auto done = [&](unsigned index) { return completed(context, index); };
    for (const auto &frame : context.frames)
        if (!context.depth_converter.complete(frame.normalization) ||
            (frame.state.release_command != 0 && !frame.state.reclaimable(done))) return true;
    return context.failed;
}

std::uint64_t submitted_color_frame()
{
    return latest_color_submission.load(std::memory_order_acquire);
}

void stop()
{
    data().active.store(false, std::memory_order_release);
}

bool matches(const plane &input, unsigned width, unsigned height, DXGI_FORMAT format,
             const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint, UINT64 bytes)
{
    return input.description.Width == width && input.description.Height == height &&
           input.footprint.Footprint.Format == format && input.bytes == bytes &&
           input.footprint.Footprint.RowPitch == footprint.Footprint.RowPitch &&
           input.footprint.Footprint.Depth == 1;
}
}
