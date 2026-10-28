// MGPU secondary-FG: the first inference probe.
//
// The capability probe proved feature 11 can be CREATED on the 5080. It never
// called EvaluateFeature, so nothing in this project has ever produced a
// generated frame. This probe drives the same dlss_fg_stage the add-on will
// use, against a synthetic scene on the 5080 alone: no game, no ReShade, no
// cross-adapter copy.
//
// What this can honestly claim, and nothing more:
//   - create / record / track_submission / complete runs against the real NGX
//     runtime in the order the stage requires;
//   - EvaluateFeature returns success on a real submission;
//   - the interpolated output UAV is written with data distinct from the input.
// It claims no picture quality. A synthetic scene is not a game frame.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <vector>

#include "../../ext/ngx/nvsdk_ngx.h"
#include "../../ext/ngx/nvsdk_ngx_defs_dlssg.h"
#include "../../ext/ngx/nvsdk_ngx_params_dlssg.h"
#include "dlss_fg_stage.hpp"
#include "frame_contract.hpp"
#include "parameter_bridge.hpp"

namespace
{
constexpr unsigned kWidth = 1280;
constexpr unsigned kHeight = 720;
constexpr unsigned kPitchAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
constexpr unsigned kMotionBytes = 2 * sizeof(unsigned short);
static_assert(kMotionBytes == 4);

D3D12_RESOURCE_STATES initial_state(D3D12_HEAP_TYPE heap)
{
    if (heap == D3D12_HEAP_TYPE_UPLOAD) return D3D12_RESOURCE_STATE_GENERIC_READ;
    if (heap == D3D12_HEAP_TYPE_READBACK) return D3D12_RESOURCE_STATE_COPY_DEST;
    return D3D12_RESOURCE_STATE_COMMON;
}

bool pack_rows(void *destination, size_t capacity, size_t pitch, const void *source,
               size_t available, size_t row_bytes, size_t rows)
{
    if (destination == nullptr || source == nullptr || row_bytes == 0 || rows == 0 ||
        pitch < row_bytes || rows > SIZE_MAX / pitch || rows > SIZE_MAX / row_bytes ||
        capacity < pitch * rows || available < row_bytes * rows) return false;
    auto *output = static_cast<unsigned char *>(destination);
    const auto *input = static_cast<const unsigned char *>(source);
    std::memset(output, 0, pitch * rows);
    for (size_t row = 0; row < rows; ++row)
        std::memcpy(output + row * pitch, input + row * row_bytes, row_bytes);
    return true;
}

unsigned long long aligned_pitch(unsigned bytes_per_pixel)
{
    const unsigned long long raw = static_cast<unsigned long long>(kWidth) * bytes_per_pixel;
    return ((raw + kPitchAlignment - 1) / kPitchAlignment) * kPitchAlignment;
}

unsigned long long footprint(unsigned bytes_per_pixel) { return aligned_pitch(bytes_per_pixel) * kHeight; }

unsigned short to_half(float value)
{
    unsigned int bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const unsigned int sign = (bits >> 16) & 0x8000u;
    int exponent = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
    const unsigned int mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0) return static_cast<unsigned short>(sign);
    if (exponent >= 31) return static_cast<unsigned short>(sign | 0x7C00u);
    unsigned short half = static_cast<unsigned short>(
        sign | (static_cast<unsigned int>(exponent) << 10) | (mantissa >> 13));
    if (mantissa & 0x1000u) half += 1;
    return half;
}

struct scene_frame
{
    std::vector<unsigned char> color;
    std::vector<float> depth;
    std::vector<unsigned short> mvec;
};

