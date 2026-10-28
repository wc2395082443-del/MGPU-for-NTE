#include "official_runtime.hpp"
#include "official_channel.hpp"
#include "shared_constants.hpp"
#include "shared_frame_transport.hpp"
#include "output_window.hpp"
#include "output_trace.hpp"
#include <thread>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <filesystem>

using Microsoft::WRL::ComPtr;
using namespace mgpu::official_fg;

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t clock_ns()
{
    static const auto frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart; }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<std::uint64_t>(double(now.QuadPart) * 1e9 / double(frequency));
}

void transition(ID3D12GraphicsCommandList *commands, ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &barrier);
}

struct source_relay
{
    official_channel &channel;
    official_runtime &runtime;
    std::mutex history_mutex;
    source_history history;
    std::atomic<unsigned> latest_sleep{0};
    std::thread thread;
    source_relay(official_channel &shared, official_runtime &sdk) : channel(shared), runtime(sdk) {}
    ~source_relay() { stop(); }
    void start()
    {
        thread = std::thread([this] {
            try {
                while (WaitForSingleObject(channel_handle(channel.stop_event), 0) == WAIT_TIMEOUT) {
                    auto consumed = read_atomic(channel.markers_read);
                    const auto written = read_atomic(channel.markers_written);
                    if (consumed == written) {
                        {
                            std::lock_guard<std::mutex> lock(history_mutex);
                            refresh_retention();
                        }
                        HANDLE waits[] = {channel_handle(channel.markers_event), channel_handle(channel.stop_event),
                            channel_handle(channel.parent_process)};
                        const DWORD result = WaitForMultipleObjects(3, waits, FALSE, 20);
                        if (result == WAIT_OBJECT_0 + 1 || result == WAIT_OBJECT_0 + 2) break;
                        continue;
                    }
                    require(written >= consumed && written - consumed <= marker_capacity, "source marker ring overflow");
                    const source_marker marker = channel.markers[consumed % marker_capacity];
                    require(marker.serial == static_cast<std::uint64_t>(consumed + 1), "source marker serial mismatch");
                    const auto operation_start = GetTickCount64();
                    if (marker.serial <= 15) std::printf("[M349-SOURCE] begin serial=%llu frame=%u kind=%u\n",
                        static_cast<unsigned long long>(marker.serial), marker.frame, marker.kind);
                    bool accepted = false;
                    source_history updated;
                    {
                        std::lock_guard<std::mutex> lock(history_mutex);
                        refresh_retention();
                        updated = history;
                        accepted = marker.kind == input_ping_marker || updated.note(marker.frame, marker.kind);
                    }
                    if (accepted) {
                        marker_timing timing{marker.native_begin_ns, marker.native_end_ns, marker.capture_ns};
                        if (marker.kind == sleep_marker) {
                            timing.dispatch_begin_ns = clock_ns();
                            runtime.source_sleep(marker.frame);
                            timing.dispatch_end_ns = clock_ns();
                            latest_sleep.store(marker.frame);
                            InterlockedCompareExchange(&channel.first_source_frame, static_cast<LONG>(marker.frame), 0);
                        } else if (relay_marker_to_sdk(marker.kind)) {
                            timing.dispatch_begin_ns = clock_ns();
                            runtime.marker(marker.frame, marker.kind);
                            timing.dispatch_end_ns = clock_ns();
                        }
                        if (marker.kind == input_ping_marker) {
                            InterlockedIncrement64(&channel.source_pings);
                        } else {
                            std::lock_guard<std::mutex> lock(history_mutex);
                            refresh_retention();
                            require(history.note(marker.frame, marker.kind), "real source marker commit lost ordering");
                            require(history.record_timing(marker.frame, marker.kind, timing), "source timing identity lost");
                        }
                    }
                    if (marker.kind == sleep_marker) {
                        InterlockedExchange64(&channel.sleep_completed, marker.serial);
                        SetEvent(channel_handle(channel.sleep_event));
                    }
                    if (marker.serial <= 15) std::printf("[M349-SOURCE] end serial=%llu accepted=%u elapsed_ms=%llu\n",
                        static_cast<unsigned long long>(marker.serial), accepted ? 1u : 0u,
                        static_cast<unsigned long long>(GetTickCount64() - operation_start));
                    InterlockedExchange64(&channel.markers_read, ++consumed);
                }
            } catch (const std::exception &error) {
                strncpy_s(channel.error, error.what(), _TRUNCATE);
                InterlockedExchange(&channel.state, static_cast<LONG>(host_state::failed));
                SetEvent(channel_handle(channel.stop_event));
                SetEvent(channel_handle(channel.sleep_event));
            }
        });
    }
    bool simulation_ready(unsigned frame)
    {
        std::lock_guard<std::mutex> lock(history_mutex);
        return history.simulation_ready(frame);
    }
    unsigned bits(unsigned frame)
    {
        std::lock_guard<std::mutex> lock(history_mutex);
        return history.bits(frame);
    }
    source_reflex_timing timing(unsigned frame)
    {
        std::lock_guard<std::mutex> lock(history_mutex);
        return history.timing(frame);
    }
    void refresh_retention()
    {
        std::array<std::uint32_t, shared_slots> frames{};
        if (published_source_frames(channel, frames)) history.retain(frames);
    }
    void diagnose(unsigned frame, std::uint64_t sequence)
    {
        std::lock_guard<std::mutex> lock(history_mutex);
        std::printf("[M355-SOURCE] seq=%llu frame=%u bits=0x%x retained=%u rolling_owner=%u latest_sleep=%u published=%lld\n",
            static_cast<unsigned long long>(sequence), frame, history.bits(frame), history.retained(frame) ? 1u : 0u,
            history.cached_frame(frame), latest_sleep.load(), read_atomic(channel.published));
    }
    void stop()
    {
        if (!thread.joinable()) return;
        SetEvent(channel_handle(channel.stop_event));
        if (WaitForSingleObject(thread.native_handle(), 2500) != WAIT_OBJECT_0) {
            std::printf("[M349] source relay did not stop; terminating helper only\n");
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 92);
        }
        thread.join();
    }
};

