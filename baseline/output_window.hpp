#pragma once

#include <windows.h>
#include <atomic>
#include <thread>
#include <stdexcept>
#include <cstdio>

namespace mgpu::official_fg
{
class output_window
{
public:
    output_window() = default;
    output_window(const output_window &) = delete;
    output_window &operator=(const output_window &) = delete;
    ~output_window() { stop(); }
    void create(HWND parent, DWORD expected_pid)
    {
        if (thread_.joinable()) throw std::runtime_error("output UI thread already exists");
        parent_ = parent;
        expected_pid_ = expected_pid;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!stop_ || !ready_ || !wake_) { stop(); throw std::runtime_error("output UI events"); }
        thread_ = std::thread([this] { run(); });
        if (WaitForSingleObject(ready_, 5000) != WAIT_OBJECT_0 || !window_.load() || failure_.load()) {
            stop();
            throw std::runtime_error("output UI startup failed");
        }
    }
    HWND handle() const { return window_.load(std::memory_order_acquire); }
    unsigned long long pulse() const { return pulse_.load(); }
    void update(HWND parent, bool visible)
    {
        if (parent != parent_ || failure_.load()) throw std::runtime_error("output UI no longer available");
        if (visible_.exchange(visible) != visible) SetEvent(wake_);
    }
    void stop()
    {
        if (thread_.joinable()) {
            SetEvent(stop_);
            if (WaitForSingleObject(thread_.native_handle(), 3000) != WAIT_OBJECT_0) {
                std::printf("[M351] helper UI thread stop timed out; terminating helper only\n");
                std::fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 94);
            }
            thread_.join();
        }
        for (HANDLE *event : {&stop_, &ready_, &wake_}) {
            if (*event) { CloseHandle(*event); *event = nullptr; }
        }
    }
private:
    void run()
    {
        DWORD owner = 0;
        GetWindowThreadProcessId(parent_, &owner);
        if (!IsWindow(parent_) || owner != expected_pid_) { failure_.store(true); SetEvent(ready_); return; }
        SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(parent_));
        WNDCLASSW description{};
        description.lpfnWndProc = DefWindowProcW;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = L"MGPU_Official_Embedded_Output";
        const ATOM atom = RegisterClassW(&description);
        if (!atom) { failure_.store(true); SetEvent(ready_); return; }
        RECT rectangle{};
        HWND window = nullptr;
        if (GetClientRect(parent_, &rectangle)) {
            window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY, MAKEINTATOM(atom), L"",
                WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0, rectangle.right, rectangle.bottom,
                parent_, nullptr, description.hInstance, nullptr);
        }
        if (!window) failure_.store(true);
        window_.store(window, std::memory_order_release);
        SetEvent(ready_);
        while (window && WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
            RECT desired{}, current{};
            if (!IsWindow(parent_) || !GetClientRect(parent_, &desired) || !GetClientRect(window, &current)) {
                failure_.store(true);
                break;
            }
            if (desired.right != current.right || desired.bottom != current.bottom)
                if (!SetWindowPos(window, HWND_TOP, 0, 0, desired.right, desired.bottom, SWP_NOACTIVATE)) {
                    failure_.store(true); break;
                }
            const bool wanted = visible_.load();
            if ((IsWindowVisible(window) != FALSE) != (wanted && IsWindowVisible(parent_)))
                ShowWindow(window, wanted ? SW_SHOWNOACTIVATE : SW_HIDE);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            pulse_.fetch_add(1);
            HANDLE waits[] = {stop_, wake_};
            const DWORD result = MsgWaitForMultipleObjects(2, waits, FALSE, 16, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) break;
        }
        if (window) { ShowWindow(window, SW_HIDE); DestroyWindow(window); }
        window_.store(nullptr);
        UnregisterClassW(MAKEINTATOM(atom), description.hInstance);
    }
    HWND parent_ = nullptr;
    DWORD expected_pid_ = 0;
    std::atomic<HWND> window_{nullptr};
    HANDLE stop_ = nullptr, ready_ = nullptr, wake_ = nullptr;
    std::atomic<bool> visible_{false}, failure_{false};
    std::atomic<unsigned long long> pulse_{0};
    std::thread thread_;
};
}
