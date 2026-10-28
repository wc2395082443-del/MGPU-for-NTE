#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cwchar>
#include "../../ext/ngx/nvsdk_ngx.h"
#include "../../ext/ngx/nvsdk_ngx_defs_dlssg.h"
#include "parameter_bridge.hpp"

static void NVSDK_CONV log_ngx(const char *message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature feature)
{
    if (message != nullptr)
        std::printf("NGX_LOG level=%u feature=%u %.1024s\n", static_cast<unsigned>(level),
            static_cast<unsigned>(feature), message);
}

static int initialize_fg(HMODULE module, IDXGIAdapter1 *adapter, const wchar_t *data_path,
                         const NVSDK_NGX_FeatureCommonInfo &common)
{
    using init_function = NVSDK_NGX_Result (NVSDK_CONV *)(const char *, NVSDK_NGX_EngineType,
        const char *, const wchar_t *, ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *);
    using parameters_function = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter **);
    using destroy_function = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Parameter *);
    using shutdown_function = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12Device *, unsigned int *);
    using create_function = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *,
        NVSDK_NGX_Feature, const NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
    using release_function = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle *);
    const auto init = reinterpret_cast<init_function>(GetProcAddress(module, "NVSDK_NGX_D3D12_Init_ProjectID"));
    const auto capabilities = reinterpret_cast<parameters_function>(GetProcAddress(module, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    const auto allocate = reinterpret_cast<parameters_function>(GetProcAddress(module, "NVSDK_NGX_D3D12_AllocateParameters"));
    const auto destroy = reinterpret_cast<destroy_function>(GetProcAddress(module, "NVSDK_NGX_D3D12_DestroyParameters"));
    const auto shutdown = reinterpret_cast<shutdown_function>(GetProcAddress(module, "NVSDK_NGX_D3D12_Shutdown1"));
    const auto create = reinterpret_cast<create_function>(GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature"));
    const auto release = reinterpret_cast<release_function>(GetProcAddress(module, "NVSDK_NGX_D3D12_ReleaseFeature"));
    const PFN_NVSDK_NGX_Parameter_GetI get_int = &probe_parameter_get_int;
    const PFN_NVSDK_NGX_Parameter_SetUI set_uint = &probe_parameter_set_uint;
    if (init == nullptr || capabilities == nullptr || allocate == nullptr || destroy == nullptr ||
        shutdown == nullptr || create == nullptr || release == nullptr)
    {
        std::printf("UNRESOLVED init=%d caps=%d allocate=%d destroy=%d shutdown=%d create=%d release=%d\n",
            init != nullptr, capabilities != nullptr, allocate != nullptr, destroy != nullptr,
            shutdown != nullptr, create != nullptr, release != nullptr);
        return 11;
    }
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *commands = nullptr;
    ID3D12Fence *fence = nullptr;
    NVSDK_NGX_Parameter *caps = nullptr;
    NVSDK_NGX_Parameter *parameters = nullptr;
    NVSDK_NGX_Handle *handle = nullptr;
    HANDLE completed_event = nullptr;
    bool initialized = false;
    bool submitted = false;
    bool gpu_completed = false;
    auto cleanup = [&]()
    {
        if (submitted && !gpu_completed)
        {
            std::puts("REFUSED_UNSAFE_CLEANUP: GPU completion not confirmed; isolated process will exit.");
            std::fflush(stdout);
            ExitProcess(30);
        }
        if (handle != nullptr) std::printf("FG_RELEASE result=0x%08X\n", static_cast<unsigned>(release(handle)));
        if (parameters != nullptr) std::printf("PARAMETERS_DESTROY result=0x%08X\n", static_cast<unsigned>(destroy(parameters)));
        if (caps != nullptr && caps != parameters) std::printf("CAPS_DESTROY result=0x%08X\n", static_cast<unsigned>(destroy(caps)));
        if (initialized)
        {
            unsigned int remaining_sessions = 0;
            std::puts("NGX_SHUTDOWN_BEGIN");
            const auto shutdown_status = shutdown(device, &remaining_sessions);
            std::printf("NGX_SHUTDOWN result=0x%08X remaining_sessions=%u\n",
                static_cast<unsigned>(shutdown_status), remaining_sessions);
        }
        if (commands != nullptr) commands->Release();
        if (allocator != nullptr) allocator->Release();
        if (queue != nullptr) queue->Release();
        if (fence != nullptr) fence->Release();
        if (completed_event != nullptr) CloseHandle(completed_event);
        if (device != nullptr) device->Release();
    };
    auto fail = [&](const char *step, unsigned status)
    {
        std::printf("FAILED step=%s result=0x%08X\n", step, status);
        cleanup();
        return 12;
    };
    auto status = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
        __uuidof(ID3D12Device), reinterpret_cast<void **>(&device));
    if (FAILED(status)) return fail("D3D12CreateDevice", static_cast<unsigned>(status));
    const auto luid = device->GetAdapterLuid();
    std::printf("DEVICE_CREATED LUID=%08X:%08X\n", static_cast<unsigned>(luid.HighPart), static_cast<unsigned>(luid.LowPart));
    const auto init_status = init("031cb83c-5d5b-4e7e-8091-266178350ffa", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "secondary-fg-prototype-0.1", data_path, device, NVSDK_NGX_Version_API, &common);
    std::printf("NGX_INIT result=0x%08X\n", static_cast<unsigned>(init_status));
    if (NVSDK_NGX_FAILED(init_status)) return fail("Init_ProjectID", static_cast<unsigned>(init_status));
    initialized = true;
    auto ngx_status = capabilities(&caps);
    if (NVSDK_NGX_FAILED(ngx_status) || caps == nullptr) return fail("GetCapabilityParameters", static_cast<unsigned>(ngx_status));
    int available = 0;
    ngx_status = get_int(caps, NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
    std::printf("FG_AVAILABLE result=0x%08X available=%d\n", static_cast<unsigned>(ngx_status), available);
    if (NVSDK_NGX_FAILED(ngx_status) || available == 0) return fail("FrameGeneration.Available", static_cast<unsigned>(ngx_status));
    ngx_status = allocate(&parameters);
    if (NVSDK_NGX_FAILED(ngx_status) || parameters == nullptr) return fail("AllocateParameters", static_cast<unsigned>(ngx_status));
    set_uint(parameters, NVSDK_NGX_Parameter_CreationNodeMask, 1);
    set_uint(parameters, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
    set_uint(parameters, NVSDK_NGX_Parameter_Width, 1280);
    set_uint(parameters, NVSDK_NGX_Parameter_Height, 720);
    set_uint(parameters, NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, DXGI_FORMAT_R8G8B8A8_UNORM);
    set_uint(parameters, NVSDK_NGX_DLSSG_Parameter_InternalWidth, 1280);
    set_uint(parameters, NVSDK_NGX_DLSSG_Parameter_InternalHeight, 720);
    set_uint(parameters, NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0);
    set_uint(parameters, NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, 0);
    std::puts("FG_CREATE_PROTOCOL: parameters match official NGX_D3D12_CREATE_DLSSG; no generic scratch query.");
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    status = device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&queue));
    if (FAILED(status)) return fail("CreateCommandQueue", static_cast<unsigned>(status));
    status = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&allocator));
    if (FAILED(status)) return fail("CreateCommandAllocator", static_cast<unsigned>(status));
    status = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
        __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&commands));
    if (FAILED(status)) return fail("CreateCommandList", static_cast<unsigned>(status));
    status = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&fence));
    if (FAILED(status)) return fail("CreateFence", static_cast<unsigned>(status));
    completed_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (completed_event == nullptr) return fail("CreateEvent", GetLastError());
    std::puts("FG_CREATE_BEGIN 1280x720 feature=11 one creation, no EvaluateFeature calls");
    ngx_status = create(commands, NVSDK_NGX_Feature_FrameGeneration, parameters, &handle);
    std::printf("FG_CREATE_END result=0x%08X handle_present=%d\n", static_cast<unsigned>(ngx_status), handle != nullptr);
    if (NVSDK_NGX_FAILED(ngx_status) || handle == nullptr) return fail("CreateFeature", static_cast<unsigned>(ngx_status));
    status = commands->Close();
    if (FAILED(status)) return fail("Close", static_cast<unsigned>(status));
    ID3D12CommandList *lists[] = {commands};
    queue->ExecuteCommandLists(1, lists);
    submitted = true;
    status = queue->Signal(fence, 1);
    if (FAILED(status)) return fail("Signal", static_cast<unsigned>(status));
    status = fence->SetEventOnCompletion(1, completed_event);
    if (FAILED(status)) return fail("SetEventOnCompletion", static_cast<unsigned>(status));
    const auto wait = WaitForSingleObject(completed_event, 5000);
    const auto completed = fence->GetCompletedValue();
    gpu_completed = wait == WAIT_OBJECT_0 && completed != UINT64_MAX && completed >= 1;
    if (!gpu_completed) return fail("GPU completion", wait);
    status = device->GetDeviceRemovedReason();
    if (FAILED(status)) return fail("GetDeviceRemovedReason", static_cast<unsigned>(status));
    std::puts("FG_INITIALIZATION_PASSED: official NVIDIA FG feature created on 5080 and init fence completed. No frames generated.");
    cleanup();
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const bool initialize = argc == 5 && std::wcscmp(argv[4], L"--initialize-fg") == 0;
    if ((argc != 4 && !initialize) || std::wcscmp(argv[1], L"--allow-sdk-query") != 0)
    {
        std::puts("Usage: capability_probe --allow-sdk-query <absolute official _nvngx.dll> <existing log directory> [--initialize-fg]");
        std::puts("Queries NVIDIA requirements on RTX 5080 only. --initialize-fg also creates a device and FG feature, never a game process.");
        return 2;
    }
    const auto process_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (process_job == nullptr ||
        !SetInformationJobObject(process_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(process_job, GetCurrentProcess()))
    {
        std::puts("REFUSED: unable to contain probe and runtime child processes.");
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
        if (FAILED(status)) { if (selected != nullptr) selected->Release(); factory->Release(); return 5; }
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
                std::printf("SELECTED RTX 5080 LUID=%08X:%08X VRAM=%llu bytes\n",
                            static_cast<unsigned>(desc.AdapterLuid.HighPart),
                            static_cast<unsigned>(desc.AdapterLuid.LowPart),
                            static_cast<unsigned long long>(desc.DedicatedVideoMemory));
            }
        }
        if (adapter != nullptr) adapter->Release();
    }
    factory->Release();
    if (selected == nullptr || matches != 1)
    {
        if (selected != nullptr) selected->Release();
        std::puts("REFUSED: expected exactly one RTX 5080. No fallback to another GPU.");
        return 6;
    }
    const auto module = LoadLibraryExW(argv[2], nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) { selected->Release(); return 7; }
    using query_function = NVSDK_NGX_Result (NVSDK_CONV *)(IDXGIAdapter *,
                            const NVSDK_NGX_FeatureDiscoveryInfo *, NVSDK_NGX_FeatureRequirement *);
    const auto query = reinterpret_cast<query_function>(
        GetProcAddress(module, "NVSDK_NGX_D3D12_GetFeatureRequirements"));
    if (query == nullptr)
    {
        std::puts("UNRESOLVED: NVIDIA core does not export GetFeatureRequirements.");
        selected->Release();
        FreeLibrary(module);
        return 8;
    }
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
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    common.LoggingInfo.DisableOtherLoggingSinks = true;
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc.ProjectId = "031cb83c-5d5b-4e7e-8091-266178350ffa";
    discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    discovery.Identifier.v.ProjectDesc.EngineVersion = "secondary-fg-prototype-0.1";
    discovery.ApplicationDataPath = argv[3];
    discovery.FeatureInfo = &common;
    NVSDK_NGX_FeatureRequirement requirements{};
    requirements.FeatureSupported = NVSDK_NGX_FeatureSupportResult_CheckNotPresent;
    const auto status = query(selected, &discovery, &requirements);
    std::printf("DLSS_FG_REQUIREMENTS result=0x%08X support_flags=0x%X min_arch=0x%X min_os=%.254s\n",
                static_cast<unsigned>(status), static_cast<unsigned>(requirements.FeatureSupported),
                requirements.MinHWArchitecture, requirements.MinOSVersion);
    const bool supported = !NVSDK_NGX_FAILED(status) &&
        requirements.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported;
    int result = supported ? 0 : 10;
    if (supported && initialize)
    {
        wchar_t runtime_path[MAX_PATH]{};
        const auto path_length = std::swprintf(runtime_path, MAX_PATH, L"%ls\\nvngx.dll", driver_folder);
        const auto runtime = path_length > 0 && path_length < MAX_PATH ? LoadLibraryExW(runtime_path, nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
        if (runtime == nullptr)
        {
            std::printf("FAILED official runtime load error=%lu\n", GetLastError());
            result = 14;
        }
        else
        {
            std::puts("INITIALIZATION_ENTRY: official DriverStore nvngx.dll runtime");
            result = initialize_fg(runtime, selected, argv[3], common);
            FreeLibrary(runtime);
        }
    }
    else std::puts("This is a requirements query, NOT successful FG initialization, inference, or presentation.");
    selected->Release();
    FreeLibrary(module);
    return result;
}
