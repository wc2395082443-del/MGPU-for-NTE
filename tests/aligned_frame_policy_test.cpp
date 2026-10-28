#include "../src/aligned_frame_policy.hpp"
#include <cstdio>

int main()
{
    using namespace mgpu::aligned;
    unsigned failures = 0;
    const auto check = [&](bool passed, const char *name)
    {
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    std::array<std::uint64_t, 3> timelines{};
    const auto completed = [&](unsigned queue) { return timelines.at(queue); };
    frame_state frame{};
    check(frame.reclaimable(completed) && !frame.ready(completed), "empty slot has no publishable frame");
    frame.serial = 1;
    frame.key = {0, 1};
    frame.commands = {11, 12, 13};
    check(frame.recorded(), "complete recording establishes order before GPU completion");
    check(!frame.schedulable(), "recording without actual submission tickets cannot schedule GPU waits");
    check(!frame.ready(completed) && !frame.abandoned_complete(completed), "recording is not GPU completion");
    frame.submitted(11, {0, 4});
    frame.submitted(12, {1, 8});
    check(!frame.schedulable(), "two tickets cannot authorize a three-plane tuple");
    frame.submitted(13, {2, 2});
    check(frame.schedulable() && !frame.ready(completed), "all submission tickets allow GPU handoff before completion");
    check(!frame.ready(completed), "submitted commands must still complete");
    timelines = {400, 7, 200};
    check(!frame.ready(completed), "another queue's high fence cannot retire depth");
    timelines[1] = 8;
    check(frame.ready(completed), "all three independent queues complete");
    timelines[1] = UINT64_MAX;
    check(!frame.ready(completed), "device removal is not completion");
    timelines[1] = 8;
    frame.leased = true;
    check(!frame.schedulable(), "leased tuple cannot be scheduled again");
    check(!frame.recorded(), "leased frame is not eligible for another acquisition");
    check(!frame.ready(completed) && !frame.reclaimable(completed) && !frame.abandoned_complete(completed),
          "a leased frame cannot be delivered twice or overwritten");
    frame.release_command = 14;
    frame.leased = false;
    check(!frame.reclaimable(completed), "CPU transfer recording does not retire staging");
    frame.submitted(14, {0, 401});
    check(!frame.reclaimable(completed), "submitted transfer must finish reading staging");
    timelines[0] = 401;
    check(frame.reclaimable(completed), "staging may be reused after outgoing read completes");
    frame_state partial{};
    partial.serial = 2;
    partial.commands[1] = 42;
    check(!partial.recorded(), "partial frame cannot obstruct a complete tuple");
    check(!partial.abandoned_complete(completed), "partial unsent copies cannot be discarded");
    partial.submitted(42, {1, 8});
    check(partial.abandoned_complete(completed) && !partial.ready(completed), "completed partial frame is disposable, not consumable");
    partial.commands[0] = 43;
    check(partial.cancel(43) && partial.commands[0] == 0, "reset cancels unsent command captures");
    check(partial.cancel(42) && partial.commands[1] == 42, "reset does not erase a submitted copy");
    partial.release_command = 44;
    check(!partial.cancel(44), "cancelled outgoing transfer is a fail-closed error");
    check(!(frame_key{1, 1} == frame_key{1, 2}), "same frame in another viewport never matches");
    check(!(frame_key{1, 1} == frame_key{2, 1}), "different engine frames never match");
    check(newer_frame(0, UINT32_MAX) && consecutive_frame(0, UINT32_MAX), "frame wrap is contiguous");
    check(!newer_frame(10, 10) && !newer_frame(9, 10), "duplicate and stale frames rejected");
    check(newer_frame(12, 10) && !consecutive_frame(12, 10), "real gap requires history reset, not motion multiplication");
    return failures == 0 ? 0 : 1;
}
