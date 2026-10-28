#include "official_channel.hpp"
#include <cstdio>

int main()
{
    using namespace mgpu::official_fg;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool result, const char *name) {
        ++checks;
        if (!result) { ++failures; std::printf("FAIL %s\n", name); }
    };
    check(source_marker_supported(0) && relay_marker_to_sdk(0), "real simulation start relayed");
    check(source_marker_supported(1) && relay_marker_to_sdk(1), "real simulation end relayed");
    check(source_marker_supported(2) && !relay_marker_to_sdk(2), "game render start retained only as evidence");
    check(source_marker_supported(3) && !relay_marker_to_sdk(3), "game render end cannot close helper submission");
    check(source_marker_supported(input_ping_marker) && relay_marker_to_sdk(input_ping_marker), "genuine input ping forwarded");
    for (auto kind : {4u, 5u, 6u, 7u, 9u, 20u, 100u, 0xffffffffu})
        check(!source_marker_supported(kind) && !relay_marker_to_sdk(kind), "no foreign Present or fabricated source markers");
    source_history history;
    const marker_timing original{10, 20, 30, 45, 80};
    check(!history.record_timing(11, sleep_marker, original), "timing alone cannot create source history");
    check(history.note(11, sleep_marker) && history.record_timing(11, sleep_marker, original), "actual sleep timing recorded");
    check(history.note(11, 0) && history.record_timing(11, 0, {90, 91, 92, 98, 99}), "source simulation and relay clocks distinct");
    check(!history.record_timing(11, input_ping_marker, original), "input ping cannot fabricate required phase");
    check(!history.complete(11), "timings do not bypass missing render phases");
    history.retain({11, 0, 0});
    for (unsigned frame = 12; frame < 180; ++frame) history.note(frame, sleep_marker);
    const auto retained = history.timing(11);
    check(retained.phases[0].native_begin_ns == 10 && retained.phases[0].dispatch_end_ns == 80,
        "timings retained across cold-start rolling-history wrap");
    check(retained.phases[1].native_begin_ns == 90 && retained.phases[1].dispatch_begin_ns == 98,
        "original and replay timestamps not collapsed");
    history.retain({0, 0, 0});
    check(history.timing(11).phases[0].native_begin_ns == 0, "expired frame cannot inherit another frame timing");
    check(history.timing(0).phases[0].native_begin_ns == 0, "zero frame remains empty");
    check(channel_version == 3 && sizeof(source_marker) == 40, "IPC version covers extended timing payload");
    std::printf("%u Reflex contract checks, %u failures; channel bytes=%zu\n", checks, failures, sizeof(official_channel));
    return failures ? 1 : 0;
}
