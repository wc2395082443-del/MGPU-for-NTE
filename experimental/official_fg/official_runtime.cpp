#include <mutex>
#include "official_runtime.hpp"
#include "probe_policy.hpp"
#include "reflex_contract.hpp"
#include <sl_security.h>
#include "fg_mode_policy.hpp"
#include <cstdio>
#include <stdexcept>

namespace mgpu::official_fg
{
static_assert(static_cast<unsigned>(sl::PCLMarker::ePCLatencyPing) == input_ping_marker);
void require_sl(sl::Result result, const char *operation)
{
    if (result != sl::Result::eOk) {
        std::printf("[M349] %s failed sl=%d\n", operation, static_cast<int>(result));
        throw std::runtime_error(operation);
    }
}
void require_hr(HRESULT result, const char *operation)
{
    if (FAILED(result)) {
        std::printf("[M349] %s failed hr=0x%08lx\n", operation, static_cast<unsigned long>(result));
        throw std::runtime_error(operation);
    }
}
namespace
{
template <typename Function> Function *exported(HMODULE module, const char *name)
{
    const auto function = GetProcAddress(module, name);
    if (!function) throw std::runtime_error(name);
    return reinterpret_cast<Function *>(function);
}
void log_message(sl::LogType type, const char *message)
{
    std::printf("SL[%u] %s\n", static_cast<unsigned>(type), message ? message : "");
}
template <typename Interface>
void upgrade(PFun_slUpgradeInterface *function, Interface *native, Microsoft::WRL::ComPtr<Interface> &target)
{
    native->AddRef();
    void *pointer = native;
    const auto result = function(&pointer);
    if (result != sl::Result::eOk) { native->Release(); require_sl(result, "slUpgradeInterface"); }
    target.Attach(static_cast<Interface *>(pointer));
}
}

template <typename Function> Function *official_runtime::feature(sl::Feature identifier, const char *name)
{
    void *address = nullptr;
    require_sl(function_(identifier, name, address), name);
    if (!address) throw std::runtime_error(name);
    return reinterpret_cast<Function *>(address);
}

official_runtime::~official_runtime()
{
    try { close(); } catch (...) {}
    if (module_) FreeLibrary(module_);
    if (search_) RemoveDllDirectory(search_);
}

void official_runtime::initialize(const std::filesystem::path &runtime, const std::filesystem::path &logs, LUID adapter,
    HWND input_window, DWORD input_process)
{
    if (module_ || GetModuleHandleW(L"sl.interposer.dll") || !runtime.is_absolute() || !logs.is_absolute())
        throw std::runtime_error("independent absolute-path runtime required");
    runtime_path_ = runtime.wstring();
    log_path_ = logs.wstring();
    for (const auto *name : {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll", L"sl.reflex.dll", L"sl.pcl.dll"})
        if (!sl::security::verifyEmbeddedSignature((runtime / name).c_str()))
            throw std::runtime_error("official NVIDIA signature check failed");
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS))
        throw std::runtime_error("secure DLL directories");
    search_ = AddDllDirectory(runtime_path_.c_str());
    if (!search_) throw std::runtime_error("runtime search directory");
    module_ = LoadLibraryExW((runtime / L"sl.interposer.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!module_) throw std::runtime_error("load official interposer");
    init_ = exported<PFun_slInit>(module_, "slInit");
    shutdown_ = exported<PFun_slShutdown>(module_, "slShutdown");
    function_ = exported<PFun_slGetFeatureFunction>(module_, "slGetFeatureFunction");
    token_ = exported<PFun_slGetNewFrameToken>(module_, "slGetNewFrameToken");
    constants_ = exported<PFun_slSetConstants>(module_, "slSetConstants");
    tag_ = exported<PFun_slSetTagForFrame>(module_, "slSetTagForFrame");
    const auto set_device = exported<PFun_slSetD3DDevice>(module_, "slSetD3DDevice");
    const auto supported = exported<PFun_slIsFeatureSupported>(module_, "slIsFeatureSupported");
    const auto upgrade_interface = exported<PFun_slUpgradeInterface>(module_, "slUpgradeInterface");
    const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    const wchar_t *paths[] = {runtime_path_.c_str()};
    sl::Preferences preferences{};
    preferences.pathsToPlugins = paths;
    preferences.numPathsToPlugins = 1;
    preferences.pathToLogsAndData = log_path_.c_str();
    preferences.logMessageCallback = log_message;
    preferences.logLevel = sl::LogLevel::eDefault;
    preferences.showConsole = false;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 3;
    preferences.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging |
        sl::PreferenceFlags::eDisableCLStateTracking;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "MGPU-M356-reflex-contract";
    preferences.projectId = "3a9df34e-0d36-47d6-8fa0-9cd799b733e9";
    require_sl(init_(preferences, sl::kSDKVersion), "slInit");
    active_ = true;
    require_hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)), "factory");
    Microsoft::WRL::ComPtr<IDXGIAdapter1> selected;
    require_hr(factory_->EnumAdapterByLuid(adapter, IID_PPV_ARGS(&selected)), "producer adapter LUID");
    DXGI_ADAPTER_DESC1 description{};
    require_hr(selected->GetDesc1(&description), "adapter description");
    if (!matches_adapter(description.VendorId, (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0,
        description.Description)) throw std::runtime_error("official helper is restricted to RTX5080");
    sl::AdapterInfo information{};
    information.deviceLUID = reinterpret_cast<uint8_t *>(&adapter);
    information.deviceLUIDSizeInBytes = sizeof(adapter);
    for (auto requested : features) require_sl(supported(requested, information), "required official feature");
    require_hr(D3D12CreateDevice(selected.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)), "5080 device");
    require_sl(set_device(device_.Get()), "set independent device");
    upgrade(upgrade_interface, device_.Get(), device_proxy_);
    upgrade(upgrade_interface, factory_.Get(), factory_proxy_);
    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    require_hr(device_proxy_->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue_proxy_)), "official presenting queue");
    const auto native = exported<PFun_slGetNativeInterface>(module_, "slGetNativeInterface");
    require_sl(native(queue_proxy_.Get(), reinterpret_cast<void **>(queue_.GetAddressOf())), "native presenting queue");
    state_ = feature<PFun_slDLSSGGetState>(sl::kFeatureDLSS_G, "slDLSSGGetState");
    options_ = feature<PFun_slDLSSGSetOptions>(sl::kFeatureDLSS_G, "slDLSSGSetOptions");
    sleep_ = feature<PFun_slReflexSleep>(sl::kFeatureReflex, "slReflexSleep");
    reflex_options_ = feature<PFun_slReflexSetOptions>(sl::kFeatureReflex, "slReflexSetOptions");
    marker_ = feature<PFun_slPCLSetMarker>(sl::kFeaturePCL, "slPCLSetMarker");
    auto pcl_options = feature<PFun_slPCLSetOptions>(sl::kFeaturePCL, "slPCLSetOptions");
    sl::ReflexOptions reflex{};
    reflex.mode = sl::ReflexMode::eLowLatency;
    require_sl(reflex_options_(reflex), "official Reflex low latency");
    DWORD window_process = 0;
    const DWORD input_thread = GetWindowThreadProcessId(input_window, &window_process);
    if (!input_thread || !input_process || window_process != input_process)
        throw std::runtime_error("PCL source input window ownership changed");
    sl::PCLOptions pcl{};
    pcl.idThread = input_thread;
    require_sl(pcl_options(pcl), "PCL source input thread after Reflex options");
    std::printf("[M356-PCL] source_pid=%lu input_thread=%lu; native game ping forwarding required; cross-process latency report not yet validated\n",
        input_process, input_thread);
    const auto initial_state = state();
    maximum_ = initial_state.numFramesToGenerateMax + 1u;
    dynamic_mfg_supported_ = initial_state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    std::printf("[M349] independent official runtime initialized adapter=%08lx:%08lx max_multiplier=%u\n",
        static_cast<unsigned long>(adapter.HighPart), adapter.LowPart, maximum_);
}

