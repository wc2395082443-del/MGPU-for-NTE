#include "official_channel.hpp"
#include <cstdio>

int main()
{
    using namespace mgpu::official_fg;
    source_history history;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool passed, const char *name) {
        ++checks;
        if (!passed) { ++failures; std::printf("FAIL %s\n", name); }
    };
    check(!history.complete(100), "NR receipt alone cannot invent source markers");
    check(!history.note(100, 0), "simulation cannot precede source sleep");
    check(history.note(100, sleep_marker), "real sleep accepted");
    check(!history.note(100, sleep_marker), "duplicate sleep rejected");
    check(!history.note(100, 3), "render end cannot precede start");
    check(history.note(100, 0), "simulation start");
    check(history.note(100, 2), "parallel render start allowed");
    check(history.note(100, 1), "simulation end");
    check(!history.complete(100), "render end still required");
    check(history.note(100, 3), "render end");
    check(history.complete(100), "complete actual source phases");
    check(!history.note(100, 4), "game Present must not replace helper Present");
    check(!history.note(100, 5), "game PresentEnd not relayed");
    check(!history.note(0, sleep_marker), "zero source id refused");
    check(history.note(164, sleep_marker), "new ring generation");
    check(!history.complete(100), "wrapped history cannot prove old frame");
    check(!history.complete(164), "new frame remains incomplete");
    const auto full = [](source_history &target, unsigned frame) {
        return target.note(frame, sleep_marker) && target.note(frame, 0) && target.note(frame, 1) &&
            target.note(frame, 2) && target.note(frame, 3);
    };
    source_history unretained, retained;
    for (unsigned frame = 1311; frame <= 1313; ++frame) {
        check(full(unretained, frame) && full(retained, frame), "real exported frame markers seeded");
    }
    retained.retain({1311, 1312, 1313});
    for (unsigned frame = 1314; frame <= 1500; ++frame) {
        check(full(unretained, frame) && full(retained, frame), "relay continues real future markers during cold Present");
    }
    check(!unretained.complete(1312), "negative control: old rolling-only history loses pending1312");
    check(retained.complete(1311) && retained.complete(1312) && retained.complete(1313), "pending published frames survive multiple history wraps");
    check(retained.cached_frame(1312) != 1312 && retained.retained(1312), "retention independent of rolling cache ownership");
    check(!retained.note(1312, sleep_marker) && !retained.note(1312, 3), "retention does not forward duplicate real markers");
    retained.retain({0, 1312, 1313});
    check(!retained.complete(1311) && retained.complete(1312), "only released tuple loses retention");
    retained.retain({0, 0, 0});
    check(!retained.complete(1312), "expired tuple no longer authorizes old frame");
    source_history partial;
    check(partial.note(200, sleep_marker) && partial.note(200, 0), "partial actual markers");
    partial.retain({200, 201, 0});
    for (unsigned frame = 202; frame < 400; ++frame) full(partial, frame);
    check(!partial.complete(200) && !partial.complete(201), "retention cannot fabricate missing render phases or absent source frame");
    check(partial.note(200, 1) && partial.note(200, 2) && partial.note(200, 3) && partial.complete(200), "late real stages complete retained frame");
    check(!partial.note(201, 3), "still reject render end without actual render start");
    source_history readiness;
    check(!readiness.simulation_ready(1437), "unknown source cannot authorize helper rendering");
    check(!readiness.note(1437, 1) && !readiness.simulation_ready(1437), "simulation end alone cannot authorize helper rendering");
    check(readiness.note(1437, sleep_marker) && !readiness.simulation_ready(1437), "real sleep alone remains incomplete");
    check(readiness.note(1437, 0) && !readiness.simulation_ready(1437), "real simulation start still needs simulation end");
    check(readiness.note(1437, 1) && readiness.simulation_ready(1437), "actual source simulation is sufficient for helper rendering");
    check(!readiness.complete(1437) && readiness.bits(1437) == 0x13, "source render diagnostics remain absent rather than fabricated");
    check(readiness.note(1437, 2) && readiness.bits(1437) == 0x17, "replay observed missing RenderSubmitEnd state");
    check(readiness.simulation_ready(1437) && !readiness.complete(1437), "observed 0x17 no longer blocks independent helper render submission");
    check(!readiness.simulation_ready(1438), "adjacent frame cannot borrow simulation markers");
    readiness.retain({1437, 0, 0});
    for (unsigned frame = 1438; frame <= 1565; ++frame) full(readiness, frame);
    check(readiness.simulation_ready(1437) && !readiness.complete(1437), "pending tuple retains actual simulation across rolling history wraps");
    check(readiness.note(1437, 3) && readiness.complete(1437), "late real render end still completes diagnostic history");
    readiness.retain({0, 0, 0});
    check(!readiness.simulation_ready(1437), "released old source loses simulation authorization");
    source_history concurrent_commit;
    full(concurrent_commit, 500);
    auto preview = concurrent_commit;
    check(preview.note(564, sleep_marker), "future marker validated before SDK callback");
    concurrent_commit.retain({500, 0, 0});
    check(concurrent_commit.note(564, sleep_marker) && concurrent_commit.complete(500), "post-callback note preserves newly retained frame instead of stale whole-history assignment");
    check(!preview.complete(500), "negative control: old speculative copy has overwritten pending frame");
    official_channel channel{};
    channel.manifest.generation = 7;
    std::array<std::uint32_t, shared_slots> frames{};
    check(published_source_frames(channel, frames) && frames == std::array<std::uint32_t, shared_slots>{}, "unpublished IPC cannot pin fabricated source IDs");
    for (unsigned sequence = 1; sequence <= 3; ++sequence) {
        auto &packet = channel.frames[slot_for(sequence)];
        packet.generation = 7;
        packet.sequence = sequence;
        packet.color_frame = 1310 + sequence;
    }
    channel.published = 3;
    check(published_source_frames(channel, frames) && frames == std::array<std::uint32_t, shared_slots>{1311, 1312, 1313}, "three published tuples pin exact source frame IDs");
    channel.frames[0].sequence = 4;
    channel.frames[0].color_frame = 1400;
    check(published_source_frames(channel, frames) && frames == std::array<std::uint32_t, shared_slots>{0, 1312, 1313}, "in-progress future publication never substitutes an old slot identity");
    channel.published = 4;
    check(published_source_frames(channel, frames) && frames == std::array<std::uint32_t, shared_slots>{1400, 1312, 1313}, "published slot replacement releases only its old source retention");
    channel.frames[1].generation = 8;
    check(published_source_frames(channel, frames) && frames[1] == 0, "foreign generation cannot pin source history");
    check(sizeof(official_channel) == 12488 && channel_version == 3, "main addon and helper require timing IPC v3");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
