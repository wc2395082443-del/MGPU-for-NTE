#include "../src/aligned_input.cpp"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace mgpu::diag
{
void info(const char *) {}
void warn(const char *) {}
}

namespace
{
void require(HRESULT status)
{
    if (FAILED(status)) throw status;
}
void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
void wait(ID3D12Fence *fence, UINT64 value)
{
    const auto deadline = GetTickCount64() + 5000;
    while (fence->GetCompletedValue() < value && GetTickCount64() < deadline) Sleep(1);
    check(fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= value, "fence timeout/removal");
}
ComPtr<ID3D12Resource> make_buffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = description.DepthOrArraySize = description.MipLevels = description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto initial = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
        type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12Resource> resource;
    require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        initial, nullptr, IID_PPV_ARGS(&resource)));
    return resource;
}
void transition(ID3D12GraphicsCommandList *command, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, 0, before, after};
    command->ResourceBarrier(1, &barrier);
}
struct commands
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    void initialize(ID3D12Device *device)
    {
        require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
            nullptr, IID_PPV_ARGS(&list)));
    }
};
struct gate_release
{
    ID3D12Fence *gate;
    ~gate_release() { gate->Signal(100); }
};
}

int main()
{
    using namespace mgpu::aligned;
#ifdef MGPU_TEST_READY_DISPATCH
    constexpr bool ready_mode = true;
#else
    constexpr bool ready_mode = false;
#endif
    try
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            std::puts("D3D12 debug validation enabled for the software test only");
        }
        else std::puts("D3D12 debug layer unavailable; pixel/fence validation still runs");
        ComPtr<IDXGIFactory4> factory;
        require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> adapter;
        require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        ComPtr<ID3D12Device> device;
        require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> diagnostics;
        (void)device.As(&diagnostics);
        data().active.store(true);
        set_game_device(device.Get());
        set_ready_dispatch(ready_mode);
        D3D12_COMMAND_QUEUE_DESC queue_description{};
        ComPtr<ID3D12CommandQueue> renderer, consumer;
        require(device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&renderer)));
        require(device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&consumer)));
        ComPtr<ID3D12Fence> gate, arrival, consumed, published, copy_gate, next_render_done;
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&arrival)));
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumed)));
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&published)));
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_gate)));
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&next_render_done)));
        if (ready_mode) require(data().outgoing.initialize(device.Get()));
        mgpu::producer_copy::transfer_queue read_queue;
        require(read_queue.initialize(device.Get()));
        constexpr UINT64 plane_bytes = 1024, tuple_bytes = 4096;
        const std::array<UINT64, 3> offsets{1024, 2048, 3072};
        const std::array<unsigned, 3> types{53, 0, 1};
        const std::array<DXGI_FORMAT, 3> formats{
            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT};
        std::array<ComPtr<ID3D12Resource>, 3> textures;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 3> footprints{};
        for (unsigned component = 0; component != 3; ++component)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            description.Width = description.Height = 4;
            description.DepthOrArraySize = description.MipLevels = description.SampleDesc.Count = 1;
            description.Format = formats[component];
            require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&textures[component])));
            device->GetCopyableFootprints(&description, 0, 1, 0, &footprints[component], nullptr, nullptr, nullptr);
        }
        std::array<ComPtr<ID3D12Resource>, 4> uploads, targets, local, readbacks;
        for (unsigned slot = 0; slot != 4; ++slot)
        {
            uploads[slot] = make_buffer(device.Get(), tuple_bytes, D3D12_HEAP_TYPE_UPLOAD);
            targets[slot] = make_buffer(device.Get(), tuple_bytes, D3D12_HEAP_TYPE_DEFAULT);
            local[slot] = make_buffer(device.Get(), tuple_bytes, D3D12_HEAP_TYPE_DEFAULT);
            readbacks[slot] = make_buffer(device.Get(), tuple_bytes, D3D12_HEAP_TYPE_READBACK);
        }
        gate_release unblock{gate.Get()};
        gate_release unblock_copy{copy_gate.Get()};
        for (unsigned batch = 0; batch != 3; ++batch)
        {
            std::array<commands, 4> game_commands, read_commands;
            require(renderer->Wait(gate.Get(), batch + 1));
            if (ready_mode) require(data().outgoing.queue()->Wait(copy_gate.Get(), batch + 1));
            for (unsigned slot = 0; slot != 4; ++slot)
            {
                const UINT64 seal = batch * 4 + slot + 1;
                const std::uint32_t engine_frame = UINT32_MAX - 3u + unsigned(seal * 2);
                const float expected = float(seal) / 16.0f;
                void *mapped = nullptr;
                const D3D12_RANGE empty{};
                require(uploads[slot]->Map(0, &empty, &mapped));
                std::memset(mapped, 0xa7, tuple_bytes);
                std::memcpy(static_cast<char *>(mapped) + 3072, &seal, sizeof seal);
                for (unsigned component = 0; component != 3; ++component)
                    for (unsigned row = 0; row != 4; ++row)
                        for (unsigned column = 0; column != 4; ++column)
                            std::memcpy(static_cast<char *>(mapped) + component * plane_bytes + row * 256 + column * 4,
                                &expected, sizeof expected);
                uploads[slot]->Unmap(0, nullptr);
                game_commands[slot].initialize(device.Get());
                auto *command = game_commands[slot].list.Get();
                for (unsigned component = 0; component != 3; ++component)
                {
                    auto *texture = textures[component].Get();
                    transition(command, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                    D3D12_TEXTURE_COPY_LOCATION source{}, target{};
                    source.pResource = uploads[slot].Get();
                    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    source.PlacedFootprint = footprints[component];
                    source.PlacedFootprint.Offset = component * plane_bytes;
                    target.pResource = texture;
                    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    command->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
                    transition(command, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    tagged_resource tag{reinterpret_cast<UINT64>(texture), D3D12_RESOURCE_STATE_COPY_SOURCE, 0, 0, 0, 4, 4};
                    if (component == 1) { tag.left = tag.top = 1; tag.width = tag.height = 2; }
                    capture(command, types[component], tag, {engine_frame, 1, true, true});
                    transition(command, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                    source.PlacedFootprint.Offset = 3 * plane_bytes;
                    command->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
                    transition(command, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                }
                guide_constants constants{};
                constants.key = {engine_frame, 1};
                constants.valid = true;
                capture_constants(constants);
                require(command->Close());
                ID3D12CommandList *lists[]{command};
                renderer->ExecuteCommandLists(1, lists);
                std::array<std::uint64_t, 64> serials{};
                UINT bytes = sizeof serials[0];
                require(command->GetPrivateData(command_guid, &bytes, &serials[0]));
                after_submit(renderer.Get(), serials, 1, mgpu::timing::cpu_ticks());
            }
            if (ready_mode)
            {
                lease pending;
                check(!acquire(pending) && !pending, "source gate must defer acquisition without blocking CPU");
                HANDLE event = dispatch_event();
                check(event != nullptr, "pending source must provide a bridge wake event");
                const auto registrations = data().wake_registrations;
                for (unsigned poll = 0; poll != 32; ++poll)
                    check(!acquire(pending) && dispatch_event() == event, "polling cannot grant an unready frame");
                check(data().wake_registrations == registrations, "one event registration per source ticket");
                SetEvent(event);
                check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0 && !acquire(pending),
                    "stale wake must never bypass the source fence");
                ++data().wake_serial;
                check(dispatch_event() == nullptr, "wake token rejects a stale slot generation");
                check(!acquire(pending) && dispatch_event() == event, "reacquire repairs a stale wake token");
                data().wake_failed = true;
                check(!acquire(pending) && dispatch_event() == nullptr,
                    "wake failure leaves safe nonblocking polling, never prequeues an unready source");
                data().wake_failed = false;
                set_ready_dispatch(false);
                check(dispatch_event() == nullptr, "disabled route exposes no wake event");
                set_ready_dispatch(true);
                check(!acquire(pending), "reenabling cannot lease an unready source");
                require(gate->Signal(batch + 1));
                check(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "source fence wakes worker without a polling sleep");
                for (auto &frame : data().frames)
                {
                    const auto ticket = frame.state.copied[0];
                    wait(data().queues[ticket.queue].fence, ticket.value);
                }
            }
            for (unsigned slot = 0; slot != 4; ++slot)
            {
                const UINT64 seal = batch * 4 + slot + 1;
                const std::uint32_t engine_frame = UINT32_MAX - 3u + unsigned(seal * 2);
                lease frame;
                check(acquire(frame), ready_mode ? "source-ready dispatch must acquire the oldest ready frame"
                    : "legacy acquire must return before captured pixels complete");
                check(frame.key == frame_key{engine_frame, 1}, "frame and constants identity must survive wrap and gaps");
                if (!ready_mode)
                    check(!data().depth_converter.complete(data().frames[frame.slot].normalization), "depth is still behind render gate");
                check(matches(frame.planes[1], 2, 2, DXGI_FORMAT_R32_FLOAT, frame.planes[1].footprint,
                    frame.planes[1].bytes), "bounded depth region has consistent consumer dimensions");
                require(submit_transfer(frame, uploads[slot].Get(), 3072, targets[slot].Get(), 0, 8,
                    offsets, arrival.Get(), seal));
                commit_transfer(frame);
                std::array<mgpu::producer_copy::range, 4> ranges{{{targets[slot].Get(), 0, 0, tuple_bytes}}};
                std::array<mgpu::producer_copy::dependency, 3> dependency{{{arrival.Get(), seal}}};
                UINT64 retirement = 0;
                require(read_queue.submit(ranges, 1, local[slot].Get(), consumed.Get(), seal, retirement, dependency));
                read_commands[slot].initialize(device.Get());
                read_commands[slot].list->CopyBufferRegion(readbacks[slot].Get(), 0, local[slot].Get(), 0, tuple_bytes);
                require(read_commands[slot].list->Close());
                require(consumer->Wait(consumed.Get(), seal));
                ID3D12CommandList *read_lists[]{read_commands[slot].list.Get()};
                consumer->ExecuteCommandLists(1, read_lists);
                require(consumer->Signal(published.Get(), seal));
                check(published->GetCompletedValue() <= batch * 4 && arrival->GetCompletedValue() <= batch * 4,
                    "GPU dependencies must hold output without blocking CPU submission");
            }
            const auto done = [&](unsigned index) { return completed(data(), index); };
            for (auto &frame : data().frames)
                check(!frame.state.reclaimable(done), "owned capture cannot recycle while downstream read is pending");
            if (ready_mode)
            {
                commands next_render;
                next_render.initialize(device.Get());
                for (unsigned component = 0; component != 3; ++component)
                {
                    auto *texture = textures[component].Get();
                    transition(next_render.list.Get(), texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                    D3D12_TEXTURE_COPY_LOCATION source{}, target{};
                    source.pResource = uploads[0].Get();
                    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    source.PlacedFootprint = footprints[component];
                    source.PlacedFootprint.Offset = 3072;
                    target.pResource = texture;
                    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    next_render.list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
                    transition(next_render.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                }
                require(next_render.list->Close());
                ID3D12CommandList *next_lists[]{next_render.list.Get()};
                renderer->ExecuteCommandLists(1, next_lists);
                require(renderer->Signal(next_render_done.Get(), batch + 1));
                wait(next_render_done.Get(), batch + 1);
                check(arrival->GetCompletedValue() <= batch * 4,
                    "next source DIRECT must finish while outgoing COPY remains gated");
                require(copy_gate->Signal(batch + 1));
            }
            else require(gate->Signal(batch + 1));
            wait(published.Get(), (batch + 1) * 4);
            for (unsigned slot = 0; slot != 4; ++slot)
            {
                const float expected = float(batch * 4 + slot + 1) / 16.0f;
                void *mapped = nullptr;
                D3D12_RANGE range{0, tuple_bytes};
                require(readbacks[slot]->Map(0, &range, &mapped));
                UINT64 actual_seal = 0;
                std::memcpy(&actual_seal, mapped, sizeof actual_seal);
                check(actual_seal == batch * 4 + slot + 1, "seal must match the exact payload generation");
                for (unsigned component = 0; component != 3; ++component)
                {
                    const unsigned size = component == 1 ? 2 : 4;
                    for (unsigned row = 0; row != size; ++row)
                        for (unsigned column = 0; column != size; ++column)
                            check(std::memcmp(static_cast<char *>(mapped) + offsets[component] + row * 256 + column * 4,
                                &expected, sizeof expected) == 0, "stale/mixed frame or post-tag source overwrite");
                }
                const D3D12_RANGE empty{};
                readbacks[slot]->Unmap(0, &empty);
            }
            for (auto &frame : data().frames)
            {
                check(frame.state.reclaimable(done), "completed outgoing transfer retires owned snapshots");
                collect_timing(data(), frame);
                check(frame.timing_collected, "capture timing collected only after retirement");
            }
        }
        check(data().timing_metrics[0].count == 12 && data().timing_metrics[1].count == 12 &&
            data().timing_metrics[2].count == 12 && data().timing_metrics[5].count == 12,
            "all capture and normalization spans must be measurable");
        if (diagnostics != nullptr)
        {
            unsigned errors = 0;
            for (UINT64 index = 0; index != diagnostics->GetNumStoredMessages(); ++index)
            {
                SIZE_T bytes = 0;
                require(diagnostics->GetMessage(index, nullptr, &bytes));
                std::vector<unsigned char> storage(bytes);
                auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
                require(diagnostics->GetMessage(index, message, &bytes));
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                {
                    std::printf("D3D12 error: %s\n", message->pDescription);
                    ++errors;
                }
            }
            check(errors == 0, "D3D12 state/lifetime validation errors");
        }
        if (ready_mode)
        {
            check(data().wake_registrations == 3, "three batches require exactly three source event registrations");
            auto &probe = data().frames[0];
            const auto saved_state = probe.state;
            const auto saved_normalization_value = probe.normalization.value;
            probe.state.key.frame = data().last_delivered.frame + 2;
            probe.state.release_command = 0;
            probe.state.released = {};
            auto &constants = data().constants[probe.state.key.frame % data().constants.size()];
            constants = {};
            constants.key = probe.state.key;
            constants.valid = true;
            probe.state.copied[2].value = UINT64_MAX - 1;
            probe.normalization.value = UINT64_MAX - 1;
            {
                lease ready_color;
                check(acquire(ready_color), "ready color must not CPU-wait for motion or normalization");
            }
            ComPtr<ID3D12Fence> sentinel;
            require(device->CreateFence(UINT64_MAX, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&sentinel)));
            auto &source_queue = data().queues[probe.state.copied[0].queue];
            auto *saved_fence = source_queue.fence;
            source_queue.fence = sentinel.Get();
            {
                lease removed;
                check(!acquire(removed) && data().failed && dispatch_event() == nullptr,
                    "UINT64_MAX completion is removal/failure, never permission to read");
            }
            source_queue.fence = saved_fence;
            probe.state = saved_state;
            probe.normalization.value = saved_normalization_value;
            stop();
            check(dispatch_event() == nullptr, "stopped capture cannot expose a source event");
        }
        std::printf("PASS: WARP ready_dispatch=%u, 12 exact tuples, four pending frames, source overwrite isolation, cropped depth, frame wrap/gaps, GPU dependencies, retirement and timestamps; not a dual-GPU throughput measurement\n", unsigned(ready_mode));
        return 0;
    }
    catch (HRESULT status) { std::printf("FAIL: 0x%08lx\n", static_cast<unsigned long>(status)); }
    catch (const std::exception &error) { std::printf("FAIL: %s\n", error.what()); }
    return 1;
}
