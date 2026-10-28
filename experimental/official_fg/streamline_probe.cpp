#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_security.h>
#include <filesystem>
#include <cstdio>
#include <stdexcept>
#include <string>
#include "probe_policy.hpp"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void check_hr(HRESULT result, const char *name)
{
    std::printf("STEP %s hr=0x%08lx\n", name, static_cast<unsigned long>(result));
    require(SUCCEEDED(result), name);
}

void check_sl(sl::Result result, const char *name)
{
    std::printf("STEP %s sl_result=%d\n", name, static_cast<int>(result));
    require(result == sl::Result::eOk, name);
}

void sdk_log(sl::LogType type, const char *message)
{
    std::printf("SDK[%u] %s\n", static_cast<unsigned>(type), message ? message : "");
}

template <typename Function>
Function *resolve(HMODULE module, const char *name)
{
    const auto address = GetProcAddress(module, name);
    require(address != nullptr, name);
    return reinterpret_cast<Function *>(address);
}

struct module_owner
{
    HMODULE handle = nullptr;
    ~module_owner() { if (handle) FreeLibrary(handle); }
};

struct search_directory
{
    DLL_DIRECTORY_COOKIE cookie = nullptr;
    ~search_directory() { if (cookie) RemoveDllDirectory(cookie); }
};

struct hidden_window
{
    HWND handle = nullptr;
    ATOM atom = 0;
    ~hidden_window()
    {
        if (handle) DestroyWindow(handle);
        if (atom) UnregisterClassW(MAKEINTATOM(atom), GetModuleHandleW(nullptr));
    }
    void create()
    {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = L"MGPU_M348_Hidden_Probe";
        atom = RegisterClassW(&window_class);
        require(atom != 0, "RegisterClassW");
        handle = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            MAKEINTATOM(atom), L"MGPU initialization probe", WS_POPUP,
            0, 0, 1280, 720, nullptr, nullptr, window_class.hInstance, nullptr);
        require(handle != nullptr && !IsWindowVisible(handle), "hidden window creation");
    }
};

struct api
{
    PFun_slInit *init;
    PFun_slShutdown *shutdown;
    PFun_slSetD3DDevice *set_device;
    PFun_slIsFeatureSupported *supported;
    PFun_slIsFeatureLoaded *loaded;
    PFun_slGetFeatureFunction *feature;
    PFun_slUpgradeInterface *upgrade;
    PFun_slGetNativeInterface *native;
    explicit api(HMODULE module)
        : init(resolve<PFun_slInit>(module, "slInit")),
          shutdown(resolve<PFun_slShutdown>(module, "slShutdown")),
          set_device(resolve<PFun_slSetD3DDevice>(module, "slSetD3DDevice")),
          supported(resolve<PFun_slIsFeatureSupported>(module, "slIsFeatureSupported")),
          loaded(resolve<PFun_slIsFeatureLoaded>(module, "slIsFeatureLoaded")),
          feature(resolve<PFun_slGetFeatureFunction>(module, "slGetFeatureFunction")),
          upgrade(resolve<PFun_slUpgradeInterface>(module, "slUpgradeInterface")),
          native(resolve<PFun_slGetNativeInterface>(module, "slGetNativeInterface")) {}

    template <typename Function>
    Function *get(sl::Feature requested, const char *name)
    {
        void *address = nullptr;
        check_sl(feature(requested, name, address), name);
        require(address != nullptr, name);
        return reinterpret_cast<Function *>(address);
    }
};

struct session
{
    api &functions;
    bool active = false;
    explicit session(api &value) : functions(value) {}
    ~session()
    {
        if (active) {
            const auto result = functions.shutdown();
            std::printf("CLEANUP slShutdown=%d\n", static_cast<int>(result));
        }
    }
    void close()
    {
        const auto result = functions.shutdown();
        active = false;
        check_sl(result, "slShutdown");
    }
};

template <typename Interface>
ComPtr<Interface> upgraded(api &functions, Interface *original)
{
    original->AddRef();
    void *pointer = original;
    const auto result = functions.upgrade(&pointer);
    if (result != sl::Result::eOk) {
        original->Release();
        check_sl(result, "slUpgradeInterface");
    }
    ComPtr<Interface> proxy;
    proxy.Attach(static_cast<Interface *>(pointer));
    return proxy;
}

