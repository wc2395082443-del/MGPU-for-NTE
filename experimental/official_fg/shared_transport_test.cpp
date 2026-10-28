#include "shared_frame_transport.hpp"
#include "shared_constants.hpp"
#include "probe_policy.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace mgpu::official_fg;

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
void check(HRESULT result, const char *message)
{
    if (FAILED(result)) {
        std::printf("HRESULT 0x%08lx at %s\n", static_cast<unsigned long>(result), message);
        throw std::runtime_error(message);
    }
}

struct handle_owner
{
    HANDLE value = nullptr;
    explicit handle_owner(HANDLE handle = nullptr) : value(handle) {}
    handle_owner(const handle_owner &) = delete;
    handle_owner &operator=(const handle_owner &) = delete;
    ~handle_owner() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct mapped_view
{
    void *value = nullptr;
    ~mapped_view() { if (value) UnmapViewOfFile(value); }
};

struct child_process
{
    PROCESS_INFORMATION info{};
    ~child_process()
    {
        if (info.hProcess) {
            if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(info.hProcess, 90);
                WaitForSingleObject(info.hProcess, 5000);
            }
            CloseHandle(info.hProcess);
        }
        if (info.hThread) CloseHandle(info.hThread);
    }
};

struct control_block
{
    shared_manifest manifest{};
    std::uint64_t ready_event = 0, done_event = 0, parent_process = 0;
    std::array<frame_packet, shared_slots> packets{};
    alignas(8) volatile LONG64 published = 0;
    alignas(8) volatile LONG64 checked = 0;
    volatile LONG failed = 0;
    std::uint32_t total_frames = 12, exit_without_release = 0;
    char error[240]{};
};

std::uint64_t integer_handle(HANDLE handle) { return reinterpret_cast<std::uintptr_t>(handle); }
HANDLE native_handle(std::uint64_t handle) { return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle)); }
LONG64 load(volatile LONG64 &value) { return InterlockedCompareExchange64(&value, 0, 0); }

ComPtr<ID3D12Device> make_device(bool warp, const shared_manifest *manifest = nullptr)
{
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter1> adapter;
    if (manifest) {
        LUID luid{manifest->adapter_low, manifest->adapter_high};
        check(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "same adapter LUID");
    } else if (warp) {
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    } else {
        unsigned matched = 0;
        for (unsigned index = 0; ; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const auto result = factory->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            check(result, "adapter enumeration");
            DXGI_ADAPTER_DESC1 description{};
            check(candidate->GetDesc1(&description), "adapter description");
            if (matches_adapter(description.VendorId, (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0,
                description.Description)) { adapter = candidate; ++matched; }
        }
        require(matched == 1, "exactly one RTX 5080 required");
    }
    DXGI_ADAPTER_DESC1 description{};
    check(adapter->GetDesc1(&description), "selected adapter description");
    if (manifest && !warp) require(matches_adapter(description.VendorId,
        (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0, description.Description), "child adapter is not 5080");
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
    std::printf("DEVICE pid=%lu name=%ls luid=%08lx:%08lx\n", GetCurrentProcessId(), description.Description,
        static_cast<unsigned long>(description.AdapterLuid.HighPart), description.AdapterLuid.LowPart);
    return device;
}

void wait_fence(ID3D12Fence *fence, std::uint64_t value)
{
    auto completed = fence->GetCompletedValue();
    require(completed != UINT64_MAX, "device removed");
    if (completed >= value) return;
    handle_owner event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    require(event.value != nullptr, "fence event");
    check(fence->SetEventOnCompletion(value, event.value), "fence notification");
    require(WaitForSingleObject(event.value, 5000) == WAIT_OBJECT_0, "GPU fence timeout");
    completed = fence->GetCompletedValue();
    require(completed != UINT64_MAX && completed >= value, "invalid GPU completion");
}

void barrier(ID3D12GraphicsCommandList *commands, ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER entry{};
    entry.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    entry.Transition.pResource = resource;
    entry.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    entry.Transition.StateBefore = before;
    entry.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &entry);
}

struct graphics_queue
{
    ComPtr<ID3D12CommandQueue> queue;
    std::array<ComPtr<ID3D12CommandAllocator>, shared_slots> allocators{};
    std::array<ComPtr<ID3D12GraphicsCommandList>, shared_slots> commands{};
    explicit graphics_queue(ID3D12Device *device)
    {
        D3D12_COMMAND_QUEUE_DESC description{};
        description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue)), "command queue");
        for (unsigned slot = 0; slot < shared_slots; ++slot) {
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[slot])), "allocator");
            check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[slot].Get(), nullptr,
                IID_PPV_ARGS(&commands[slot])), "commands");
            check(commands[slot]->Close(), "initial close");
        }
    }
    ID3D12GraphicsCommandList *begin(unsigned slot)
    {
        check(allocators[slot]->Reset(), "allocator reset after completion");
        check(commands[slot]->Reset(allocators[slot].Get(), nullptr), "command reset");
        return commands[slot].Get();
    }
    void submit(unsigned slot)
    {
        check(commands[slot]->Close(), "close");
        ID3D12CommandList *lists[] = {commands[slot].Get()};
        queue->ExecuteCommandLists(1, lists);
    }
};