void official_runtime::configure(unsigned multiplier)
{
    if (!supports_multiplier(multiplier, maximum_ ? maximum_ - 1 : 0))
        throw std::runtime_error("requested multiplier not supported; refusing silent downgrade");
    const auto requested_mode = mode_for_multiplier(multiplier);
    if (requested_mode == fg_mode::dynamic && !dynamic_mfg_supported_)
        throw std::runtime_error("dynamic MFG requested but runtime does not report support");
    sl::DLSSGOptions options{};
    options.mode = requested_mode == fg_mode::dynamic ? sl::DLSSGMode::eDynamic : sl::DLSSGMode::eOn;
    options.numFramesToGenerate = multiplier - 1;
    options.dynamicTargetFrameRate = 200.0f;
    options.numBackBuffers = 3;
    require_sl(options_(viewport_, options), "official FG options");
    if (configured_multiplier_ != multiplier || configured_mode_ != requested_mode) {
        std::printf("[M360-FG-MODE] requested=%ux mode=%s generated_limit=%u dynamic_supported=%u\n",
            multiplier, requested_mode == fg_mode::dynamic ? "dynamic" : "fixed",
            options.numFramesToGenerate, dynamic_mfg_supported_ ? 1u : 0u);
        configured_multiplier_ = multiplier;
        configured_mode_ = requested_mode;
        std::printf("[CONTROL-ROLLBACK] official dynamic target=200 FPS; SDK pacing retained\n");
    }
}