void inspect_or_initialize(mgpu::official_fg::probe_mode mode, const fs::path &runtime, const fs::path &logs)
{
    require(GetModuleHandleW(L"sl.interposer.dll") == nullptr,
        "refusing to reuse an existing process Streamline runtime");
    for (const auto *name : {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll",
                            L"sl.reflex.dll", L"sl.pcl.dll"}) {
        const auto file = runtime / name;
        require(fs::is_regular_file(file), "official runtime file missing");
        require(sl::security::verifyEmbeddedSignature(file.c_str()), "NVIDIA signature validation failed");
    }
    require(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS) != FALSE,
        "SetDefaultDllDirectories");
    search_directory search;
    search.cookie = AddDllDirectory(runtime.c_str());
    require(search.cookie != nullptr, "AddDllDirectory");
    module_owner module;
    module.handle = LoadLibraryExW((runtime / L"sl.interposer.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    require(module.handle != nullptr, "LoadLibraryExW official interposer");
    api functions(module.handle);
    std::printf("EXPORTS verified; SDK=%d.%d.%d pid=%lu\n",
        SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH, GetCurrentProcessId());
    if (mode == mgpu::official_fg::probe_mode::verify) return;

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> selected;
    ComPtr<ID3D12Device> device;
    hidden_window window;
    session lifetime(functions);
    const auto plugin_path = runtime.wstring();
    const auto log_path = logs.wstring();
    const wchar_t *plugin_paths[] = {plugin_path.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences preferences{};
    preferences.showConsole = false;
    preferences.logLevel = sl::LogLevel::eDefault;
    preferences.logMessageCallback = sdk_log;
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = 1;
    preferences.pathToLogsAndData = log_path.c_str();
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 3;
    preferences.flags = sl::PreferenceFlags::eUseManualHooking |
        sl::PreferenceFlags::eUseFrameBasedResourceTagging | sl::PreferenceFlags::eDisableCLStateTracking;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "MGPU-M348-isolation-probe";
    preferences.projectId = "3a9df34e-0d36-47d6-8fa0-9cd799b733e9";
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    check_sl(functions.init(preferences, sl::kSDKVersion), "slInit independent process");
    lifetime.active = true;
    check_hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    DXGI_ADAPTER_DESC1 selected_description{};
    unsigned matching_adapters = 0;
    for (unsigned adapter_index = 0; ; ++adapter_index) {
        ComPtr<IDXGIAdapter1> candidate;
        const auto enumeration = factory->EnumAdapters1(adapter_index, &candidate);
        if (enumeration == DXGI_ERROR_NOT_FOUND) break;
        check_hr(enumeration, "EnumAdapters1");
        DXGI_ADAPTER_DESC1 description{};
        check_hr(candidate->GetDesc1(&description), "GetDesc1");
        std::printf("ADAPTER index=%u name=%ls luid=%08lx:%08lx\n", adapter_index,
            description.Description, static_cast<unsigned long>(description.AdapterLuid.HighPart),
            description.AdapterLuid.LowPart);
        if (mgpu::official_fg::matches_adapter(description.VendorId,
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0, description.Description)) {
            ++matching_adapters;
            selected = candidate;
            selected_description = description;
        }
    }
    require(matching_adapters == 1, "exactly one RTX 5080 required; no adapter fallback");
    sl::AdapterInfo adapter_info{};
    adapter_info.deviceLUID = reinterpret_cast<uint8_t *>(&selected_description.AdapterLuid);
    adapter_info.deviceLUIDSizeInBytes = sizeof(LUID);
    for (const auto requested : features) {
        const auto support = functions.supported(requested, adapter_info);
        std::printf("FEATURE id=%u support=%d\n", requested, static_cast<int>(support));
        check_sl(support, "slIsFeatureSupported RTX5080");
    }
    check_hr(D3D12CreateDevice(selected.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)),
        "D3D12CreateDevice RTX5080");
    const auto actual_luid = device->GetAdapterLuid();
    require(actual_luid.LowPart == selected_description.AdapterLuid.LowPart &&
        actual_luid.HighPart == selected_description.AdapterLuid.HighPart, "device LUID mismatch");
    check_sl(functions.set_device(device.Get()), "slSetD3DDevice RTX5080 only");
    for (const auto requested : features) {
        bool loaded = false;
        check_sl(functions.loaded(requested, loaded), "slIsFeatureLoaded");
        std::printf("FEATURE id=%u loaded=%u\n", requested, loaded ? 1u : 0u);
        require(loaded, "required feature was not loaded");
    }
    auto get_fg = functions.get<PFun_slDLSSGGetState>(sl::kFeatureDLSS_G, "slDLSSGGetState");
    auto set_fg = functions.get<PFun_slDLSSGSetOptions>(sl::kFeatureDLSS_G, "slDLSSGSetOptions");
    auto get_reflex = functions.get<PFun_slReflexGetState>(sl::kFeatureReflex, "slReflexGetState");
    functions.get<PFun_slReflexSleep>(sl::kFeatureReflex, "slReflexSleep");
    functions.get<PFun_slPCLSetMarker>(sl::kFeaturePCL, "slPCLSetMarker");
    sl::ReflexState reflex{};
    check_sl(get_reflex(reflex), "slReflexGetState");
    std::printf("REFLEX lowLatencyAvailable=%u\n", reflex.lowLatencyAvailable ? 1u : 0u);
    require(reflex.lowLatencyAvailable, "Reflex low latency not available");
    const sl::ViewportHandle viewport(0);
    sl::DLSSGState fg_state{};
    check_sl(get_fg(viewport, fg_state, nullptr), "slDLSSGGetState initial");
    std::printf("CAPABILITY max_generated=%u max_multiplier=%u status=0x%x\n",
        fg_state.numFramesToGenerateMax, fg_state.numFramesToGenerateMax + 1,
        static_cast<unsigned>(fg_state.status));
    require(mgpu::official_fg::supports_multiplier(6, fg_state.numFramesToGenerateMax),
        "runtime did not report sixfold capability");

    if (mode == mgpu::official_fg::probe_mode::hidden_swapchain) {
        window.create();
        auto proxy_device = upgraded(functions, device.Get());
        auto proxy_factory = upgraded(functions, factory.Get());
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check_hr(proxy_device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue)),
            "official proxy CreateCommandQueue");
        sl::DLSSGOptions options{};
        options.mode = sl::DLSSGMode::eOn;
        options.numFramesToGenerate = 5;
        options.numBackBuffers = 3;
        options.colorWidth = 1280;
        options.colorHeight = 720;
        options.colorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        check_sl(set_fg(viewport, options), "slDLSSGSetOptions sixfold");
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = 1280;
        description.Height = 720;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 3;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> chain;
        check_hr(proxy_factory->CreateSwapChainForHwnd(queue.Get(), window.handle, &description,
            nullptr, nullptr, &chain), "official CreateSwapChainForHwnd hidden");
        ComPtr<IDXGISwapChain3> chain3;
        check_hr(chain.As(&chain3), "QueryInterface IDXGISwapChain3");
        std::printf("CHAIN backbuffer_index=%u visible=%u\n", chain3->GetCurrentBackBufferIndex(),
            IsWindowVisible(window.handle) ? 1u : 0u);
        ComPtr<IDXGISwapChain> native_chain;
        check_sl(functions.native(chain.Get(), reinterpret_cast<void **>(native_chain.GetAddressOf())),
            "slGetNativeInterface swapchain");
        ComPtr<IUnknown> proxy_identity;
        ComPtr<IUnknown> native_identity;
        check_hr(chain.As(&proxy_identity), "swapchain proxy identity");
        check_hr(native_chain.As(&native_identity), "swapchain native identity");
        require(proxy_identity.Get() != native_identity.Get(), "swapchain was not proxied");
        require(!IsWindowVisible(window.handle), "probe window unexpectedly visible");
        check_sl(get_fg(viewport, fg_state, nullptr), "slDLSSGGetState after swapchain");
        std::printf("PROXY verified=1 max_generated=%u status=0x%x presents=%u\n",
            fg_state.numFramesToGenerateMax, static_cast<unsigned>(fg_state.status),
            fg_state.numFramesActuallyPresented);
        require(fg_state.numFramesActuallyPresented == 0, "unexpected presentation");
        options.mode = sl::DLSSGMode::eOff;
        check_sl(set_fg(viewport, options), "slDLSSGSetOptions off for teardown");
    }
    lifetime.close();
    std::printf("INITIALIZATION complete; no inference, no Present, no game resources, no source Reflex claim\n");
}
}

int wmain(int argument_count, wchar_t **arguments)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argument_count != 4 || mgpu::official_fg::parse_mode(arguments[1]) == mgpu::official_fg::probe_mode::invalid) {
        std::printf("Usage: streamline_probe (--verify|--initialize-5080|--hidden-swapchain-5080) absolute_runtime_dir absolute_log_dir\n");
        return 2;
    }
    try {
        const fs::path runtime(arguments[2]);
        const fs::path logs(arguments[3]);
        require(runtime.is_absolute() && logs.is_absolute(), "absolute directories required");
        require(fs::is_directory(runtime) && fs::is_directory(logs), "directories must exist");
        inspect_or_initialize(mgpu::official_fg::parse_mode(arguments[1]), runtime, logs);
        std::printf("RESULT PASS\n");
        return 0;
    } catch (const std::exception &error) {
        std::printf("RESULT FAIL %s last_error=%lu\n", error.what(), GetLastError());
        return 1;
    }
}