class presenter
{
public:
    presenter(official_runtime &runtime, shared_frame_transport &transport)
        : runtime_(runtime), transport_(transport)
    {
        auto *device = runtime.device();
        require_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retired_)), "present retirement fence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event_ != nullptr, "retirement event");
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        for (auto &slot : slots_) {
            require_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)), "private allocator");
            require_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                IID_PPV_ARGS(&slot.commands)), "private commands");
            require_hr(slot.commands->Close(), "initial close");
            for (unsigned plane = 0; plane < shared_planes; ++plane) {
                const auto description = shared_frame_transport::texture_description(transport.manifest().descriptions[plane]);
                require_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.planes[plane])), "private FG input");
            }
        }
    }
    ~presenter() { if (event_) CloseHandle(event_); }
    double present(const frame_packet &packet, official_channel &channel, bool first, output_frame_trace &trace, bool measure_present)
    {
        auto &slot = slots_[slot_for(packet.sequence)];
        wait(slot.retired);
        trace.mark(output_stage::slot_ready);
        trace.helper_render_begin_ns = clock_ns();
        runtime_.marker(packet.color_frame, static_cast<unsigned>(sl::PCLMarker::eRenderSubmitStart));
        if (slot.official_fence && slot.official_value)
            require_hr(runtime_.queue()->Wait(slot.official_fence.Get(), slot.official_value), "official input-read retirement");
        require_hr(slot.allocator->Reset(), "retired allocator reset");
        require_hr(slot.commands->Reset(slot.allocator.Get(), nullptr), "command reset");
        require_hr(transport_.wait_ready(runtime_.queue(), packet.sequence), "NR export fence wait");
        std::array<texture_binding, shared_planes> destinations{};
        for (unsigned plane = 0; plane < shared_planes; ++plane)
            destinations[plane] = {slot.planes[plane].Get(), slot.states[plane]};
        require_hr(transport_.record_read(slot.commands.Get(), slot_for(packet.sequence), destinations), "import NR tuple");
        const unsigned backbuffer_index = runtime_.swapchain()->GetCurrentBackBufferIndex();
        ComPtr<ID3D12Resource> backbuffer;
        require_hr(runtime_.swapchain()->GetBuffer(backbuffer_index, IID_PPV_ARGS(&backbuffer)), "official backbuffer");
        transition(slot.commands.Get(), slot.planes[0].Get(), slot.states[0], D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(slot.commands.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        slot.commands->CopyResource(backbuffer.Get(), slot.planes[0].Get());
        trace.mark(output_stage::osd_begin);
        trace.mark(output_stage::osd_end);
        transition(slot.commands.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        transition(slot.commands.Get(), slot.planes[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, slot.states[0]);
        for (unsigned plane = 1; plane < shared_planes; ++plane) {
            transition(slot.commands.Get(), slot.planes[plane].Get(), slot.states[plane], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            slot.states[plane] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        require_hr(slot.commands->Close(), "close imported frame");
        trace.mark(output_stage::recorded);
        ID3D12CommandList *lists[] = {slot.commands.Get()};
        runtime_.queue()->ExecuteCommandLists(1, lists);
        require_hr(transport_.signal_released(runtime_.queue(), packet.sequence), "release shared NR slot after private copy");
        runtime_.marker(packet.color_frame, static_cast<unsigned>(sl::PCLMarker::eRenderSubmitEnd));
        trace.helper_render_end_ns = clock_ns();
        trace.mark(output_stage::submitted);
        sl::Constants constants{};
        require(unpack_constants(packet, transport_.manifest().generation, constants), "same-frame constants invalid");
        if (first) constants.reset = sl::eTrue;
        runtime_.constants(constants, packet.color_frame);
        sl::Resource depth(sl::ResourceType::eTex2d, slot.planes[1].Get(), slot.states[1]);
        sl::Resource motion(sl::ResourceType::eTex2d, slot.planes[2].Get(), slot.states[2]);
        const auto &descriptions = transport_.manifest().descriptions;
        sl::Extent depth_extent{0, 0, descriptions[1].width, descriptions[1].height};
        sl::Extent motion_extent{0, 0, descriptions[2].width, descriptions[2].height};
        sl::ResourceTag tags[] = {
            {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &depth_extent},
            {&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &motion_extent}
        };
        runtime_.tag(packet.color_frame, tags, 2);
        runtime_.marker(packet.color_frame, static_cast<unsigned>(sl::PCLMarker::ePresentStart));
        trace.mark(output_stage::tagged);
        const HRESULT result = runtime_.swapchain()->Present(0, 0);
        trace.mark(output_stage::present_returned);
        runtime_.marker(packet.color_frame, static_cast<unsigned>(sl::PCLMarker::ePresentEnd));
        require_hr(result, "official proxy Present");
        const auto fg = runtime_.state();
        trace.sdk_reported = fg.numFramesActuallyPresented;
        trace.sdk_status = static_cast<unsigned>(fg.status);
        UINT present_count = 0;
        if (measure_present && SUCCEEDED(runtime_.swapchain()->GetLastPresentCount(&present_count))) trace.present_count = present_count;
        trace.present_result = static_cast<unsigned>(result);
        trace.mark(output_stage::state_read);
        InterlockedExchange(&channel.sdk_status, static_cast<LONG>(fg.status));
        require(fg.status == sl::DLSSGStatus::eOk, "official FG reports invalid integration");
        slot.official_fence = static_cast<ID3D12Fence *>(fg.inputsProcessingCompletionFence);
        slot.official_value = fg.lastPresentInputsProcessingCompletionFenceValue;
        slot.retired = ++next_retired_;
        require_hr(runtime_.queue()->Signal(retired_.Get(), slot.retired), "presentation queue retirement");
        InterlockedIncrement64(&channel.real_presents);
        InterlockedAdd64(&channel.sdk_presents, fg.numFramesActuallyPresented);
        InterlockedExchange64(&channel.last_source_frame, packet.color_frame);
        if (result == S_OK && fg.numFramesActuallyPresented > 0) InterlockedExchange(&channel.presenting, 1);
        trace.mark(output_stage::finished);
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const double age = double(now.QuadPart) * 1000.0 / double(frequency.QuadPart) - double(packet.capture_ns) / 1e6;
        if (packet.sequence <= 3 || packet.sequence % 120 == 0) {
            std::printf("[M349-PRESENT] seq=%llu source=%u sdk_reported=%u status=0x%x source_age_ms=%.3f; SDK count not scanout proof\n",
                static_cast<unsigned long long>(packet.sequence), packet.color_frame, fg.numFramesActuallyPresented,
                static_cast<unsigned>(fg.status), age);
        }
        return age;
    }
    void drain()
    {
        if (!runtime_.queue()) return;
        const auto value = ++next_retired_;
        require_hr(runtime_.queue()->Signal(retired_.Get(), value), "drain presenting queue");
        wait(value);
    }
private:
    struct input_slot
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        std::array<ComPtr<ID3D12Resource>, shared_planes> planes;
        std::array<D3D12_RESOURCE_STATES, shared_planes> states{};
        ComPtr<ID3D12Fence> official_fence;
        std::uint64_t official_value = 0, retired = 0;
    };
    void wait(std::uint64_t value)
    {
        if (!value) return;
        const auto completed = retired_->GetCompletedValue();
        require(completed != UINT64_MAX, "present device removed");
        if (completed >= value) return;
        require_hr(retired_->SetEventOnCompletion(value, event_), "retirement notification");
        require(WaitForSingleObject(event_, 3000) == WAIT_OBJECT_0, "present retirement timeout");
        require(retired_->GetCompletedValue() != UINT64_MAX && retired_->GetCompletedValue() >= value, "retirement not complete");
    }
    official_runtime &runtime_;
    shared_frame_transport &transport_;
    std::array<input_slot, shared_slots> slots_{};
    ComPtr<ID3D12Fence> retired_;
    std::uint64_t next_retired_ = 0;
    HANDLE event_ = nullptr;
};

int run(official_channel &channel, const std::filesystem::path &directory)
{
    require(channel.version == channel_version && channel.byte_size == sizeof(channel), "channel protocol mismatch");
    auto *parent = static_cast<HWND>(channel_handle(channel.parent_window));
    official_runtime runtime;
    LUID adapter{channel.manifest.adapter_low, channel.manifest.adapter_high};
    auto logs = std::filesystem::temp_directory_path() / (L"mgpu-official-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(logs);
    runtime.initialize(directory / L"runtime", logs, adapter, parent, channel.parent_pid);
    output_window window;
    window.create(parent, channel.parent_pid);
    const auto color = channel.manifest.descriptions[0];
    unsigned multiplier = static_cast<unsigned>(read_atomic(channel.multiplier));
    runtime.create_swapchain(window.handle(), color.width, color.height, static_cast<DXGI_FORMAT>(color.format), multiplier);
    shared_frame_transport transport;
    require_hr(transport.open(runtime.device(), channel.manifest), "open exported NR textures");
    presenter output(runtime, transport);
    const auto trace_path = logs / L"output-stages.csv";
    output_trace trace(trace_path);
    std::printf("[M356] helper render markers bracket actual import submission; native simulation retained; built-in output OSD removed; CPU-stage trace: %ls; trigger required, limit=%u frames, not GPU execution times\n",
        trace_path.c_str(), output_trace::capacity);
    source_relay source(channel, runtime);
    source.start();
    consumer_schedule schedule(channel.manifest.generation);
    InterlockedExchange(&channel.maximum_multiplier, runtime.maximum_multiplier());
    InterlockedExchange(&channel.state, static_cast<LONG>(host_state::ready));
    std::uint64_t next = 1;
    const auto started = GetTickCount64();
    auto last_heartbeat_log = started;
    rate_meter metrics;
    std::int64_t input_poll_wait_ticks = 0;
    try {
        while (WaitForSingleObject(channel_handle(channel.stop_event), 0) == WAIT_TIMEOUT &&
            WaitForSingleObject(channel_handle(channel.parent_process), 0) == WAIT_TIMEOUT) {
            InterlockedExchange64(&channel.heartbeat_ms, GetTickCount64());
            trace.poll();
            if (GetTickCount64() - last_heartbeat_log >= 5000) {
                std::printf("[M350-HEARTBEAT] state=%ld published=%lld next=%llu real=%lld sdk=%lld source_first=%ld markers=%lld/%lld failures=%ld\n",
                    read_atomic(channel.state), read_atomic(channel.published), static_cast<unsigned long long>(next),
                    read_atomic(channel.real_presents), read_atomic(channel.sdk_presents), read_atomic(channel.first_source_frame),
                    read_atomic(channel.markers_read), read_atomic(channel.markers_written), read_atomic(channel.marker_failures));
                last_heartbeat_log = GetTickCount64();
            }
            window.update(parent, read_atomic(channel.visible) != 0 && read_atomic(channel.real_presents) > 0);
            const bool visible_output = read_atomic(channel.visible) != 0 && IsWindowVisible(parent) &&
                GetForegroundWindow() == GetAncestor(parent, GA_ROOT);
            rate_snapshot sample{};
            if (metrics.update(GetTickCount64(), read_atomic(channel.real_presents), read_atomic(channel.sdk_presents), visible_output, sample)) {
                publish_metrics(channel.current_metrics, sample);
                if (sample.visible && sample.valid) publish_metrics(channel.last_visible_metrics, sample);
            }
            if (read_atomic(channel.marker_failures)) throw std::runtime_error("source Reflex relay timed out or overflowed");
            if (!read_atomic(channel.first_source_frame) && GetTickCount64() - started > 20000)
                throw std::runtime_error("no live source Reflex sleep hook; refusing fabricated source timing");
            if (read_atomic(channel.published) < static_cast<LONG64>(next)) {
                HANDLE waits[] = {channel_handle(channel.frames_event), channel_handle(channel.stop_event),
                    channel_handle(channel.parent_process)};
                LARGE_INTEGER wait_begin{}, wait_end{};
                QueryPerformanceCounter(&wait_begin);
                MsgWaitForMultipleObjects(3, waits, FALSE, 4, QS_ALLINPUT);
                QueryPerformanceCounter(&wait_end);
                input_poll_wait_ticks += wait_end.QuadPart - wait_begin.QuadPart;
                continue;
            }
            const frame_packet packet = channel.frames[slot_for(next)];
            output_frame_trace frame{};
            frame.sequence = packet.sequence;
            frame.source = packet.color_frame;
            frame.capture_ns = packet.capture_ns;
            frame.camera = packet.camera;
            frame.input_poll_wait_ticks = input_poll_wait_ticks;
            input_poll_wait_ticks = 0;
            frame.backlog = static_cast<std::uint64_t>(read_atomic(channel.published)) - next + 1;
            frame.visible = visible_output ? 1 : 0;
            frame.mark(output_stage::acquired);
            require(schedule.acquire(packet), "published tuple identity mismatch");
            const auto deadline = GetTickCount64() + 100;
            while (!source.simulation_ready(packet.color_frame) && GetTickCount64() < deadline &&
                WaitForSingleObject(channel_handle(channel.stop_event), 1) == WAIT_TIMEOUT) {}
            if (!source.simulation_ready(packet.color_frame)) {
                source.diagnose(packet.color_frame, next);
                std::printf("[M349-SOURCE] incomplete frame=%u bits=0x%x written=%lld read=%lld failures=%ld stop=%lu\n",
                    packet.color_frame, source.bits(packet.color_frame), read_atomic(channel.markers_written),
                    read_atomic(channel.markers_read), read_atomic(channel.marker_failures),
                    WaitForSingleObject(channel_handle(channel.stop_event), 0));
                throw std::runtime_error("missing real source simulation markers");
            }
            const unsigned wanted = static_cast<unsigned>(read_atomic(channel.multiplier));
            if (wanted != multiplier) {
                runtime.configure(wanted);
                multiplier = wanted;
            }
            frame.requested = wanted;
            frame.reflex = source.timing(packet.color_frame);
            frame.source_pings = static_cast<std::uint64_t>(read_atomic(channel.source_pings));
            frame.mark(output_stage::markers_ready);
            if (next <= 3) source.diagnose(packet.color_frame, next);
            metrics.source_age(output.present(packet, channel, next == 1, frame, trace.pipeline_requested()));
            trace.record(frame);
            require(schedule.release(next), "ordered private-copy release");
            ++next;
        }
        source.stop();
        output.drain();
        InterlockedExchange(&channel.presenting, 0);
        window.update(parent, false);
        runtime.close();
        if (read_atomic(channel.state) != static_cast<LONG>(host_state::failed))
            InterlockedExchange(&channel.state, static_cast<LONG>(host_state::stopped));
        return 0;
    } catch (...) {
        source.stop();
        try { output.drain(); } catch (...) {
            std::printf("[M349] helper GPU work did not retire; terminating helper only\n");
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 93);
        }
        throw;
    }
}
}

int wmain(int count, wchar_t **arguments)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (count != 3 || std::wstring(arguments[1]) != L"--channel") return 2;
    wchar_t *end = nullptr;
    const auto mapping = std::wcstoull(arguments[2], &end, 10);
    if (!mapping || !end || *end) return 2;
    auto *channel = static_cast<official_channel *>(MapViewOfFile(channel_handle(mapping), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(official_channel)));
    if (!channel) return 3;
    int result = 1;
    try {
        auto log = std::filesystem::temp_directory_path() / (L"mgpu-official-host-" + std::to_wstring(GetCurrentProcessId()) + L".log");
        require(_wfreopen(log.c_str(), L"w", stdout) != nullptr, "live-readable helper log");
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        wchar_t executable[32768]{};
        require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "host path");
        result = run(*channel, std::filesystem::path(executable).parent_path());
    } catch (const std::exception &error) {
        strncpy_s(channel->error, error.what(), _TRUNCATE);
        InterlockedExchange(&channel->state, static_cast<LONG>(host_state::failed));
        InterlockedExchange(&channel->presenting, 0);
        SetEvent(channel_handle(channel->sleep_event));
        std::printf("[M349] FAILED: %s\n", error.what());
    }
    UnmapViewOfFile(channel);
    return result;
}
