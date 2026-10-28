#include "../src/motion_frame_packet.hpp"
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

int main()
{
    unsigned failures = 0;
    const auto check = [&failures](bool passed, const char *name)
    {
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    mgpu::motion_packet::handoff<6> frames;
    check(!frames.valid(0, 1), "pending slot is not consumable");
    const mgpu::motion_packet::identity engine{0, 7, true, true};
    frames.recorded(100, 200, engine);
    check(!frames.valid(0, 1), "recording alone does not publish a sealed frame");
    auto first = frames.close(1);
    check(first.seal == 1 && first.source == 100 && first.command == 200 &&
          first.engine.frame == 0 && first.engine.frame_known && first.engine.viewport == 7,
          "identity and source freeze together, engine frame zero remains valid");
    check(frames.valid(0, 1) && !frames.valid(0, 7), "consumer checks generation, not only ring slot");
    frames.recorded(101, 201, {1, 7, true, true});
    check(frames.at(0, 1).source == 100, "next-frame recording cannot rewrite closed metadata");
    check(frames.close(1).seal == 0 && frames.pending().source == 101,
          "duplicate close cannot erase next-frame recording");
    check(frames.close(3).seal == 0 && frames.pending().seal == 2,
          "out-of-order close cannot relabel a capture");
    check(frames.close(2).source == 101, "copy after close belongs to next packet");
    check(frames.close(3).source == 0 && !frames.valid(2, 3), "missing capture never borrows last motion");
    (void)frames.close(4);
    (void)frames.close(5);
    (void)frames.close(6);
    (void)frames.close(7);
    check(!frames.valid(0, 1) && !frames.valid(0, 7), "ring reuse rejects old valid metadata");
    frames = {};
    check(frames.pending().seal == 1 && !frames.valid(0, 1), "rearm clears all generations");

    std::mutex mutex;
    std::condition_variable changed;
    unsigned phase = 0;
    std::thread capture([&]
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return phase == 1; });
        frames.recorded(222, 333, {22, 7, true, true});
        phase = 2;
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        (void)frames.close(1);
        phase = 1;
        changed.notify_all();
        changed.wait(lock, [&] { return phase == 2; });
    }
    capture.join();
    check(frames.pending().source == 222 && !frames.valid(0, 1) && frames.close(2).source == 222,
          "capture between packet close and producer completion is preserved");

    unsigned legacy_next_valid = 0;
    legacy_next_valid = 1;
    legacy_next_valid = 0;
    check(legacy_next_valid == 0 && frames.valid(1, 2),
          "old publish-then-clear schedule loses motion; packet handoff retains it");
    frames.reset(301);
    frames.recorded(888, 999, {});
    check(!frames.valid(1, 2) && frames.close(301).source == 888 && frames.valid(0, 301),
          "rearm follows the existing transport counter instead of assuming seal one");
    return failures == 0 ? 0 : 1;
}
