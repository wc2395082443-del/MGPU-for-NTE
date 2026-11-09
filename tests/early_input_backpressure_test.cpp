#include "../experimental/official_fg/source_admission_gate.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

int main()
{
    using mgpu::official_fg::source_admission_gate;
    using mgpu::official_fg::source_admission_result;
    unsigned failures = 0;
    const auto check = [&](bool condition, const char *name) {
        std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) ++failures;
    };
    source_admission_gate gate;
    check(!gate.wait_before_input(10).ready, "disabled input path never adds a wait");
    gate.reset(true);
    check(!gate.wait_before_input(10).ready, "initialization never waits for a first NR job that does not exist");
    gate.complete(99);
    const auto empty = gate.wait_before_input(100);
    check(empty.ready && !empty.waited && empty.pending == 0, "NR caught up: frame start has zero added waiting");
    gate.wait_submitted(100);
    const auto overlap = gate.wait_before_input(101);
    check(!overlap.waited && overlap.pending == 1 && overlap.completed == 99,
        "one in-flight source remains allowed while the next input frame starts");
    gate.wait_submitted(101);

    std::atomic<bool> entered{false}, finished{false};
    std::atomic<unsigned> input_value{7}, sampled_input{0};
    source_admission_result result;
    std::thread producer([&] {
        entered.store(true);
        result = gate.wait_before_input(102);
        sampled_input.store(input_value.load());
        finished.store(true);
    });
    while (!entered.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(!finished.load() && sampled_input.load() == 0,
        "capacity pressure is waited before new input is sampled");
    input_value.store(8);
    gate.complete(100);
    producer.join();
    check(result.waited && !result.timed_out && result.target == 100 && result.completed == 100 && result.pending == 1,
        "early wait depends only on an already-submitted oldest frame");
    check(sampled_input.load() == 8, "input changing during the wait is sampled fresh after capacity returns");
    const auto submitted = gate.wait_submitted(102);
    check(!submitted.waited && submitted.pending == 2 && submitted.completed == 100,
        "same source can submit while its own and preceding NR remain unfinished");
    check(!gate.wait_before_input(102).waited, "duplicate Sleep for one token does not add another front wait");
    gate.complete(102);
    bool fast_wait = false;
    for (unsigned frame = 103; frame < 120; ++frame) {
        const auto front = gate.wait_before_input(frame);
        fast_wait = fast_wait || front.waited || front.pending != 0;
        gate.complete(frame);
        const auto back = gate.wait_submitted(frame);
        fast_wait = fast_wait || back.waited;
    }
    check(!fast_wait, "sustained faster NR keeps both input and submission paths nonblocking");

    gate.reset(true);
    gate.complete(200);
    gate.wait_submitted(201);
    gate.wait_submitted(202);
    finished.store(false);
    std::thread stopped([&] {
        result = gate.wait_before_input(203);
        finished.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    gate.reset(false);
    stopped.join();
    check(finished.load() && result.cancelled && !result.timed_out,
        "shutdown interrupts an early input wait without waiting for GPU or game submission");

    gate.reset(true);
    gate.complete(300);
    gate.wait_submitted(301);
    gate.wait_submitted(302);
    const auto timeout = gate.wait_before_input(303, std::chrono::milliseconds(20));
    check(timeout.timed_out && !gate.wait_submitted(303).ready,
        "a stuck early wait has the existing explicit bounded failure path");

    gate.reset(true);
    gate.complete(0);
    std::mutex queue_lock;
    std::condition_variable queue_wake;
    unsigned published = 0, completed = 0, peak = 0;
    bool stop = false;
    std::atomic<bool> nr_busy{false};
    std::vector<unsigned> retired;
    unsigned front_waits = 0, back_waits = 0, overlap_count = 0, returned_before_own_nr = 0;
    bool failed = false;
    std::thread nr([&] {
        std::unique_lock<std::mutex> lock(queue_lock);
        queue_wake.wait(lock, [&] { return stop || published >= 2; });
        while (!stop || completed < published) {
            queue_wake.wait(lock, [&] { return stop || completed < published; });
            if (completed == published) continue;
            const auto next = completed + 1;
            nr_busy.store(true);
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            lock.lock();
            completed = next;
            retired.push_back(next);
            nr_busy.store(false);
            lock.unlock();
            gate.complete(next);
            lock.lock();
        }
    });
    for (unsigned frame = 1; frame <= 64; ++frame) {
        const auto front = gate.wait_before_input(frame);
        front_waits += front.waited ? 1u : 0u;
        failed = failed || front.timed_out || front.cancelled;
        overlap_count += nr_busy.load() ? 1u : 0u;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        {
            std::lock_guard<std::mutex> lock(queue_lock);
            published = frame;
            if (published - completed > peak) peak = published - completed;
        }
        queue_wake.notify_one();
        const auto back = gate.wait_submitted(frame);
        back_waits += back.waited ? 1u : 0u;
        failed = failed || back.timed_out || back.cancelled;
        std::lock_guard<std::mutex> lock(queue_lock);
        returned_before_own_nr += completed < frame ? 1u : 0u;
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
    std::printf("early pipeline: peak=%u front_waits=%u back_waits=%u overlap=%u returned_before_own_nr=%u\n",
        peak, front_waits, back_waits, overlap_count, returned_before_own_nr);
    check(!failed && consecutive && front_waits > 0 && back_waits == 0 && peak == 2,
        "sustained slow NR preserves all 64 frames while moving capacity waits before input rather than after submission");
    check(overlap_count > 0 && returned_before_own_nr > 0,
        "early input pressure still allows source work concurrent with earlier NR");
    return failures == 0 ? 0 : 1;
}
