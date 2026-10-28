#pragma once

#include <windows.h>
#include <d3d12.h>
#include "../../ext/ngx/nvsdk_ngx.h"
#include "../../ext/ngx/nvsdk_ngx_defs_dlssg.h"
#include "../../ext/ngx/nvsdk_ngx_params_dlssg.h"
#include "frame_contract.hpp"

namespace mgpu::secondary_fg
{
struct ngx_api
{
    NVSDK_NGX_Result (NVSDK_CONV *allocate)(NVSDK_NGX_Parameter **) = nullptr;
    NVSDK_NGX_Result (NVSDK_CONV *destroy)(NVSDK_NGX_Parameter *) = nullptr;
    NVSDK_NGX_Result (NVSDK_CONV *create)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
                                        const NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **) = nullptr;
    NVSDK_NGX_Result (NVSDK_CONV *evaluate)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *,
                                          const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback_C) = nullptr;
    NVSDK_NGX_Result (NVSDK_CONV *release)(NVSDK_NGX_Handle *) = nullptr;
};

struct configuration
{
    unsigned width = 0;
    unsigned height = 0;
    unsigned guide_width = 0;
    unsigned guide_height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    LUID adapter_luid{};
    unsigned multiplier = min_multiplier;
};

struct texture_binding
{
    ID3D12Resource *resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct frame_resources
{
    frame_contract contract;
    texture_binding neural_color;
    texture_binding depth;
    texture_binding motion;
    texture_binding interpolated[max_generated_frames];
    unsigned generated_count = 1;
    bool motion_vectors_jittered = false;
    bool camera_available = false;
    texture_binding disable_interpolation;
    NVSDK_NGX_DLSSG_Opt_Eval_Params camera{};
};

class dlss_fg_stage
{
public:
    dlss_fg_stage() = default;
    dlss_fg_stage(const dlss_fg_stage &) = delete;
    dlss_fg_stage &operator=(const dlss_fg_stage &) = delete;

    NVSDK_NGX_Result create(ID3D12Device *device, ID3D12GraphicsCommandList *commands,
                            NVSDK_NGX_Parameter *capabilities, const ngx_api &api,
                            const configuration &config);
    NVSDK_NGX_Result check_capability(NVSDK_NGX_Parameter *capabilities, unsigned multiplier);
    NVSDK_NGX_Result record(ID3D12GraphicsCommandList *commands, const frame_resources &frame,
                            decision &result);
    bool track_submission(ID3D12Fence *fence, std::uint64_t value);
    bool complete(std::uint64_t frame_id, bool succeeded);
    bool cancel_recording();
    bool recording() const { return recorded_; }
    bool release_completed();
    bool live() const { return handle_ != nullptr; }
    unsigned maximum_generated_frames() const { return maximum_generated_frames_; }
    bool capability_reported() const { return capability_reported_; }
    void invalidate_history() { history_.clear(); }

private:
    ngx_api api_{};
    configuration config_{};
    NVSDK_NGX_Parameter *parameters_ = nullptr;
    NVSDK_NGX_Handle *handle_ = nullptr;
    ID3D12Fence *fence_ = nullptr;
    std::uint64_t fence_value_ = 0;
    bool recorded_ = false;
    bool submitted_ = false;
    bool evaluation_pending_ = false;
    bool evaluation_succeeded_ = false;
    frame_contract pending_contract_{};
    NVSDK_NGX_DLSSG_Opt_Eval_Params camera_{};
    frame_history history_;
    unsigned maximum_generated_frames_ = 1;
    bool capability_reported_ = false;
};
}
