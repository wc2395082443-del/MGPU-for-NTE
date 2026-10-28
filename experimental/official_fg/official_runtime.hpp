#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include "fg_mode_policy.hpp"
#include <filesystem>
#include <mutex>

namespace mgpu::official_fg
{
class official_runtime
{
public:
    official_runtime() = default;
    official_runtime(const official_runtime &) = delete;
    official_runtime &operator=(const official_runtime &) = delete;
    ~official_runtime();
    void initialize(const std::filesystem::path &runtime, const std::filesystem::path &logs, LUID adapter,
        HWND input_window, DWORD input_process);
    void create_swapchain(HWND window, unsigned width, unsigned height, DXGI_FORMAT format, unsigned multiplier);
    void configure(unsigned multiplier);
    sl::FrameToken *token(std::uint32_t frame);
    void source_sleep(std::uint32_t frame);
    void marker(std::uint32_t frame, unsigned kind);
    void constants(const sl::Constants &values, std::uint32_t frame);
    void tag(std::uint32_t frame, const sl::ResourceTag *resources, unsigned count);
    sl::DLSSGState state();
    void close();
    ID3D12Device *device() const { return device_.Get(); }
    ID3D12CommandQueue *queue() const { return queue_.Get(); }
    IDXGISwapChain3 *swapchain() const { return swapchain_.Get(); }
    unsigned maximum_multiplier() const { return maximum_; }
private:
    HMODULE module_ = nullptr;
    DLL_DIRECTORY_COOKIE search_ = nullptr;
    bool active_ = false;
    unsigned maximum_ = 0;
    std::wstring runtime_path_, log_path_;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory_, factory_proxy_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_, device_proxy_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_, queue_proxy_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain_;
    sl::ViewportHandle viewport_{0};
    PFun_slInit *init_ = nullptr;
    PFun_slShutdown *shutdown_ = nullptr;
    PFun_slGetFeatureFunction *function_ = nullptr;
    PFun_slGetNewFrameToken *token_ = nullptr;
    PFun_slSetConstants *constants_ = nullptr;
    PFun_slSetTagForFrame *tag_ = nullptr;
    PFun_slDLSSGGetState *state_ = nullptr;
    PFun_slDLSSGSetOptions *options_ = nullptr;
    PFun_slReflexSleep *sleep_ = nullptr;
    PFun_slReflexSetOptions *reflex_options_ = nullptr;
    PFun_slPCLSetMarker *marker_ = nullptr;
    std::mutex token_mutex_;
    bool dynamic_mfg_supported_ = false;
    unsigned configured_multiplier_ = 0;
    fg_mode configured_mode_ = fg_mode::fixed;
    template <typename Function> Function *feature(sl::Feature identifier, const char *name);
};
void require_sl(sl::Result result, const char *operation);
void require_hr(HRESULT result, const char *operation);
}
