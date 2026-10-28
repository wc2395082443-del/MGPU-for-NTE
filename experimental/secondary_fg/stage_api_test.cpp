#include "dlss_fg_stage.hpp"

#include <cstdio>

extern "C" int test_parameter_bridge();

int main()
{
    if (test_parameter_bridge() != 0) return 5;
    mgpu::secondary_fg::dlss_fg_stage stage;
    mgpu::secondary_fg::ngx_api api;
    mgpu::secondary_fg::configuration config;
    mgpu::secondary_fg::frame_resources frame;
    mgpu::secondary_fg::decision result;
    if (stage.live() || !stage.release_completed()) return 1;
    if (stage.create(nullptr, nullptr, nullptr, api, config) != NVSDK_NGX_Result_FAIL_InvalidParameter)
        return 2;
    if (stage.record(nullptr, frame, result) != NVSDK_NGX_Result_FAIL_InvalidParameter) return 3;
    if (stage.track_submission(nullptr, 1) || stage.complete(1, true)) return 4;
    if (stage.recording() || !stage.cancel_recording() || !stage.release_completed()) return 6;
    std::puts("PASS: seven typed MSVC-ABI parameter bridge calls and stage invalid-call guards; no GPU or DLL loaded");
}
