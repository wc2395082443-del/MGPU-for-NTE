#include "official_metrics.hpp"
#include <atomic>
#include <cstdio>
#include <limits>
#include <thread>

int main()
{
    using namespace mgpu::official_fg;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool condition, const char *name) {
        ++checks;
        if (!condition) { ++failures; std::printf("FAIL %s\n", name); }
    };
    const auto equal = [](double left, double right) { return std::abs(left - right) < 0.001; };
    rate_meter meter;
    rate_snapshot sample{};
    check(!meter.update(1000, 100, 600, true, sample), "first counter snapshot is not an FPS result");
    meter.source_age(40);
    meter.source_age(50);
    meter.source_age(-1);
    meter.source_age(std::numeric_limits<double>::infinity());
    check(!meter.update(1999, 130, 780, true, sample), "minimum one-second interval");
    check(meter.update(2000, 131, 786, true, sample), "measured window closes");
    check(sample.valid && sample.visible, "foreground sample valid");
    check(equal(sample.real_fps, 31) && equal(sample.sdk_fps, 186) && equal(sample.multiplier, 6), "rates derive from counters not requested factor");
    check(equal(sample.source_age_ms, 45), "source age mean excludes unknown/nonfinite values");
    metric_block current{}, visible{};
    publish_metrics(current, sample);
    publish_metrics(visible, sample);
    rate_snapshot decoded{};
    check(read_metrics(current, decoded) && equal(decoded.sdk_fps, 186), "atomic record roundtrip");
    check(!meter.update(2100, 134, 804, false, sample), "Home visibility change starts a new window");
    meter.source_age(55);
    check(meter.update(3100, 165, 835, false, sample) && sample.valid && !sample.visible, "hidden output measured separately");
    check(equal(sample.multiplier, 1), "hidden onefold is not mislabeled sixfold");
    publish_metrics(current, sample);
    check(read_metrics(visible, decoded) && decoded.visible && equal(decoded.multiplier, 6), "opening Home retains last visible window");
    check(fresh_metrics(decoded, 3200, 2500), "recent sample accepted");
    check(!fresh_metrics(decoded, 33001, 30000), "old gameplay sample expires");
    check(!fresh_metrics(decoded, 1999, 30000), "future timestamp refused");
    check(meter.update(4100, 165, 835, false, sample) && !sample.valid, "zero new real frames invalidates rates");
    check(!meter.update(4200, 0, 0, false, sample), "counter reset does not underflow into huge FPS");
    check(!meter.update(4300, 3, 18, true, sample), "foreground reset excludes hidden history");
    check(meter.update(10400, 35, 210, true, sample) && !sample.valid, "suspended window rejected");
    rate_meter partial;
    check(!partial.update(1000, 0, 0, true, sample), "partial-rate baseline");
    check(partial.update(2000, 30, 165, true, sample) && equal(sample.multiplier, 5.5), "effective multiplier is not rounded to requested sixfold");
    metric_block empty;
    check(!read_metrics(empty, decoded), "unpublished telemetry unavailable");
    empty.sequence = 1;
    check(!read_metrics(empty, decoded), "in-progress write never exposed");
    metric_block concurrent;
    std::atomic<bool> finished{false};
    std::atomic<unsigned> inconsistent{0};
    std::thread writer([&] {
        for (unsigned index = 1; index <= 20000; ++index) {
            rate_snapshot entry{};
            entry.begin_ms = index * 1000;
            entry.end_ms = entry.begin_ms + 1000;
            entry.real_delta = index;
            entry.sdk_delta = index * 6;
            entry.real_fps = index;
            entry.sdk_fps = index * 6;
            entry.multiplier = 6;
            entry.source_age_ms = 49;
            entry.valid = entry.visible = true;
            publish_metrics(concurrent, entry);
        }
        finished.store(true);
    });
    while (!finished.load()) {
        rate_snapshot value{};
        if (read_metrics(concurrent, value) &&
            (value.sdk_delta != value.real_delta * 6 || !equal(value.sdk_fps, value.real_fps * 6) ||
             value.end_ms - value.begin_ms != 1000 || !equal(value.source_age_ms, 49))) ++inconsistent;
    }
    writer.join();
    check(inconsistent.load() == 0, "concurrent telemetry cannot mix two frames or rates");
    check(read_metrics(concurrent, decoded) && decoded.real_delta == 20000, "final telemetry publication visible");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
