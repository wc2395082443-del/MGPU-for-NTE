#include "../src/frame_measurement.hpp"
#include "../src/queue_dependency_trace.hpp"
#include "stage2_sfdiag.hpp"
#include <cstdio>
#include <string>
#include <thread>

int main()
{
    static_assert(!mgpu::gpu_measurement::cpu_statistics);
    unsigned failures = 0;
    const auto check = [&](bool condition, const char *name) {
        std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) ++failures;
    };
    mgpu::measurement::poll();
    check(!mgpu::measurement::session() && !mgpu::measurement::continuous(),
        "main detailed measurement cannot activate or open trigger events");
    mgpu::queue_trace::observe_queue(reinterpret_cast<ID3D12CommandQueue *>(1));
    mgpu::queue_trace::observe_fence(reinterpret_cast<ID3D12Fence *>(1));
    mgpu::queue_trace::poll(1, true);
    check(!mgpu::queue_trace::active() && !mgpu::queue_trace::export_path()[0],
        "queue observer cannot patch driver methods or create trace files");
    check(mgpu::timing::cpu_ticks() > 0, "generic CPU source clock is still real");
    std::mutex mutex;
    {
        mgpu::measurement::scoped_lock scope(mutex, "source tuple");
        bool blocked = false;
        std::thread other([&] {
            const bool acquired = mutex.try_lock();
            blocked = !acquired;
            if (acquired) mutex.unlock();
        });
        other.join();
        check(blocked, "removing lock timing retains exclusive source resource ownership");
    }
    const bool acquired = mutex.try_lock();
    check(acquired, "source resource mutex is released after timed-wrapper replacement");
    if (acquired) mutex.unlock();
    const auto hold_before = sfdiag::hold_n[sfdiag::SITE_GAME].load();
    {
        sfdiag::hold_timer scope(sfdiag::SITE_GAME);
        sfdiag::bucket(0, 10);
    }
    check(sfdiag::qpc() == 0 && sfdiag::hold_n[sfdiag::SITE_GAME].load() == hold_before && sfdiag::b_n[0].load() == 0,
        "actual extracted game/bridge lock statistics perform no clock read or atomic accumulation");
    HANDLE control = OpenEventW(EVENT_MODIFY_STATE, FALSE,
        (L"Local\\MGPUFrameTiming." + std::to_wstring(GetCurrentProcessId())).c_str());
    check(control == nullptr, "measurement poll creates no process timing trigger");
    if (control) CloseHandle(control);
    return failures ? 1 : 0;
}
