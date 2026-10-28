#if defined(MGPU_LEGACY_INPUT_CONTROL)
#include "../../baseline/output_window.hpp"
#else
#include "output_window.hpp"
#endif
#include <string>

namespace
{
int private_case()
{
    WNDCLASSW description{};
    description.lpfnWndProc = DefWindowProcW;
    description.hInstance = GetModuleHandleW(nullptr);
    description.lpszClassName = L"MGPU_Input_Queue_Parent_Test";
    const auto atom = RegisterClassW(&description);
    if (!atom) return 10;
    const auto parent = CreateWindowExW(WS_EX_TOOLWINDOW, MAKEINTATOM(atom), L"",
        WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, description.hInstance, nullptr);
    if (!parent) return 11;
    int result = 0;
    try {
        mgpu::official_fg::output_window window;
        window.create(parent, GetCurrentProcessId());
        SetFocus(parent);
        SetCapture(parent);
        GUITHREADINFO parent_state{sizeof(parent_state)}, output_state{sizeof(output_state)};
        const auto output_thread = GetWindowThreadProcessId(window.handle(), nullptr);
        if (!GetGUIThreadInfo(GetCurrentThreadId(), &parent_state) ||
            !GetGUIThreadInfo(output_thread, &output_state)) result = 12;
        if (parent_state.hwndFocus != parent || parent_state.hwndCapture != parent) result = 13;
        const bool linked = output_state.hwndFocus == parent && output_state.hwndCapture == parent;
#if defined(MGPU_LEGACY_INPUT_CONTROL)
        if (!linked) result = 14;
#else
        if (output_state.hwndFocus == parent || output_state.hwndCapture == parent ||
            GetParent(window.handle()) || GetWindow(window.handle(), GW_OWNER)) result = 15;
#endif
        std::printf("private-desktop input-state control: parent_thread=%lu output_thread=%lu linked=%u result=%d\n",
            static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long>(output_thread),
            linked ? 1u : 0u, result);
        ReleaseCapture();
        SetFocus(nullptr);
        window.stop();
    } catch (const std::exception &error) {
        std::printf("FAIL private-desktop case: %s\n", error.what());
        result = 16;
    }
    DestroyWindow(parent);
    UnregisterClassW(MAKEINTATOM(atom), description.hInstance);
    return result;
}
}

int wmain(int count, wchar_t **arguments)
{
    if (count == 2 && std::wstring(arguments[1]) == L"--private") return private_case();
    const auto desktop_name = L"MGPU_InputIsolation_" + std::to_wstring(GetCurrentProcessId()) +
        L"_" + std::to_wstring(GetTickCount64());
    const auto desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) return 20;
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable)) { CloseDesktop(desktop); return 21; }
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --private";
    std::wstring desktop_path = L"WinSta0\\" + desktop_name;
    STARTUPINFOW startup{sizeof(startup)};
    startup.lpDesktop = desktop_path.data();
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process)) {
        const auto error = GetLastError();
        CloseDesktop(desktop);
        std::printf("FAIL private-desktop launch: %lu\n", static_cast<unsigned long>(error));
        return 22;
    }
    const auto wait = WaitForSingleObject(process.hProcess, 10000);
    DWORD result = 23;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &result);
    else { TerminateProcess(process.hProcess, result); WaitForSingleObject(process.hProcess, 1000); }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseDesktop(desktop);
    std::printf("isolated-desktop test exit=%lu; input desktop never switched\n", static_cast<unsigned long>(result));
    return static_cast<int>(result);
}