ComPtr<ID3D12Resource> make_buffer(ID3D12Device *device, D3D12_HEAP_TYPE type, UINT64 bytes)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&resource)), "test staging buffer");
    return resource;
}

struct test_plane
{
    ComPtr<ID3D12Resource> texture, staging;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total_bytes = 0;
    void initialize(ID3D12Device *device, plane_description specification, bool upload)
    {
        auto description = shared_frame_transport::texture_description(specification);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)), "private test texture");
        device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, &rows, &row_bytes, &total_bytes);
        staging = make_buffer(device, upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK, total_bytes);
    }
    static unsigned char expected(std::uint64_t sequence, unsigned plane, unsigned row, UINT64 column)
    {
        return static_cast<unsigned char>((sequence * 17 + plane * 31 + row * 7 + column) % 251);
    }
    void fill(std::uint64_t sequence, unsigned plane)
    {
        void *mapped = nullptr;
        D3D12_RANGE no_read{0, 0};
        check(staging->Map(0, &no_read, &mapped), "test upload map");
        auto *bytes = static_cast<unsigned char *>(mapped);
        for (unsigned row = 0; row < rows; ++row)
            for (UINT64 column = 0; column < row_bytes; ++column)
                bytes[footprint.Offset + static_cast<UINT64>(row) * footprint.Footprint.RowPitch + column] =
                    expected(sequence, plane, row, column);
        staging->Unmap(0, nullptr);
    }
    void copy(ID3D12GraphicsCommandList *commands, bool upload)
    {
        const auto state = upload ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier(commands, texture.Get(), D3D12_RESOURCE_STATE_COMMON, state);
        D3D12_TEXTURE_COPY_LOCATION buffer{}, surface{};
        buffer.pResource = staging.Get();
        buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        buffer.PlacedFootprint = footprint;
        surface.pResource = texture.Get();
        surface.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(upload ? &surface : &buffer, 0, 0, 0, upload ? &buffer : &surface, nullptr);
        barrier(commands, texture.Get(), state, D3D12_RESOURCE_STATE_COMMON);
    }
    bool verify(std::uint64_t sequence, unsigned plane)
    {
        void *mapped = nullptr;
        D3D12_RANGE read{0, static_cast<SIZE_T>(total_bytes)};
        check(staging->Map(0, &read, &mapped), "test readback map");
        const auto *bytes = static_cast<const unsigned char *>(mapped);
        bool valid = true;
        for (unsigned row = 0; row < rows && valid; ++row)
            for (UINT64 column = 0; column < row_bytes; ++column)
                if (bytes[footprint.Offset + static_cast<UINT64>(row) * footprint.Footprint.RowPitch + column] !=
                    expected(sequence, plane, row, column)) { valid = false; break; }
        D3D12_RANGE no_write{0, 0};
        staging->Unmap(0, &no_write);
        return valid;
    }
};

