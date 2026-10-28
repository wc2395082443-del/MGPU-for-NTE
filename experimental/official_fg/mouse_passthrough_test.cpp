#if defined(MGPU_M362_MOUSE_CONTROL)
#include "../../baseline/m362_output_window.hpp"
#else
#include "output_window.hpp"
#endif

int wmain()
{
    WNDCLASSW description{};
    description.lpfnWndProc = DefWindowProcW;
    description.hInstance = GetModuleHandleW(nullptr);
    description.lpszClassName = L"MGPU_Mouse_Passthrough_Contract_Test";
    const auto atom = RegisterClassW(&description);
    if (!atom) return 1;
    const auto parent = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, MAKEINTATOM(atom), L"",
        WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, description.hInstance, nullptr);
    if (!parent) return 2;
    int result = 0;
    try {
        mgpu::official_fg::output_window output;
        output.create(parent, GetCurrentProcessId());
        const auto handle = output.handle();
        const auto extended = static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_EXSTYLE));
        COLORREF color_key = 0;
        BYTE alpha = 0;
        DWORD attributes = 0;
        const bool initialized = GetLayeredWindowAttributes(handle, &color_key, &alpha, &attributes) != FALSE;
        const bool documented_passthrough = (extended & WS_EX_LAYERED) && (extended & WS_EX_TRANSPARENT) &&
            initialized && alpha == 255 && attributes == LWA_ALPHA;
        if (GetParent(handle) || GetWindow(handle, GW_OWNER) || IsWindowVisible(handle) ||
            IsWindowEnabled(handle) || !(extended & WS_EX_NOACTIVATE)) result = 3;
#if defined(MGPU_M362_MOUSE_CONTROL)
        if (documented_passthrough) result = 4;
#else
        if (!documented_passthrough) result = 5;
#endif
        std::printf("hidden-window mouse contract: layered=%u paint_transparent=%u alpha_initialized=%u alpha=%u documented_passthrough=%u result=%d; no mouse events injected\n",
            (extended & WS_EX_LAYERED) ? 1u : 0u, (extended & WS_EX_TRANSPARENT) ? 1u : 0u,
            initialized ? 1u : 0u, static_cast<unsigned>(alpha), documented_passthrough ? 1u : 0u, result);
        output.stop();
    } catch (const std::exception &error) {
        std::printf("FAIL %s\n", error.what());
        result = 6;
    }
    DestroyWindow(parent);
    UnregisterClassW(MAKEINTATOM(atom), description.hInstance);
    return result;
}
