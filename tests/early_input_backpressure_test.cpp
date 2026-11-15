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
    check(empty.ready && !empty.waited && empty.pending == 1, "NR caught up: frame starts without waiting and immediately reserves one credit");
    gate.wait_submitted(100);
    const auto overlap = gate.wait_before_input(101);
    check(!overlap.waited && overlap.pending == 2 && overlap.completed == 99,
        "one in-flight source remains allowed while the next input frame starts");
    gate.wait_submitted(101);
    check(gate.wait_before_input(102).pending == 3,
        "third lifecycle credit allows the input frame needed to advance a two-frame-lagged render thread");

    std::atomic<bool> entered{false}, finished{false};
    std::atomic<unsigned> input_value{7}, sampled_input{0};
    source_admission_result result;
    std::thread producer([&] {
        entered.store(true);
        result = gate.wait_before_input(103);
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
    check(result.waited && !result.timed_out && result.target == 100 && result.completed == 100 && result.pending == 3,
        "early wait depends only on an already-submitted oldest frame");
    check(sampled_input.load() == 8, "input changing during the wait is sampled fresh after capacity returns");
    const auto submitted = gate.wait_submitted(103);
    check(!submitted.waited && submitted.pending == 3 && submitted.completed == 100,
        "same source can submit while its own and preceding NR remain unfinished");
    check(!gate.wait_before_input(103).waited, "duplicate Sleep for one token does not add another front wait");
    gate.complete(103);
    bool fast_wait = false;
    for (unsigned frame = 104; frame < 120; ++frame) {
        const auto front = gate.wait_before_input(frame);
        fast_wait = fast_wait || front.waited || front.pending != 1;
        gate.complete(frame);
        const auto back = gate.wait_submitted(frame);
        fast_wait = fast_wait || back.waited;
    }
    check(!fast_wait, "sustained faster NR keeps both input and submission paths nonblocking");

    gate.reset(true);
    gate.complete(200);
    gate.wait_submitted(201);
    gate.wait_submitted(202);
    gate.wait_before_input(203);
    finished.store(false);
    std::thread stopped([&] {
        result = gate.wait_before_input(204);
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
    gate.wait_before_input(303);
    const auto timeout = gate.wait_before_input(304, std::chrono::milliseconds(20));
    check(timeout.timed_out && !gate.wait_submitted(304).ready,
        "a stuck early wait suspends admission through the bounded failure path");
    gate.complete(299);
    check(!gate.wait_before_input(305).ready,
        "stale completion cannot rearm a full suspended gate");
    gate.complete(301);
    const auto recovered = gate.wait_before_input(306);
    check(recovered.ready && !recovered.waited && recovered.pending == 3,
        "real NR completion releases an owned credit and rearms a suspended gate");

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
    check(!failed && consecutive && front_waits + back_waits > 0 && peak <= 3,
        "sustained slow NR preserves all 64 frames within three lifecycle credits");
    check(overlap_count > 0 && returned_before_own_nr > 0,
        "early input pressure still allows source work concurrent with earlier NR");
    gate.reset(true);
    gate.complete(400);
    const auto reserved_first = gate.wait_before_input(401);
    const auto reserved_second = gate.wait_before_input(402);
    const auto reserved_third = gate.wait_before_input(403);
    check(reserved_first.pending == 1 && reserved_second.pending == 2 &&
        !reserved_first.waited && !reserved_second.waited,
        "two admitted but not yet submitted input frames consume two distinct credits");
    check(reserved_third.pending == 3 && !reserved_third.waited,
        "third admitted input can advance the engine render pipeline without increasing the three-credit budget");
    check(gate.wait_before_input(403).pending == 3,
        "duplicate input token does not reserve an additional credit");
    finished.store(false);
    std::thread delayed_submission([&] {
        result = gate.wait_before_input(404, std::chrono::milliseconds(500));
        finished.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(!finished.load(), "fourth input cannot bypass credits while render submissions are delayed");
    const auto confirmed_first = gate.wait_submitted(401);
    const auto confirmed_second = gate.wait_submitted(402);
    check(!confirmed_first.waited && !confirmed_second.waited &&
        confirmed_first.pending == 3 && confirmed_second.pending == 3,
        "submission confirms each reserved credit without double counting or waiting for its own NR");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(!finished.load(), "confirming submission alone does not release a credit before NR completion");
    gate.complete(401);
    delayed_submission.join();
    check(result.waited && !result.timed_out && result.pending == 3 && result.completed == 401,
        "oldest real NR completion releases exactly one reservation for the next input");
    check(gate.wait_submitted(404).pending == 3 && !gate.wait_submitted(404).waited,
        "newly admitted frame can submit while previous NR remains unfinished");
    gate.reset(true);
    gate.complete(0xfffffffeu);
    check(gate.wait_before_input(0xffffffffu).pending == 1 && gate.wait_before_input(0).pending == 2,
        "reserved input credits survive source ID wrap");
    gate.complete(0xffffffffu);
    check(gate.wait_before_input(1).pending == 2, "wrapped completion retires only its corresponding reservation");
    gate.reset(true);
    gate.complete(500);
    bool delayed_engine_failed = false;
    unsigned delayed_engine_peak = 0, delayed_engine_submitted = 0;
    for (unsigned frame = 501; frame <= 564; ++frame) {
        const auto admitted = gate.wait_before_input(frame, std::chrono::milliseconds(20));
        delayed_engine_failed = delayed_engine_failed || admitted.timed_out || admitted.cancelled || !admitted.ready;
        if (admitted.pending > delayed_engine_peak) delayed_engine_peak = admitted.pending;
        if (frame >= 503) {
            const auto render_frame = frame - 2;
            const auto confirmed = gate.wait_submitted(render_frame, std::chrono::milliseconds(20));
            delayed_engine_failed = delayed_engine_failed || confirmed.timed_out || confirmed.cancelled || !confirmed.ready;
            gate.complete(render_frame);
            ++delayed_engine_submitted;
        }
    }
    check(!delayed_engine_failed && delayed_engine_peak == 3 && delayed_engine_submitted == 62,
        "two-frame-lagged engine can admit 64 simulations and submit 62 real render frames without a cyclic input wait");
    gate.reset(false);
    gate.complete(564);
    check(!gate.wait_before_input(565).ready,
        "explicit route shutdown remains disabled even if late completions arrive");
    return failures == 0 ? 0 : 1;
}