void official_runtime::create_swapchain(HWND window, unsigned width, unsigned height, DXGI_FORMAT format, unsigned multiplier)
{
    if (swapchain_) throw std::runtime_error("swapchain already exists");
    configure(multiplier);
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 3;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain;
    require_hr(factory_proxy_->CreateSwapChainForHwnd(queue_proxy_.Get(), window, &description, nullptr, nullptr, &swapchain),
        "official HWND swapchain");
    require_hr(swapchain.As(&swapchain_), "official swapchain3");
    const auto native = exported<PFun_slGetNativeInterface>(module_, "slGetNativeInterface");
    Microsoft::WRL::ComPtr<IDXGISwapChain> original;
    require_sl(native(swapchain_.Get(), reinterpret_cast<void **>(original.GetAddressOf())), "native swapchain verification");
    Microsoft::WRL::ComPtr<IUnknown> proxy_identity, original_identity;
    require_hr(swapchain_.As(&proxy_identity), "proxy identity");
    require_hr(original.As(&original_identity), "native identity");
    if (proxy_identity.Get() == original_identity.Get()) throw std::runtime_error("official swapchain proxy missing");
}

sl::FrameToken *official_runtime::token(std::uint32_t frame)
{
    std::lock_guard<std::mutex> lock(token_mutex_);
    sl::FrameToken *result = nullptr;
    require_sl(token_(result, &frame), "source frame token");
    if (!result) throw std::runtime_error("null frame token");
    return result;
}
void official_runtime::source_sleep(std::uint32_t frame) { require_sl(sleep_(*token(frame)), "source-linked Reflex sleep"); }
void official_runtime::marker(std::uint32_t frame, unsigned kind)
{
    require_sl(marker_(static_cast<sl::PCLMarker>(kind), *token(frame)), "source-linked PCL marker");
}
void official_runtime::constants(const sl::Constants &values, std::uint32_t frame)
{
    require_sl(constants_(values, *token(frame), viewport_), "same-frame constants");
}
void official_runtime::tag(std::uint32_t frame, const sl::ResourceTag *resources, unsigned count)
{
    require_sl(tag_(*token(frame), viewport_, resources, count, nullptr), "same-frame resource tags");
}
sl::DLSSGState official_runtime::state()
{
    sl::DLSSGState result{};
    require_sl(state_(viewport_, result, nullptr), "FG state on presentation thread");
    return result;
}
void official_runtime::close()
{
    if (!active_) return;
    if (options_) {
        sl::DLSSGOptions options{};
        options.mode = sl::DLSSGMode::eOff;
        require_sl(options_(viewport_, options), "disable FG for shutdown");
    }
    swapchain_.Reset();
    if (reflex_options_) {
        sl::ReflexOptions options{};
        options.mode = sl::ReflexMode::eOff;
        require_sl(reflex_options_(options), "release helper Reflex mode");
    }
    require_sl(shutdown_(), "official shutdown");
    active_ = false;
    queue_.Reset();
    queue_proxy_.Reset();
    device_proxy_.Reset();
    factory_proxy_.Reset();
    device_.Reset();
    factory_.Reset();
}
}
