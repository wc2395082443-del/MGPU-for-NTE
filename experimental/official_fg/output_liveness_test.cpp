#include "output_liveness.hpp"
#include "output_visibility.hpp"
#include <cstdio>

int main()
{
    using namespace mgpu::official_fg;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool passed, const char *name) {
        ++checks;
        if (!passed) { ++failures; std::printf("FAIL %s\n", name); }
    };
    output_liveness output;
    check(!output.visible(1000), "no historical image may cover native loading");
    check(!output.complete(0, 1000), "zero identity cannot authorize output");
    check(output.complete(1, 1000) && !output.visible(1000), "isolated loading output remains hidden");
    check(!output.complete(1, 1100) && output.completed_ms() == 1000, "repeated presents cannot refresh completion");
    check(output.complete(2, 1030) && !output.visible(1030), "second completed output still hidden");
    check(output.complete(3, 1060) && output.visible(1060), "three advancing completions enable output");
    check(output.visible(1310) && !output.visible(1311), "stale output yields native at exact expiry boundary");
    check(!output.complete(2, 1312) && !output.visible(1312), "late old completion cannot revive output");
    check(output.complete(4, 1320) && !output.visible(1320), "first frame after transition cannot show old history");
    check(output.complete(5, 1350) && !output.visible(1350), "second recovery frame remains hidden");
    check(output.complete(6, 1380) && output.visible(1380), "complete recovery resumes without a pacing queue");
    output.invalidate();
    check(!output.visible(1381), "photo gap immediately invalidates old display");
    check(!output.complete(6, 1382) && !output.visible(1382), "same real frame never counts twice after invalidation");
    check(output.complete(7, 1410) && output.complete(8, 1440) && output.complete(9, 1470) && output.visible(1470), "complete sources recover after photo gap");
    check(!output.visible(100), "clock regression hides output");
    output_liveness sparse;
    for (unsigned sequence = 1; sequence <= 5; ++sequence)
        check(sparse.complete(sequence, sequence * 300) && !sparse.visible(sequence * 300), "sparse loading results never flash over native");
    output_liveness elapsed_without_poll;
    elapsed_without_poll.complete(1, 1000);
    elapsed_without_poll.complete(2, 1030);
    elapsed_without_poll.complete(3, 1060);
    check(elapsed_without_poll.complete(4, 2000) && !elapsed_without_poll.visible(2000), "completion detects stale period even without visibility polling");
    output_liveness fast;
    for (unsigned sequence = 1; sequence <= 1000; ++sequence) {
        check(fast.complete(sequence, sequence * 20), "normal rendering advances without sleep or retained frame group");
        if (sequence >= 3) check(fast.visible(sequence * 20), "steady output is never hidden by recovery policy");
    }
    const auto stale_routes = choose_visible_routes(true, false, 0, output.visible(10000), true);
    check(!stale_routes.helper && !stale_routes.fallback, "both stale output owners yield native game");
    const auto fallback = choose_visible_routes(true, false, 0, fast.visible(20000), false);
    check(fallback.helper && fallback.fallback, "fresh NR remains available while helper recovers");
    check(output.expires_ms() >= output.completed_ms(), "UI expiry deadline never precedes last completion");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