// One flat plane at depth 20 with a 100x100 square at depth 5. Frame 2 is the
// same scene with the square moved 40 pixels right, so the square's pixels
// carry a real per-pixel vector and the plane's carry zero.
scene_frame make_frame(unsigned square_x, float vector_x, float vector_y)
{
    scene_frame frame;
    frame.color.resize(static_cast<size_t>(kWidth) * kHeight * 4);
    frame.depth.resize(static_cast<size_t>(kWidth) * kHeight);
    frame.mvec.resize(static_cast<size_t>(kWidth) * kHeight * 2);
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x)
        {
            const size_t pixel = static_cast<size_t>(y) * kWidth + x;
            const bool square = x >= square_x && x < square_x + 100 && y >= 300 && y < 400;
            const size_t base = pixel * 4;
            frame.color[base + 0] = static_cast<unsigned char>((x * 200) / kWidth);
            frame.color[base + 1] = static_cast<unsigned char>((y * 200) / kHeight);
            frame.color[base + 2] = 128;
            frame.color[base + 3] = 255;
            if (square)
            {
                frame.color[base + 0] = 255;
                frame.color[base + 1] = 32;
                frame.color[base + 2] = 32;
                frame.depth[pixel] = 100.0f / 99.0f - 100.0f / (99.0f * 5.0f);
                frame.mvec[pixel * 2 + 0] = to_half(vector_x);
                frame.mvec[pixel * 2 + 1] = to_half(vector_y);
            }
            else
            {
                frame.depth[pixel] = 100.0f / 99.0f - 100.0f / (99.0f * 20.0f);
                frame.mvec[pixel * 2 + 0] = 0;
                frame.mvec[pixel * 2 + 1] = 0;
            }
        }
    return frame;
}

void perspective(float (&matrix)[4][4], float aspect, float fov, float near_plane, float far_plane)
{
    const float f = 1.0f / std::tan(fov * 0.5f);
    std::memset(matrix, 0, sizeof(matrix));
    matrix[0][0] = f / aspect;
    matrix[1][1] = f;
    matrix[2][2] = far_plane / (far_plane - near_plane);
    matrix[2][3] = 1.0f;
    matrix[3][2] = -near_plane * far_plane / (far_plane - near_plane);
}

bool invert(const float (&matrix)[4][4], float (&out)[4][4])
{
    double work[4][8]{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            work[r][c] = matrix[r][c];
            work[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    for (int col = 0; col < 4; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(work[r][col]) > std::fabs(work[pivot][col])) pivot = r;
        if (std::fabs(work[pivot][col]) < 1e-9) return false;
        if (pivot != col)
            for (int c = 0; c < 8; ++c) std::swap(work[pivot][c], work[col][c]);
        const double lead = work[col][col];
        for (int c = 0; c < 8; ++c) work[col][c] /= lead;
        for (int r = 0; r < 4; ++r)
        {
            if (r == col) continue;
            const double factor = work[r][col];
            if (factor == 0.0) continue;
            for (int c = 0; c < 8; ++c) work[r][c] -= factor * work[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = static_cast<float>(work[r][c + 4]);
    return true;
}

ID3D12Resource *create_resource(ID3D12Device *device, D3D12_HEAP_TYPE heap_type,
                                const D3D12_RESOURCE_DESC &desc, bool allow_uav)
{
    D3D12_RESOURCE_DESC copy = desc;
    copy.Flags = allow_uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type;
    ID3D12Resource *resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy,
            initial_state(heap_type), nullptr, __uuidof(ID3D12Resource),
            reinterpret_cast<void **>(&resource))))
        return nullptr;
    return resource;
}

ID3D12Resource *create_texture(ID3D12Device *device, DXGI_FORMAT format, bool allow_uav)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    return create_resource(device, D3D12_HEAP_TYPE_DEFAULT, desc, allow_uav);
}

ID3D12Resource *create_buffer(ID3D12Device *device, unsigned long long bytes,
                              D3D12_HEAP_TYPE heap_type, bool allow_uav)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return create_resource(device, heap_type, desc, allow_uav);
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

void copy_texture(ID3D12GraphicsCommandList *commands, ID3D12Resource *texture,
                  ID3D12Resource *buffer, unsigned bytes_per_pixel, bool readback)
{
    D3D12_TEXTURE_COPY_LOCATION image{};
    image.pResource = texture;
    image.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION staging{};
    staging.pResource = buffer;
    staging.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    staging.PlacedFootprint.Offset = 0;
    staging.PlacedFootprint.Footprint.Format = texture->GetDesc().Format;
    staging.PlacedFootprint.Footprint.Width = kWidth;
    staging.PlacedFootprint.Footprint.Height = kHeight;
    staging.PlacedFootprint.Footprint.Depth = 1;
    staging.PlacedFootprint.Footprint.RowPitch = static_cast<unsigned>(aligned_pitch(bytes_per_pixel));
    commands->CopyTextureRegion(readback ? &staging : &image, 0, 0, 0,
                                 readback ? &image : &staging, nullptr);
}

