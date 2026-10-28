#include "official_bridge.hpp"
#include "probe_policy.hpp"
#include <dxgi1_6.h>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace mgpu::diag
{
void info(const char *line) { std::printf("%s\n", line); }
void warn(const char *line) { std::printf("%s\n", line); }
void error(const char *line) { std::printf("%s\n", line); }
}

namespace
{
using Microsoft::WRL::ComPtr;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void check(HRESULT result, const char *message)
{
    if (FAILED(result)) {
        std::printf("hr=0x%08lx %s\n", static_cast<unsigned long>(result), message);
        throw std::runtime_error(message);
    }
}
struct window_owner
{
    HWND window = nullptr;
    ~window_owner() { if (window) DestroyWindow(window); }
};
void pump()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
void wait(ID3D12Fence *fence, UINT64 value)
{
    const auto deadline = GetTickCount64() + 3000;
    while (fence->GetCompletedValue() < value && GetTickCount64() < deadline) { pump(); Sleep(1); }
    require(fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= value, "test producer GPU timeout");
}
}

int wmain(int count, wchar_t **arguments)
{
    std::setvbuf(stdout, nullptr, _IOFBF, 65536);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if ((count != 2 && count != 4) || (std::wstring(arguments[1]) != L"--5080-hidden" &&
        std::wstring(arguments[1]) != L"--5080-missing-simulation" &&
        std::wstring(arguments[1]) != L"--5080-missing-render-end" &&
        std::wstring(arguments[1]) != L"--5080-delayed-render-end")) return 2;
    unsigned requested_multiplier = 6;
    if (count == 4) {
        if (std::wstring(arguments[2]) != L"--multiplier") return 2;
        const std::wstring value(arguments[3]);
        if (value != L"2" && value != L"3" && value != L"4" && value != L"6") return 2;
        requested_multiplier = static_cast<unsigned>(value[0] - L'0');
    }
    const bool missing_simulation = std::wstring(arguments[1]) == L"--5080-missing-simulation";
    const bool missing_render_end = std::wstring(arguments[1]) == L"--5080-missing-render-end";
    const bool delayed_render_end = std::wstring(arguments[1]) == L"--5080-delayed-render-end";
    try {
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
        ComPtr<IDXGIAdapter1> selected;
        unsigned matched = 0;
        for (unsigned index = 0; ; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            auto result = factory->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            check(result, "adapter");
            DXGI_ADAPTER_DESC1 description{};
            check(candidate->GetDesc1(&description), "adapter description");
            if (mgpu::official_fg::matches_adapter(description.VendorId,
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0, description.Description)) {
                selected = candidate;
                ++matched;
            }
        }
        require(matched == 1, "exact RTX5080 required");
        ComPtr<ID3D12Device> device;
        check(D3D12CreateDevice(selected.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "5080 device");
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = L"MGPU_Connection_Test_Hidden_Parent";
        require(RegisterClassW(&window_class) != 0, "test class");
        window_owner parent;
        parent.window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, window_class.lpszClassName,
            L"", WS_POPUP, 0, 0, 1280, 720, nullptr, nullptr, window_class.hInstance, nullptr);
        require(parent.window && !IsWindowVisible(parent.window), "parent must remain hidden");
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue)), "producer queue");
        ComPtr<ID3D12CommandAllocator> allocator;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        ComPtr<ID3D12GraphicsCommandList> commands;
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&commands)), "commands");
        check(commands->Close(), "initial close");
        ComPtr<ID3D12Fence> finished;
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&finished)), "producer fence");
        ComPtr<ID3D12DescriptorHeap> descriptors;
        D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
        heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_description.NumDescriptors = 3;
        check(device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&descriptors)), "RTVs");
        const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        std::array<ComPtr<ID3D12Resource>, 3> textures;
        std::array<mgpu::official_fg::texture_binding, 3> bindings{};
        const std::array<mgpu::official_fg::plane_description, 3> sizes{{
            {1280, 720, DXGI_FORMAT_R10G10B10A2_UNORM}, {640, 360, DXGI_FORMAT_R32_FLOAT}, {640, 360, DXGI_FORMAT_R16G16_FLOAT}
        }};
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        for (unsigned plane = 0; plane < 3; ++plane) {
            auto description = mgpu::official_fg::shared_frame_transport::texture_description(sizes[plane]);
            description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&textures[plane])), "synthetic input");
            auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += plane * increment;
            device->CreateRenderTargetView(textures[plane].Get(), nullptr, handle);
            bindings[plane] = {textures[plane].Get(), D3D12_RESOURCE_STATE_COMMON};
        }
        mgpu::fgmap::camera_snapshot camera{};
        camera.valid = camera.camera_motion_included = 1;
        camera.camera_near = 0.1f;
        camera.camera_far = 1000;
        camera.camera_fov = 1.1f;
        camera.camera_aspect = 16.0f / 9.0f;
        camera.mvec_scale[0] = camera.mvec_scale[1] = 1;
        camera.camera_right[0] = camera.camera_up[1] = camera.camera_fwd[2] = 1;
        camera.mvec_subrect_width = camera.depth_subrect_width = 640;
        camera.mvec_subrect_height = camera.depth_subrect_height = 360;
        for (unsigned component = 0; component < 16; ++component) {
            const float value = component % 5 == 0 ? 1.0f : 0.0f;
            camera.camera_view_to_clip[component] = camera.clip_to_camera_view[component] = value;
            camera.clip_to_lens_clip[component] = camera.clip_to_prev_clip[component] = camera.prev_clip_to_clip[component] = value;
        }
        mgpu::official_bridge::request(true, requested_multiplier);
        check(allocator->Reset(), "startup allocator");
        check(commands->Reset(allocator.Get(), nullptr), "startup commands");
        camera.frame_id = 100;
        camera.capture_ns = 1;
        require(!mgpu::official_bridge::record(commands.Get(), parent.window, bindings, 100, 0, camera, 40, true),
            "startup must not publish before source relay");
        check(commands->Close(), "startup close");
        const auto ready_deadline = GetTickCount64() + 15000;
        while (!mgpu::official_bridge::inspect().ready && GetTickCount64() < ready_deadline) {
            require(!mgpu::official_bridge::inspect().failed, "helper startup failed");
            pump(); Sleep(10);
        }
        require(mgpu::official_bridge::inspect().ready, "helper readiness timeout");
        for (unsigned frame = 101; frame < (missing_simulation ? 102u : 113u); ++frame) {
            const auto available_deadline = GetTickCount64() + 3000;
            while (mgpu::official_bridge::backpressured() && GetTickCount64() < available_deadline) { pump(); Sleep(1); }
            require(!mgpu::official_bridge::backpressured(), "shared input backpressure timeout");
            mgpu::official_bridge::source_sleep(frame);
            mgpu::official_bridge::source_marker(frame, 0);
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
            camera.frame_id = frame;
            camera.capture_ns = static_cast<std::uint64_t>(double(now.QuadPart) * 1e9 / double(frequency.QuadPart));
            camera.reset = frame == 101;
            if (!missing_simulation) mgpu::official_bridge::source_marker(frame, 1);
            mgpu::official_bridge::source_marker(frame, 2);
            check(allocator->Reset(), "allocator reset");
            check(commands->Reset(allocator.Get(), nullptr), "frame commands");
            for (unsigned plane = 0; plane < 3; ++plane) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = textures[plane].Get();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                commands->ResourceBarrier(1, &barrier);
                auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += plane * increment;
                const float color[4] = {plane == 0 ? 0.25f : plane == 1 ? 0.5f : 0.0f, plane == 0 ? 0.4f : 0.0f, 0.0f, 1.0f};
                commands->ClearRenderTargetView(handle, color, 0, nullptr);
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                commands->ResourceBarrier(1, &barrier);
            }
            require(mgpu::official_bridge::record(commands.Get(), parent.window, bindings, frame, 0, camera, 40, frame == 101),
                "synthetic post-NR tuple export failed");
            check(commands->Close(), "frame close");
            ID3D12CommandList *lists[] = {commands.Get()};
            queue->ExecuteCommandLists(1, lists);
            require(mgpu::official_bridge::submitted(queue.Get()), "export publication");
            if (delayed_render_end && frame == 107) {
                const auto delayed_until = GetTickCount64() + 180;
                while (GetTickCount64() < delayed_until) { pump(); Sleep(1); }
                const auto before_end = mgpu::official_bridge::inspect();
                require(before_end.real_presents >= 7 && !before_end.failed,
                    "helper still waits for source render end despite submitted GPU-ready input");
            }
            if (!missing_render_end) mgpu::official_bridge::source_marker(frame, 3);
            check(queue->Signal(finished.Get(), frame), "producer finish");
            wait(finished.Get(), frame);
            require(!IsWindowVisible(parent.window), "test parent unexpectedly visible");
            require(!mgpu::official_bridge::inspect().failed, "official helper failed");
            pump(); Sleep(30);
        }
        if (missing_simulation) {
            const auto deadline = GetTickCount64() + 3000;
            while (!mgpu::official_bridge::inspect().failed && GetTickCount64() < deadline) { pump(); Sleep(10); }
            require(mgpu::official_bridge::inspect().failed, "missing simulation marker was incorrectly accepted");
            require(mgpu::official_bridge::inspect().real_presents == 0, "incomplete source was presented");
            require(mgpu::official_bridge::shutdown(), "failed host did not drain");
            std::printf("RESULT PASS negative control: missing source SimulationEnd stopped host, no fabricated Present\n");
            return 0;
        }
        const auto deadline = GetTickCount64() + 5000;
        while (mgpu::official_bridge::inspect().real_presents < 12 && GetTickCount64() < deadline) {
            require(!mgpu::official_bridge::inspect().failed, "official presenter stopped");
            pump(); Sleep(10);
        }
        const auto result = mgpu::official_bridge::inspect();
        std::printf("CONNECTED exported=%llu official_present_calls=%llu sdk_reported=%llu maximum=%u hidden_parent=1 requested=%u\n",
            static_cast<unsigned long long>(result.exported), static_cast<unsigned long long>(result.real_presents),
            static_cast<unsigned long long>(result.sdk_presents), result.maximum_multiplier, requested_multiplier);
        require(result.exported == 12 && result.real_presents == 12 && !result.failed, "incomplete official frame path");
        require(mgpu::official_bridge::shutdown(), "official helper did not drain");
        std::printf("RESULT PASS; synthetic post-NR frames through production bridge and official proxy Present; not game or visible quality validation\n");
        return 0;
    } catch (const std::exception &error) {
        std::printf("RESULT FAIL %s\n", error.what());
        mgpu::official_bridge::shutdown();
        return 1;
    }
}