frame_packet test_packet(std::uint64_t generation, std::uint64_t sequence)
{
    frame_packet packet{};
    packet.generation = generation;
    packet.sequence = sequence;
    packet.capture_ns = 1000000000 + sequence * 16000000;
    packet.color_frame = packet.depth_frame = packet.motion_frame = packet.camera_frame = static_cast<unsigned>(sequence + 100);
    packet.neural_output = packet.camera_valid = 1;
    packet.viewport = 7;
    packet.camera.near_plane = 0.1f;
    packet.camera.far_plane = 10000;
    packet.camera.fov = 1.1f;
    packet.camera.aspect = 16.0f / 9.0f;
    packet.camera.motion_scale = {0.001f, -0.002f};
    packet.camera.jitter = {0.25f, -0.125f};
    packet.camera.camera_motion_included = 1;
    packet.camera.up[1] = packet.camera.right[0] = packet.camera.forward[2] = 1;
    for (unsigned component = 0; component < 16; ++component) {
        const float diagonal = component % 5 == 0 ? 1.0f : 0.0f;
        packet.camera.view_to_clip[component] = packet.camera.clip_to_view[component] = diagonal;
        packet.camera.clip_to_lens[component] = packet.camera.clip_to_previous[component] =
            packet.camera.previous_to_clip[component] = diagonal;
    }
    return packet;
}

int child_main(HANDLE mapping, bool warp)
{
    mapped_view view;
    view.value = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(control_block));
    require(view.value != nullptr, "child control mapping");
    auto &control = *static_cast<control_block *>(view.value);
    try {
        auto device = make_device(warp, &control.manifest);
        shared_frame_transport reader;
        check(reader.open(device.Get(), control.manifest), "child OpenSharedHandle all planes and fences");
        graphics_queue graphics(device.Get());
        std::array<test_plane, shared_planes> local{};
        for (unsigned plane = 0; plane < shared_planes; ++plane)
            local[plane].initialize(device.Get(), control.manifest.descriptions[plane], false);
        consumer_schedule schedule(control.manifest.generation);
        for (std::uint64_t sequence = 1; sequence <= control.total_frames; ++sequence) {
            while (load(control.published) < static_cast<LONG64>(sequence)) {
                HANDLE waits[] = {native_handle(control.ready_event), native_handle(control.parent_process)};
                require(WaitForMultipleObjects(2, waits, FALSE, 5000) == WAIT_OBJECT_0,
                    "parent unavailable or publication timeout");
            }
            const frame_packet packet = control.packets[slot_for(sequence)];
            require(schedule.acquire(packet), "child same-frame/epoch/sequence rejection");
            sl::Constants constants{};
            require(unpack_constants(packet, control.manifest.generation, constants), "SDK constants conversion");
            require(constants.mvecScale.x == 0.001f && constants.mvecScale.y == -0.002f && packet.viewport == 7,
                "metadata did not survive process boundary");
            if (control.exit_without_release) {
                wait_fence(reader.ready_fence(), sequence);
                return 42;
            }
            check(reader.wait_ready(graphics.queue.Get(), sequence), "consumer GPU wait on ready");
            const unsigned slot = slot_for(sequence);
            auto *commands = graphics.begin(slot);
            std::array<texture_binding, shared_planes> destinations{};
            for (unsigned plane = 0; plane < shared_planes; ++plane)
                destinations[plane] = {local[plane].texture.Get(), D3D12_RESOURCE_STATE_COMMON};
            check(reader.record_read(commands, slot, destinations), "shared to private copy");
            for (auto &plane : local) plane.copy(commands, false);
            graphics.submit(slot);
            check(reader.signal_released(graphics.queue.Get(), sequence), "consumer GPU release signal");
            wait_fence(reader.released_fence(), sequence);
            for (unsigned plane = 0; plane < shared_planes; ++plane)
                require(local[plane].verify(sequence, plane), "pixel bytes mismatch across processes");
            require(schedule.release(sequence), "ordered release");
            InterlockedExchange64(&control.checked, static_cast<LONG64>(sequence));
            SetEvent(native_handle(control.done_event));
        }
        return 0;
    } catch (const std::exception &error) {
        strncpy_s(control.error, error.what(), _TRUNCATE);
        InterlockedExchange(&control.failed, 1);
        SetEvent(native_handle(control.done_event));
        return 1;
    }
}

