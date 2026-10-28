#pragma once

#include <windows.h>
#include <process.h>
#include <atomic>
#include <new>

namespace mgpu
{
class async_consumer
{
public:
    using poll_fn = void (*)();
    using wait_fn = void (*)(HANDLE, HANDLE);
    async_consumer() = default;
    async_consumer(const async_consumer &) = delete;
    async_consumer &operator=(const async_consumer &) = delete;
    ~async_consumer() { stop(); }

    bool start(poll_fn poll, wait_fn wait)
    {
        if (state_ != nullptr || poll == nullptr || wait == nullptr) return false;
        auto *created = new (std::nothrow) state;
        if (created == nullptr) return false;
        created->poll = poll;
        created->wait = wait;
        created->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        created->wake_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        created->high_resolution = created->wake_timer != nullptr;
        if (created->wake_timer == nullptr) created->wake_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        HMODULE pinned = nullptr;
        if (created->stop == nullptr || created->wake_timer == nullptr ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&entry), &pinned))
        {
            destroy(created);
            return false;
        }
        created->thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, entry, created, 0, nullptr));
        if (created->thread == nullptr)
        {
            destroy(created);
            return false;
        }
        state_ = created;
        return true;
    }
    bool running() const
    {
        return state_ != nullptr && WaitForSingleObject(state_->thread, 0) == WAIT_TIMEOUT;
    }
    bool started() const { return state_ != nullptr; }
    bool failed() const { return state_ != nullptr && state_->failed.load(); }
    bool high_resolution() const { return state_ != nullptr && state_->high_resolution; }
    bool stop(unsigned timeout_ms = 2000)
    {
        if (state_ == nullptr) return true;
        SetEvent(state_->stop);
        if (WaitForSingleObject(state_->thread, timeout_ms) != WAIT_OBJECT_0) return false;
        destroy(state_);
        state_ = nullptr;
        return true;
    }
private:
    struct state
    {
        HANDLE thread = nullptr, stop = nullptr, wake_timer = nullptr;
        bool high_resolution = false;
        poll_fn poll = nullptr;
        wait_fn wait = nullptr;
        std::atomic<bool> failed{false};
    };
    state *state_ = nullptr;
    static unsigned __stdcall entry(void *argument)
    {
        auto &task = *static_cast<state *>(argument);
        try
        {
            while (WaitForSingleObject(task.stop, 0) == WAIT_TIMEOUT)
            {
                task.poll();
                task.wait(task.stop, task.wake_timer);
            }
        }
        catch (...) { task.failed.store(true); return 1; }
        return 0;
    }
    static void destroy(state *task)
    {
        if (task->thread != nullptr) CloseHandle(task->thread);
        if (task->wake_timer != nullptr)
        {
            CancelWaitableTimer(task->wake_timer);
            CloseHandle(task->wake_timer);
        }
        if (task->stop != nullptr) CloseHandle(task->stop);
        delete task;
    }
};
}
