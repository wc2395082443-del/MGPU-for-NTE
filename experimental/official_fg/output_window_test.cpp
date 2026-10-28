#include "output_window.hpp"
#include <cstdio>

int wmain()
{
    WNDCLASSW description{};
    description.lpfnWndProc = DefWindowProcW;
    description.hInstance = GetModuleHandleW(nullptr);
    description.lpszClassName = L"MGPU_M351_Hidden_UI_Test";
    const auto atom = RegisterClassW(&description);
    if (!atom) return 1;
    const auto parent = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, MAKEINTATOM(atom), L"",
        WS_POPUP, 0, 0, 32, 32, nullptr, nullptr, description.hInstance, nullptr);
    if (!parent) return 2;
    int result = 0;
    try {
        mgpu::official_fg::output_window child;
        child.create(parent, GetCurrentProcessId());
        const auto handle = child.handle();
        DWORD process = 0;
        const auto thread = GetWindowThreadProcessId(handle, &process);
        if (thread == GetCurrentThreadId() || process != GetCurrentProcessId() ||
            GetParent(handle) != nullptr || GetWindow(handle, GW_OWNER) != nullptr ||
            GetAncestor(handle, GA_ROOT) != handle || IsWindowEnabled(handle) || IsWindowVisible(handle)) result = 3;
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_STYLE));
        const auto extended = static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_EXSTYLE));
        if ((style & WS_CHILD) || !(style & WS_POPUP) || !(style & WS_DISABLED) ||
            !(extended & WS_EX_NOACTIVATE) || !(extended & WS_EX_TOOLWINDOW)) result = 8;
        COLORREF color_key = 0;
        BYTE alpha = 0;
        DWORD attributes = 0;
        if (!(extended & WS_EX_LAYERED) || !(extended & WS_EX_TRANSPARENT) ||
            !GetLayeredWindowAttributes(handle, &color_key, &alpha, &attributes) ||
            alpha != 255 || attributes != LWA_ALPHA) result = 11;
        const auto before = child.pulse();
        Sleep(250);
        if (child.pulse() <= before + 3) result = 4;
        if (!SetWindowPos(parent, nullptr, -128, -64, 96, 80, SWP_NOACTIVATE | SWP_NOZORDER)) result = 9;
        const auto deadline = GetTickCount64() + 1000;
        RECT actual{};
        while (GetTickCount64() < deadline) {
            GetWindowRect(handle, &actual);
            if (actual.left == -128 && actual.top == -64 && actual.right == -32 && actual.bottom == 16) break;
            Sleep(5);
        }
        if (actual.left != -128 || actual.top != -64 || actual.right != -32 || actual.bottom != 16) result = 10;
        child.update(parent, true);
        Sleep(30);
        if (IsWindowVisible(parent) || IsWindowVisible(handle)) result = 5;
        child.update(parent, false);
        child.stop();
        if (IsWindow(handle)) result = 6;
        std::printf("Independent input/UI, negative-screen geometry, hidden output lifecycle result=%d\n", result);
    } catch (const std::exception &error) { std::printf("FAIL %s\n", error.what()); result = 7; }
    DestroyWindow(parent);
    UnregisterClassW(MAKEINTATOM(atom), description.hInstance);
    return result;
}
