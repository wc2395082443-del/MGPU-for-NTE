#include "output_trace.hpp"
#include "official_channel.hpp"
#include "output_liveness.hpp"
#include <cstdio>

int main()
{
    static_assert(!mgpu::official_fg::helper_measurements);
    using namespace mgpu::official_fg;
    unsigned failures = 0;
    const auto check = [&](bool condition, const char *name) {
        std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) ++failures;
    };
    const auto path = std::filesystem::temp_directory_path() /
        (L"mgpu-helper-no-trace-" + std::to_wstring(GetCurrentProcessId()) + L".csv");
    {
        output_trace trace(path);
        output_frame_trace frame;
        frame.sequence = 1;
        frame.sdk_reported = 6;
        frame.present_result = S_OK;
        frame.mark(output_stage::acquired);
        frame.mark(output_stage::finished);
        trace.poll();
        HANDLE trigger = OpenEventW(EVENT_MODIFY_STATE, FALSE, output_trace::trigger_name().c_str());
        HANDLE pipeline = OpenEventW(EVENT_MODIFY_STATE, FALSE, output_trace::pipeline_trigger_name().c_str());
        check(!trigger && !pipeline && !trace.pipeline_requested(), "helper exposes no measurement trigger events");
        if (trigger) CloseHandle(trigger);
        if (pipeline) CloseHandle(pipeline);
        check(!trace.record(frame) && frame.ticks[0] == 0 && frame.ticks[10] == 0,
            "frame tracing performs no timestamp reads or recording");
        check(frame.sdk_reported == 6 && frame.present_result == S_OK,
            "functional SDK count and Present result remain available to freshness protection");
    }
    check(!std::filesystem::exists(path) && !std::filesystem::exists(path.wstring() + L".pipeline.bin"),
        "helper creates no CSV or pipeline measurement file");
    source_history history;
    check(history.note(100, sleep_marker) && history.note(100, 0) && history.note(100, 1),
        "native Sleep and simulation markers retain source identity without timing samples");
    check(history.simulation_ready(100), "real simulation readiness does not depend on diagnostic timestamps");
    check(history.note(100, 2) && history.note(100, 3) && history.complete(100),
        "all native source marker phases remain valid without diagnostic timings");
    output_liveness health;
    health.complete(1, 100);
    health.complete(2, 101);
    health.complete(3, 102);
    check(health.visible(103) && !health.visible(353),
        "completed-frame freshness and stale output expiry remain functional");
    return failures ? 1 : 0;
}