void launch_child(child_process &child, HANDLE mapping, bool warp, const std::vector<HANDLE> &inherited)
{
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    require(InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != FALSE, "child attribute list");
    struct attribute_cleanup
    {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~attribute_cleanup() { DeleteProcThreadAttributeList(value); }
    } cleanup{attributes};
    require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        const_cast<HANDLE *>(inherited.data()), inherited.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE,
        "explicit inherited handle allowlist");
    wchar_t executable[32768]{};
    require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "child executable");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child " +
        std::to_wstring(integer_handle(mapping)) + (warp ? L" warp" : L" 5080");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    require(CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | BELOW_NORMAL_PRIORITY_CLASS,
        nullptr, nullptr, &startup.StartupInfo, &child.info) != FALSE, "isolated child process");
}

void run_case(bool warp, bool large, bool exit_without_release)
{
    auto device = make_device(warp);
    std::array<plane_description, shared_planes> descriptions{};
    descriptions[0] = {large ? 3840u : 64u, large ? 2160u : 36u, DXGI_FORMAT_R16G16B16A16_FLOAT};
    descriptions[1] = {large ? 2562u : 48u, large ? 1441u : 27u, DXGI_FORMAT_R32_FLOAT};
    descriptions[2] = {large ? 2562u : 50u, large ? 1441u : 29u, DXGI_FORMAT_R16G16_FLOAT};
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const auto generation = static_cast<std::uint64_t>(counter.QuadPart);
    shared_frame_transport writer;
    check(writer.create(device.Get(), descriptions, generation), "shared DEFAULT textures and bidirectional fences");
    auto bad_manifest = writer.manifest();
    bad_manifest.adapter_low ^= 0x40000000u;
    shared_frame_transport wrong_adapter;
    require(wrong_adapter.open(device.Get(), bad_manifest) == E_INVALIDARG, "wrong LUID must reject before import");
    bad_manifest = writer.manifest();
    bad_manifest.version++;
    shared_frame_transport wrong_version;
    require(wrong_version.open(device.Get(), bad_manifest) == E_INVALIDARG, "wrong protocol must reject");
    bad_manifest = writer.manifest();
    bad_manifest.descriptions[0].width--;
    shared_frame_transport wrong_extent;
    require(wrong_extent.open(device.Get(), bad_manifest) == E_INVALIDARG, "actual resource extent mismatch must reject");
    graphics_queue graphics(device.Get());
    std::array<std::array<test_plane, shared_planes>, shared_slots> sources{};
    for (auto &slot : sources)
        for (unsigned plane = 0; plane < shared_planes; ++plane)
            slot[plane].initialize(device.Get(), descriptions[plane], true);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    handle_owner mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE,
        0, sizeof(control_block), nullptr));
    handle_owner ready_event(CreateEventW(&attributes, FALSE, FALSE, nullptr));
    handle_owner done_event(CreateEventW(&attributes, FALSE, FALSE, nullptr));
    handle_owner parent;
    require(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent.value,
        SYNCHRONIZE, TRUE, 0) != FALSE, "parent lifetime handle");
    require(mapping.value && ready_event.value && done_event.value, "anonymous shared control handles");
    mapped_view view;
    view.value = MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(control_block));
    require(view.value != nullptr, "parent control mapping");
    auto *control = new (view.value) control_block{};
    control->manifest = writer.manifest();
    control->ready_event = integer_handle(ready_event.value);
    control->done_event = integer_handle(done_event.value);
    control->parent_process = integer_handle(parent.value);
    control->total_frames = exit_without_release ? 3u : large ? 5u : 12u;
    control->exit_without_release = exit_without_release ? 1u : 0u;
    auto inherited = writer.inheritable_handles();
    inherited.insert(inherited.end(), {mapping.value, ready_event.value, done_event.value, parent.value});
    child_process child;
    launch_child(child, mapping.value, warp, inherited);
    producer_schedule schedule(generation);
    for (std::uint64_t sequence = 1; sequence <= control->total_frames; ++sequence) {
        const auto packet = test_packet(generation, sequence);
        while (!schedule.reserve(packet, writer.released_fence()->GetCompletedValue(), writer.ready_fence()->GetCompletedValue())) {
            require(InterlockedCompareExchange(&control->failed, 0, 0) == 0, control->error);
            HANDLE waits[] = {done_event.value, child.info.hProcess};
            require(WaitForMultipleObjects(2, waits, FALSE, 5000) == WAIT_OBJECT_0, "slot recycling timeout or child exit");
        }
        const auto slot = slot_for(sequence);
        auto *commands = graphics.begin(slot);
        std::array<texture_binding, shared_planes> bindings{};
        for (unsigned plane = 0; plane < shared_planes; ++plane) {
            sources[slot][plane].fill(sequence, plane);
            sources[slot][plane].copy(commands, true);
            bindings[plane] = {sources[slot][plane].texture.Get(), D3D12_RESOURCE_STATE_COMMON};
        }
        check(writer.record_write(commands, slot, bindings), "record NR tuple export");
        graphics.submit(slot);
        check(writer.signal_ready(graphics.queue.Get(), sequence), "GPU signal after export submission");
        require(schedule.publish(), "publish only submitted tuple");
        control->packets[slot] = packet;
        InterlockedExchange64(&control->published, static_cast<LONG64>(sequence));
        SetEvent(ready_event.value);
    }
    require(WaitForSingleObject(child.info.hProcess, 15000) == WAIT_OBJECT_0, "child did not finish");
    DWORD exit_code = 0;
    require(GetExitCodeProcess(child.info.hProcess, &exit_code) != FALSE, "child exit status");
    wait_fence(writer.ready_fence(), control->total_frames);
    if (exit_without_release) {
        require(exit_code == 42, "expected deliberate consumer exit");
        require(writer.released_fence()->GetCompletedValue() == 0, "dead child must not fabricate release");
        const auto next = test_packet(generation, 4);
        require(!schedule.reserve(next, 0, 3), "held slot must not be overwritten after consumer death");
        schedule.disconnect();
        require(!schedule.reserve(next, 3, 3), "disconnected transport remains closed");
        std::printf("PASS consumer-exit: no reverse signal, no overwrite, no queued wait on dead consumer\n");
    } else {
        require(exit_code == 0, control->error[0] ? control->error : "child failed");
        require(load(control->checked) == control->total_frames, "not every frame was verified");
        require(schedule.idle(writer.released_fence()->GetCompletedValue(), writer.ready_fence()->GetCompletedValue()),
            "transport not fully drained");
        std::printf("PASS transport: child_pid=%lu frames=%u slots=%u color=%ux%u depth=%ux%u motion=%ux%u all_bytes_equal=1\n",
            child.info.dwProcessId, control->total_frames, shared_slots, descriptions[0].width, descriptions[0].height,
            descriptions[1].width, descriptions[1].height, descriptions[2].width, descriptions[2].height);
    }
}
}

int wmain(int count, wchar_t **arguments)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        if (count == 4 && std::wstring(arguments[1]) == L"--child") {
            wchar_t *end = nullptr;
            const auto value = std::wcstoull(arguments[2], &end, 10);
            require(value && end && *end == 0, "invalid inherited mapping handle");
            return child_main(native_handle(value), std::wstring(arguments[3]) == L"warp");
        }
        if (count != 2 || (std::wstring(arguments[1]) != L"--warp" && std::wstring(arguments[1]) != L"--5080" &&
            std::wstring(arguments[1]) != L"--5080-4k")) {
            std::printf("Usage: shared_transport_test --warp|--5080|--5080-4k\n");
            return 2;
        }
        const bool warp = std::wstring(arguments[1]) == L"--warp";
        const bool large = std::wstring(arguments[1]) == L"--5080-4k";
        run_case(warp, large, false);
        run_case(warp, false, true);
        std::printf("RESULT PASS; synthetic transport only, no Streamline inference or Present\n");
        return 0;
    } catch (const std::exception &error) {
        std::printf("RESULT FAIL %s\n", error.what());
        return 1;
    }
}
