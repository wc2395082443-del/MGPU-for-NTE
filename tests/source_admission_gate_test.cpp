#ifndef MGPU_SOURCE_ADMISSION_HEADER
#define MGPU_SOURCE_ADMISSION_HEADER "../experimental/official_fg/source_admission_gate.hpp"
#endif
#include MGPU_SOURCE_ADMISSION_HEADER
#include "../src/aligned_frame_policy.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

int main()
{
    using namespace mgpu;
    unsigned failures = 0;
    const auto check = [&](bool condition, const char *description) {
        std::printf("%s: %s\n", condition ? "PASS" : "FAIL", description);
        failures += condition ? 0u : 1u;
    };
    aligned::frame_state frame;
    frame.serial = 1;
    frame.key = {100, 1};
    frame.commands = {10, 20, 30};
    aligned::guide_constants constants;
    constants.key = frame.key;
    constants.valid = true;
    const std::array<std::uint64_t, 3> serials{30, 0, 0};
    check(!aligned::submitted_source_target(frame, constants, serials, 1),
        "recorded but unsubmitted commands cannot become a wait target");
    frame.submitted(10, {0, 1});
    frame.submitted(20, {1, 2});
    check(!aligned::submitted_source_target(frame, constants, serials, 1),
        "color and depth alone cannot authorize a wait for missing motion");
    frame.submitted(30, {2, 3});
    check(aligned::submitted_source_target(frame, constants, serials, 1),
        "all source tickets and matching constants authorize the exact submitted frame");
    auto wrong_constants = constants;
    wrong_constants.key.viewport = 2;
    check(!aligned::submitted_source_target(frame, wrong_constants, serials, 1),
        "another viewport cannot authorize the tuple");
    wrong_constants = constants;
    wrong_constants.key.frame = 102;
    check(!aligned::submitted_source_target(frame, wrong_constants, serials, 1),
        "CPU constants two frames ahead cannot replace target frame metadata");
    check(!aligned::submitted_source_target(frame, constants, serials, 0),
        "unrelated submission cannot repeat an old tuple target");
    frame.leased = true;
    check(!aligned::submitted_source_target(frame, constants, serials, 1),
        "leased resources cannot authorize a second producer wait");

    official_fg::source_admission_gate gate;
    check(!gate.wait_submitted(100).ready, "disabled route never waits");
    gate.reset(true);
    check(!gate.wait_submitted(100).ready, "startup remains unblocked until real NR completion");
    gate.complete(99);
    const auto first = gate.wait_submitted(100);
    const auto second = gate.wait_submitted(101);
    check(first.ready && !first.waited && first.completed == 99 && first.pending == 1,
        "source N returns before NR N completes when pipeline has a credit");
    check(!second.waited && second.completed == 99 && second.pending == 2,
        "source N+1 continues while NR N is unfinished");
    std::atomic<bool> entered{false}, finished{false};
    official_fg::source_admission_result waited;
    std::thread waiter([&] {
        entered.store(true, std::memory_order_release);
        waited = gate.wait_submitted(102);
        finished.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(!finished.load(std::memory_order_acquire), "only the full three-slot pipeline applies backpressure");
    gate.complete(98);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(!finished.load(std::memory_order_acquire), "stale NR acknowledgement cannot return a pipeline credit");
    gate.complete(100);
    waiter.join();
    check(waited.waited && waited.target == 100 && waited.completed == 100 && waited.pending == 2 && !waited.timed_out,
        "oldest NR completion releases the producer while newer NR frames remain in flight");
    check(!gate.wait_submitted(102).waited && gate.wait_submitted(102).pending == 2,
        "duplicate submitted tuple cannot consume another credit");

    finished.store(false);
    std::thread cancelled([&] {
        waited = gate.wait_submitted(103);
        finished.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    gate.reset(false);
    cancelled.join();
    check(finished.load() && waited.cancelled && !waited.timed_out, "shutdown cancels the pending wait");

    gate.reset(true);
    gate.complete(100);
    gate.wait_submitted(101);
    gate.wait_submitted(102);
    const auto timeout = gate.wait_submitted(103, std::chrono::milliseconds(25));
    check(timeout.timed_out && timeout.wait_ms >= 20 && !gate.wait_submitted(104).ready,
        "consumer stall is bounded and explicitly disables the failed gate");
    gate.reset(true);
    gate.complete(0xfffffffeu);
    gate.wait_submitted(0xffffffffu);
    gate.wait_submitted(0);
    const auto wrap_timeout = gate.wait_submitted(1, std::chrono::milliseconds(10));
    check(wrap_timeout.timed_out && wrap_timeout.target == 0xffffffffu,
        "wrapped source IDs retain FIFO credit ownership");
    gate.reset(true);
    gate.complete(0);
    check(gate.wait_submitted(0).ready && !gate.wait_submitted(0).waited,
        "explicit completion of wrapped zero is valid");

    gate.reset(true);
    gate.complete(90);
    gate.wait_submitted(100);
    gate.wait_submitted(110);
    const auto gap_timeout = gate.wait_submitted(120, std::chrono::milliseconds(10));
    check(gap_timeout.timed_out && gap_timeout.target == 100,
        "credits count actual frames rather than subtracting numeric source IDs");

    gate.reset(true);
    gate.complete(0);
    std::mutex queue_lock;
    std::condition_variable queue_wake;
    unsigned published = 0, completed = 0, peak_pending = 0;
    bool stop = false;
    std::vector<unsigned> retired;
    std::atomic<bool> invalid_return{false}, nr_busy{false};
    unsigned overlapping_source_work = 0, returned_before_own_nr = 0;
    std::thread nr([&] {
        std::unique_lock<std::mutex> lock(queue_lock);
        queue_wake.wait(lock, [&] { return stop || published >= official_fg::source_admission_gate::capacity; });
        while (!stop || completed < published) {
            queue_wake.wait(lock, [&] { return stop || completed < published; });
            if (completed == published) continue;
            const auto next = completed + 1;
            nr_busy.store(true);
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            lock.lock();
            completed = next;
            nr_busy.store(false);
            retired.push_back(next);
            lock.unlock();
            gate.complete(next);
            lock.lock();
        }
    });
    for (unsigned target = 1; target <= 64; ++target) {
        if (nr_busy.load()) ++overlapping_source_work;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        {
            std::lock_guard<std::mutex> lock(queue_lock);
            published = target;
            const auto pending = published - completed;
            if (pending > peak_pending) peak_pending = pending;
        }
        queue_wake.notify_one();
        const auto result = gate.wait_submitted(target);
        std::lock_guard<std::mutex> lock(queue_lock);
        if (result.timed_out || result.cancelled || published - completed >= official_fg::source_admission_gate::capacity)
            invalid_return.store(true);
        if (completed < target) ++returned_before_own_nr;
    }
    {
        std::lock_guard<std::mutex> lock(queue_lock);
        stop = true;
    }
    queue_wake.notify_one();
    nr.join();
    bool consecutive = retired.size() == 64;
    for (unsigned index = 0; index < retired.size(); ++index)
        consecutive = consecutive && retired[index] == index + 1;
    std::printf("pipeline: peak=%u returned_early=%u overlaps=%u retired=%zu invalid=%u consecutive=%u\n",
        peak_pending, returned_before_own_nr, overlapping_source_work, retired.size(),
        unsigned(invalid_return.load()), unsigned(consecutive));
    check(!invalid_return.load() && peak_pending == 3 && consecutive,
        "fast producer and slower NR retain all 64 source frames within three pipeline slots");
    check(overlapping_source_work > 0 && returned_before_own_nr >= 2,
        "source work overlaps active NR and producer does not wait for every frame's own NR");
    return failures == 0 ? 0 : 1;
}
