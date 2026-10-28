#include "../src/async_consumer.hpp"
#include <cstdio>

static std::atomic<unsigned> polls{0};
static HANDLE entered = nullptr, release_poll = nullptr;
static std::atomic<bool> block{false};

static void poll()
{
    ++polls;
    SetEvent(entered);
    if (block.load()) WaitForSingleObject(release_poll, 1000);
}
static void wait(void *stop, void *timer)
{
    LARGE_INTEGER due{};
    due.QuadPart = -10000;
    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) return;
    const HANDLE handles[]{stop, timer};
    WaitForMultipleObjects(2, handles, FALSE, 20);
}

int main()
{
    entered = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    release_poll = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!entered || !release_poll) return 1;
    mgpu::async_consumer worker;
    if (worker.started() || worker.start(nullptr, wait) || !worker.stop()) return 2;
    if (!worker.start(poll, wait) || worker.start(poll, wait)) return 3;
    if (WaitForSingleObject(entered, 1000) != WAIT_OBJECT_0) return 4;
    const unsigned before = polls.load();
    Sleep(50);
    if (polls.load() <= before || !worker.running()) return 5;
    if (!worker.stop() || worker.started()) return 6;
    const unsigned stopped = polls.load();
    Sleep(10);
    if (polls.load() != stopped) return 7;
    block.store(true);
    ResetEvent(entered);
    if (!worker.start(poll, wait) || WaitForSingleObject(entered, 1000) != WAIT_OBJECT_0) return 8;
    if (worker.stop(0) || !worker.started()) return 9;
    SetEvent(release_poll);
    if (!worker.stop() || worker.started()) return 10;
    block.store(false);
    for (unsigned iteration = 0; iteration != 40; ++iteration)
    {
        ResetEvent(entered);
        if (!worker.start(poll, wait) || WaitForSingleObject(entered, 1000) != WAIT_OBJECT_0 || !worker.stop()) return 11;
    }
    CloseHandle(release_poll);
    CloseHandle(entered);
    std::puts("PASS: compute polling survives blocked presentation thread; cancellation, timeout ownership and 40 restart cycles; CPU only");
    return 0;
}