bool wait_fence(ID3D12Fence *fence, unsigned long long value, HANDLE event, unsigned timeout_ms)
{
    if (FAILED(fence->SetEventOnCompletion(value, event))) return false;
    if (WaitForSingleObject(event, timeout_ms) != WAIT_OBJECT_0) return false;
    const unsigned long long completed = fence->GetCompletedValue();
    return completed != UINT64_MAX && completed >= value;
}

void NVSDK_CONV log_ngx(const char *message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature feature)
{
    if (message != nullptr)
        std::printf("NGX_LOG level=%u feature=%u %.512s\n", static_cast<unsigned>(level),
                    static_cast<unsigned>(feature), message);
}
}

int wmain(int argc, wchar_t **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    if (argc == 2 && std::wcscmp(argv[1], L"--cpu-self-test") == 0)
    {
        const auto scene = make_frame(240, -40.0f / kWidth, 0.0f);
        unsigned char source[6] = {1, 2, 3, 4, 5, 6};
        unsigned char packed[8]{};
        const bool checks[] = {
            scene.mvec.size() * sizeof(scene.mvec[0]) == footprint(kMotionBytes),
            scene.mvec.size() * sizeof(scene.mvec[0]) < footprint(8),
            pack_rows(packed, sizeof packed, 4, source, sizeof source, 3, 2),
            packed[0] == 1 && packed[3] == 0 && packed[4] == 4 && packed[7] == 0,
            !pack_rows(packed, sizeof packed, 4, source, 5, 3, 2),
            !pack_rows(packed, 7, 4, source, sizeof source, 3, 2),
            !pack_rows(packed, sizeof packed, 2, source, sizeof source, 3, 2),
            !pack_rows(packed, sizeof packed, SIZE_MAX, source, sizeof source, 1, 2),
            initial_state(D3D12_HEAP_TYPE_UPLOAD) == D3D12_RESOURCE_STATE_GENERIC_READ,
            initial_state(D3D12_HEAP_TYPE_READBACK) == D3D12_RESOURCE_STATE_COPY_DEST,
            scene.depth.front() > 0.0f && scene.depth.front() < 1.0f
        };
        for (size_t index = 0; index < sizeof checks / sizeof checks[0]; ++index)
            if (!checks[index]) { std::printf("CPU_SELF_TEST_FAILED index=%zu\n", index); return 60; }
        std::puts("PASS: 11 CPU-only scene, row packing, overflow and heap-state checks; no GPU calls.");
        return 0;
    }

    if (argc != 4 || std::wcscmp(argv[1], L"--allow-sdk-query") != 0)
    {
        std::puts("Usage: fg_inference_probe --allow-sdk-query <absolute official _nvngx.dll> <existing log directory>");
        std::puts("Synthetic scene on the RTX 5080 only. No game process, no other GPU.");
        return 2;
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (!GlobalMemoryStatusEx(&memory) || memory.ullAvailPhys < 8ULL * 1024 * 1024 * 1024 ||
        memory.ullAvailPageFile < 12ULL * 1024 * 1024 * 1024)
    {
        std::puts("REFUSED_MEMORY_PRESSURE: require 8 GiB free physical and 12 GiB free commit; no GPU initialized.");
        return 61;
    }

    const auto process_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (process_job == nullptr ||
        !SetInformationJobObject(process_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(process_job, GetCurrentProcess()))
    {
        std::puts("REFUSED: unable to contain the probe and its runtime children.");
        if (process_job != nullptr) CloseHandle(process_job);
        return 13;
    }

    wchar_t resolved[MAX_PATH]{};
    if (GetFullPathNameW(argv[2], MAX_PATH, resolved, nullptr) == 0 ||
        _wcsicmp(resolved, argv[2]) != 0 || GetFileAttributesW(argv[3]) == INVALID_FILE_ATTRIBUTES)
        return 3;

    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return 4;
    IDXGIAdapter1 *selected = nullptr;
    unsigned matches = 0;
    for (unsigned index = 0;; ++index)
    {
        IDXGIAdapter1 *adapter = nullptr;
        const auto status = factory->EnumAdapters1(index, &adapter);
        if (status == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(status)) { if (selected) selected->Release(); factory->Release(); return 5; }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10de &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            std::wcscmp(desc.Description, L"NVIDIA GeForce RTX 5080") == 0)
        {
            ++matches;
            if (selected == nullptr)
            {
                selected = adapter;
                adapter = nullptr;
                std::printf("SELECTED RTX 5080 LUID=%08X:%08X\n",
                            static_cast<unsigned>(desc.AdapterLuid.HighPart),
                            static_cast<unsigned>(desc.AdapterLuid.LowPart));
            }
        }
        if (adapter) adapter->Release();
    }
    factory->Release();
    if (selected == nullptr || matches != 1)
    {
        if (selected) selected->Release();
        std::puts("REFUSED: expected exactly one RTX 5080. No fallback to another GPU.");
        return 6;
    }

    const auto module = LoadLibraryExW(argv[2], nullptr,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) { selected->Release(); return 7; }
    wchar_t driver_folder[MAX_PATH]{};
    std::wcscpy(driver_folder, resolved);
    auto separator = std::wcsrchr(driver_folder, L'\\');
    if (separator == nullptr) separator = std::wcsrchr(driver_folder, L'/');
    if (separator == nullptr) { selected->Release(); FreeLibrary(module); return 9; }
    *separator = 0;

    const wchar_t *search_paths[] = {driver_folder};
    NVSDK_NGX_FeatureCommonInfo common{};
    common.PathListInfo.Path = search_paths;
    common.PathListInfo.Length = 1;
    common.LoggingInfo.LoggingCallback = &log_ngx;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    common.LoggingInfo.DisableOtherLoggingSinks = true;

    wchar_t runtime_path[MAX_PATH]{};
    std::swprintf(runtime_path, MAX_PATH, L"%ls\\nvngx.dll", driver_folder);
    const auto runtime = LoadLibraryExW(runtime_path, nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (runtime == nullptr)
    {
        std::printf("FAILED official runtime load error=%lu\n", GetLastError());
        selected->Release();
        FreeLibrary(module);
        return 14;
    }

    using init_function = NVSDK_NGX_Result (NVSDK_CONV *)(const char *, NVSDK_NGX_EngineType,
        const char *, const wchar_t *, ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *);
    using parameters_function = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter **);
    using destroy_function = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter *);
    using shutdown_function = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12Device *, unsigned int *);
    using release_function = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle *);
    using create_function = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
        const NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
    using evaluate_function = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *,
        const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback_C);

    mgpu::secondary_fg::ngx_api api{};
    api.allocate = reinterpret_cast<parameters_function>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_AllocateParameters"));
    api.destroy = reinterpret_cast<destroy_function>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_DestroyParameters"));
    api.create = reinterpret_cast<create_function>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_CreateFeature"));
    api.evaluate = reinterpret_cast<evaluate_function>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_EvaluateFeature"));
    api.release = reinterpret_cast<release_function>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_ReleaseFeature"));
    const auto init = reinterpret_cast<init_function>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_Init_ProjectID"));
    const auto capabilities = reinterpret_cast<parameters_function>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    const auto shutdown = reinterpret_cast<shutdown_function>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_Shutdown1"));
    if (init == nullptr || capabilities == nullptr || api.allocate == nullptr || api.destroy == nullptr ||
        api.create == nullptr || api.evaluate == nullptr || api.release == nullptr || shutdown == nullptr)
    {
        std::puts("UNRESOLVED runtime entry point.");
        selected->Release();
        FreeLibrary(runtime);
        FreeLibrary(module);
        return 15;
    }

    ID3D12Device *device = nullptr;
    if (FAILED(D3D12CreateDevice(selected, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
            reinterpret_cast<void **>(&device))))
    {
        std::puts("FAILED D3D12CreateDevice");
        selected->Release();
        FreeLibrary(runtime);
        FreeLibrary(module);
        return 17;
    }
    const auto luid = device->GetAdapterLuid();
    std::printf("DEVICE_CREATED LUID=%08X:%08X\n", static_cast<unsigned>(luid.HighPart), static_cast<unsigned>(luid.LowPart));

    const auto init_status = init("031cb83c-5d5b-4e7e-8091-266178350ffa", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "secondary-fg-prototype-0.1", argv[3], device, NVSDK_NGX_Version_API, &common);
    std::printf("NGX_INIT result=0x%08X\n", static_cast<unsigned>(init_status));
    if (NVSDK_NGX_FAILED(init_status))
    {
        selected->Release();
        FreeLibrary(runtime);
        FreeLibrary(module);
        device->Release();
        return 18;
    }

    NVSDK_NGX_Parameter *caps = nullptr;
    if (NVSDK_NGX_FAILED(capabilities(&caps)) || caps == nullptr)
    {
        std::puts("FAILED GetCapabilityParameters");
        unsigned remaining_sessions = 0;
        shutdown(device, &remaining_sessions);
        selected->Release();
        FreeLibrary(runtime);
        FreeLibrary(module);
        device->Release();
        return 19;
    }

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *commands = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue),
            reinterpret_cast<void **>(&queue)))) { std::puts("FAILED CreateCommandQueue"); return 20; }
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
            reinterpret_cast<void **>(&allocator)))) { std::puts("FAILED CreateCommandAllocator"); return 21; }
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
            __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&commands))))
    { std::puts("FAILED CreateCommandList"); return 22; }
    if (FAILED(commands->Close())) return 22;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
            reinterpret_cast<void **>(&fence)))) { std::puts("FAILED CreateFence"); return 23; }
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) { std::puts("FAILED CreateEvent"); return 24; }

    ID3D12CommandList *lists[] = {commands};

    ID3D12Resource *color = create_texture(device, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    ID3D12Resource *depth = create_texture(device, DXGI_FORMAT_R32_FLOAT, false);
    ID3D12Resource *motion = create_texture(device, DXGI_FORMAT_R16G16_FLOAT, false);
    ID3D12Resource *interpolated = create_texture(device, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    ID3D12Resource *hint = create_buffer(device, 16, D3D12_HEAP_TYPE_DEFAULT, true);
    ID3D12Resource *upload_color = create_buffer(device, footprint(4), D3D12_HEAP_TYPE_UPLOAD, false);
    ID3D12Resource *upload_depth = create_buffer(device, footprint(4), D3D12_HEAP_TYPE_UPLOAD, false);
    ID3D12Resource *upload_motion = create_buffer(device, footprint(kMotionBytes), D3D12_HEAP_TYPE_UPLOAD, false);
    ID3D12Resource *upload_hint = create_buffer(device, 16, D3D12_HEAP_TYPE_UPLOAD, false);
    ID3D12Resource *readback_color = create_buffer(device, footprint(4), D3D12_HEAP_TYPE_READBACK, false);
    ID3D12Resource *readback_hint = create_buffer(device, 16, D3D12_HEAP_TYPE_READBACK, false);
    if (color == nullptr || depth == nullptr || motion == nullptr || interpolated == nullptr ||
        hint == nullptr || upload_color == nullptr || upload_depth == nullptr || upload_motion == nullptr ||
        upload_hint == nullptr || readback_color == nullptr || readback_hint == nullptr)
    {
        std::puts("FAILED resource allocation");
        return 25;
    }

    mgpu::secondary_fg::configuration config{};
    config.width = kWidth;
    config.height = kHeight;
    config.guide_width = kWidth;
    config.guide_height = kHeight;
    config.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    config.adapter_luid = luid;

    auto write_upload = [&](ID3D12Resource *buffer, const void *data, unsigned long long bytes) -> bool
    {
        void *mapped = nullptr;
        std::printf("WU bytes=%llu mapped_ptr=%d\n", bytes, buffer != nullptr);
        if (FAILED(buffer->Map(0, nullptr, &mapped))) return false;
        std::memcpy(mapped, data, static_cast<size_t>(bytes));
        buffer->Unmap(0, nullptr);
        return true;
    };

    // Upload the frame, then leave each input in the state the stage expects to
    // find it in, so the stage's own barriers are the only ones that move.
    auto upload_frame = [&](ID3D12Resource *upload, ID3D12Resource *destination,
                            unsigned bytes_per_pixel, D3D12_RESOURCE_STATES current,
                            D3D12_RESOURCE_STATES settle, unsigned long long fence_value) -> bool
    {
        std::printf("UPLOAD fence=%llu\n", fence_value);
        commands->Reset(allocator, nullptr);
        std::puts("UF a");
        transition(commands, destination, current, D3D12_RESOURCE_STATE_COPY_DEST);
        std::puts("UF b");
        copy_texture(commands, destination, upload, bytes_per_pixel, false);
        std::puts("UF c");
        transition(commands, destination, D3D12_RESOURCE_STATE_COPY_DEST, settle);
        std::puts("UF d");
        commands->Close();
        std::puts("UF e");
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, fence_value);
        return wait_fence(fence, fence_value, event, 20000);
    };

    mgpu::secondary_fg::dlss_fg_stage stage;

    commands->Reset(allocator, nullptr);
    const auto stage_create = stage.create(device, commands, caps, api, config);
    std::printf("STAGE_CREATE result=0x%08X live=%d\n", static_cast<unsigned>(stage_create), stage.live());
    if (!stage.live()) return 27;
    commands->Close();
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, 1);
    if (!wait_fence(fence, 1, event, 20000)) { std::puts("FAILED create fence"); return 28; }
    if (!stage.track_submission(fence, 1)) { std::puts("FAILED track create submission"); return 29; }

    float view_to_clip[4][4]{};
    float clip_to_view[4][4]{};
    float lens_clip[4][4]{};
    float identity[4][4]{};
    const float aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
    const float fov = 1.04719755f;
    perspective(view_to_clip, aspect, fov, 1.0f, 100.0f);
    if (!invert(view_to_clip, clip_to_view)) { std::puts("FAILED camera inverse"); return 30; }
    for (int i = 0; i < 4; ++i) lens_clip[i][i] = 1.0f;
    for (int i = 0; i < 4; ++i) identity[i][i] = 1.0f;

    const scene_frame frame1 = make_frame(200, 0.0f, 0.0f);
    const scene_frame frame2 = make_frame(240, 40.0f / static_cast<float>(kWidth), 0.0f);

    auto make_resources = [&]() -> mgpu::secondary_fg::frame_resources
    {
        mgpu::secondary_fg::frame_resources resources;
        resources.neural_color = {color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        resources.depth = {depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        resources.motion = {motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        resources.interpolated[0] = {interpolated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        resources.generated_count = 1;
        resources.disable_interpolation = {hint, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        return resources;
    };

    auto placed = [](ID3D12Resource *buffer, unsigned long long bytes)
    {
        D3D12_TEXTURE_COPY_LOCATION location{};
        location.pResource = buffer;
        location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        location.PlacedFootprint.Offset = 0;
        location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_UNKNOWN;
        location.PlacedFootprint.Footprint.Width = static_cast<unsigned>(bytes);
        location.PlacedFootprint.Footprint.Height = 1;
        location.PlacedFootprint.Footprint.Depth = 1;
        location.PlacedFootprint.Footprint.RowPitch = static_cast<unsigned>(bytes);
        return location;
    };

    auto subresource = [](ID3D12Resource *resource)
    {
        D3D12_TEXTURE_COPY_LOCATION location{};
        location.pResource = resource;
        location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        location.SubresourceIndex = 0;
        return location;
    };

    const unsigned long long hint_bytes = 16;
    const unsigned char hint_zero[hint_bytes]{};

    auto clear_hint = [&]() -> bool
    {
        if (!write_upload(upload_hint, hint_zero, hint_bytes)) return false;
        commands->Reset(allocator, nullptr);
        transition(commands, hint, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(commands, upload_hint, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->CopyBufferRegion(hint, 0, upload_hint, 0, hint_bytes);
        transition(commands, upload_hint, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
        transition(commands, hint, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto closed = commands->Close();
        std::printf("CLEAR_HINT close result=0x%08X\nn", static_cast<unsigned>(closed));

        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, 2000);
        const bool ok = wait_fence(fence, 2000, event, 20000);
        if (!ok) std::printf("CLEAR_HINT fence not completed; device_removed=0x%08X\n",
                             static_cast<unsigned>(device->GetDeviceRemovedReason()));
        return ok;
    };

    auto evaluate = [&](const mgpu::secondary_fg::frame_resources &resources,
                        unsigned long long fence_value, const char *label) -> mgpu::secondary_fg::decision
    {
        commands->Reset(allocator, nullptr);
        mgpu::secondary_fg::decision result{};
        std::printf("BEFORE_RECORD %s\n", label);
        const auto status = stage.record(commands, resources, result);
        std::printf("AFTER_RECORD %s\n", label);
        std::printf("EVALUATE %s result=0x%08X reason=%d evaluate=%d reset=%d allow_generated=%d frame_interval_ns=%llu\n",
                    label, static_cast<unsigned>(status), static_cast<int>(result.reason),
                    result.evaluate, result.reset, result.allow_generated, result.frame_interval_ns);
        commands->Close();
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, fence_value);
        if (!wait_fence(fence, fence_value, event, 20000)) { std::puts("FAILED evaluate fence"); return result; }
        if (!stage.track_submission(fence, fence_value)) { std::puts("FAILED track submission"); return result; }
        stage.complete(resources.contract.frame_id, true);
        return result;
    };

    const auto sr = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (!clear_hint()) return 41;
    std::puts("CLEAR_HINT_OK");

    if (!write_upload(upload_color, frame1.color.data(), footprint(4))) return 42;
    if (!write_upload(upload_depth, frame1.depth.data(), footprint(4))) return 42;
    if (!write_upload(upload_motion, frame1.mvec.data(), footprint(kMotionBytes))) return 42;
    if (!upload_frame(upload_color, color, 4, D3D12_RESOURCE_STATE_COMMON, sr, 3001)) return 43;
    if (!upload_frame(upload_depth, depth, 4, D3D12_RESOURCE_STATE_COMMON, sr, 3002)) return 43;
    if (!upload_frame(upload_motion, motion, kMotionBytes, D3D12_RESOURCE_STATE_COMMON, sr, 3003)) return 43;

    auto contract = [&](unsigned long long frame_id, unsigned long long capture_ns, bool reset)
    {
        mgpu::secondary_fg::frame_contract value{};
        value.frame_id = frame_id;
        value.depth_frame_id = frame_id;
        value.motion_frame_id = frame_id;
        value.camera_frame_id = frame_id;
        value.capture_ns = capture_ns;
        value.neural_output = true;
        value.depth_valid = true;
        value.motion_valid = true;
        value.camera_valid = true;
        value.camera_values_valid = true;
        value.reset = reset;
        value.normalized_motion_scale_x = 1.0f;
        value.normalized_motion_scale_y = 1.0f;
        return value;
    };

    auto camera_block = [&](const float (&current)[4][4], const float (&current_inverse)[4][4],
                            const float (&previous)[4][4], const float (&previous_inverse)[4][4],
                            float jitter_x, float jitter_y)
    {
        NVSDK_NGX_DLSSG_Opt_Eval_Params camera{};
        std::memcpy(camera.cameraViewToClip, current, sizeof(camera.cameraViewToClip));
        std::memcpy(camera.clipToCameraView, current_inverse, sizeof(camera.clipToCameraView));
        std::memcpy(camera.clipToLensClip, lens_clip, sizeof(camera.clipToLensClip));
        std::memcpy(camera.cameraViewToClip, current, sizeof(camera.cameraViewToClip));
        std::memcpy(camera.clipToPrevClip, previous, sizeof(camera.clipToPrevClip));
        std::memcpy(camera.prevClipToClip, previous_inverse, sizeof(camera.prevClipToClip));
        camera.jitterOffset[0] = jitter_x;
        camera.jitterOffset[1] = jitter_y;
        camera.mvecScale[0] = 1.0f;
        camera.mvecScale[1] = 1.0f;
        camera.cameraPos[0] = 0.0f; camera.cameraPos[1] = 0.0f; camera.cameraPos[2] = 0.0f;
        camera.cameraUp[0] = 0.0f; camera.cameraUp[1] = 1.0f; camera.cameraUp[2] = 0.0f;
        camera.cameraRight[0] = 1.0f; camera.cameraRight[1] = 0.0f; camera.cameraRight[2] = 0.0f;
        camera.cameraFwd[0] = 0.0f; camera.cameraFwd[1] = 0.0f; camera.cameraFwd[2] = 1.0f;
        camera.cameraNear = 1.0f;
        camera.cameraFar = 100.0f;
        camera.cameraFOV = fov;
        camera.cameraAspectRatio = aspect;
        return camera;
    };

    auto resources1 = make_resources();
    resources1.contract = contract(1, 1000000000ULL, true);
    resources1.camera = camera_block(view_to_clip, clip_to_view, identity, identity, 0.0f, 0.0f);
    evaluate(resources1, 4001, "frame=1");

    if (!write_upload(upload_color, frame2.color.data(), footprint(4))) return 44;
    if (!write_upload(upload_depth, frame2.depth.data(), footprint(4))) return 44;
    if (!write_upload(upload_motion, frame2.mvec.data(), footprint(kMotionBytes))) return 44;
    if (!upload_frame(upload_color, color, 4, sr, sr, 4101)) return 45;
    if (!upload_frame(upload_depth, depth, 4, sr, sr, 4102)) return 45;
    if (!upload_frame(upload_motion, motion, kMotionBytes, sr, sr, 4103)) return 45;

    auto resources2 = make_resources();
    resources2.contract = contract(2, 1016666667ULL, false);
    resources2.camera = camera_block(view_to_clip, clip_to_view, identity, identity, 0.001f, -0.002f);
    evaluate(resources2, 4201, "frame=2");

    commands->Reset(allocator, nullptr);
    transition(commands, interpolated, uav, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_texture(commands, interpolated, readback_color, 4, true);
    transition(commands, interpolated, D3D12_RESOURCE_STATE_COPY_SOURCE, uav);
    transition(commands, hint, uav, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->CopyBufferRegion(readback_hint, 0, hint, 0, hint_bytes);
    transition(commands, hint, D3D12_RESOURCE_STATE_COPY_SOURCE, uav);
    commands->Close();
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, 5000);
    if (!wait_fence(fence, 5000, event, 20000)) return 46;

    void *mapped_output = nullptr;
    void *mapped_hint = nullptr;
    if (FAILED(readback_color->Map(0, nullptr, &mapped_output))) return 47;
    if (FAILED(readback_hint->Map(0, nullptr, &mapped_hint))) return 48;

    const unsigned char *output = static_cast<const unsigned char *>(mapped_output);
    const unsigned int hint_value = *static_cast<const unsigned int *>(mapped_hint);
    const unsigned long long row_pitch = aligned_pitch(4);

    unsigned long long written = 0;
    unsigned long long differs = 0;
    unsigned long long run = 0;
    unsigned long long longest_run = 0;
    for (unsigned y = 0; y < kHeight; ++y)
    {
        for (unsigned x = 0; x < kWidth; ++x)
        {
            const unsigned long long offset = y * row_pitch + static_cast<unsigned long long>(x) * 4;
            const unsigned char r = output[offset + 0];
            const unsigned char g = output[offset + 1];
            const unsigned char b = output[offset + 2];
            const unsigned char a = output[offset + 3];
            if (r == 0 && g == 0 && b == 0 && a == 0) continue;
            ++written;
            const size_t pixel = static_cast<size_t>(y) * kWidth + x;
            const size_t input_base = pixel * 4;
            if (r != frame2.color[input_base] || g != frame2.color[input_base + 1] ||
                b != frame2.color[input_base + 2] || a != frame2.color[input_base + 3])
                ++differs;
            bool same = true;
            if (x > 0)
            {
                const unsigned long long previous = y * row_pitch + static_cast<unsigned long long>(x - 1) * 4;
                same = output[previous] == r && output[previous + 1] == g &&
                       output[previous + 2] == b && output[previous + 3] == a;
            }
            if (same) { ++run; if (run > longest_run) longest_run = run; }
            else run = 1;
        }
    }

    std::printf("READBACK interpolated: written_pixels=%llu differs_from_frame2_input=%llu longest_identical_run=%llu\n",
                written, differs, longest_run);
    std::printf("READBACK disable_interpolation hint=%u\n", hint_value);

    const bool inference_ran = written > 0 && differs > 0;
    std::puts(inference_ran
        ? "INFERENCE_RAN: EvaluateFeature wrote the interpolated UAV with data distinct from the input frame. "
          "Synthetic scene, not a game frame: this proves the call path works, not picture quality."
        : "NO_OUTPUT_WRITTEN: the interpolated UAV is empty or an exact copy of the input.");

    stage.release_completed();
    NVSDK_NGX_Parameter *caps_to_destroy = caps;
    if (caps_to_destroy != nullptr) api.destroy(caps_to_destroy);
    unsigned remaining_sessions = 0;
    shutdown(device, &remaining_sessions);
    readback_color->Release();
    readback_hint->Release();
    upload_color->Release();
    upload_depth->Release();
    upload_motion->Release();
    upload_hint->Release();
    color->Release();
    depth->Release();
    motion->Release();
    interpolated->Release();
    hint->Release();
    CloseHandle(event);
    fence->Release();
    commands->Release();
    allocator->Release();
    queue->Release();
    device->Release();
    selected->Release();
    FreeLibrary(runtime);
    FreeLibrary(module);
    if (process_job != nullptr) CloseHandle(process_job);
    return inference_ran ? 0 : 49;
}
