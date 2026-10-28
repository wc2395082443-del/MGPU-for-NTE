#include "native_reflex.hpp"
#include "native_reflex_request.hpp"
#include "diag.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace mgpu::native_reflex
{
namespace
{
std::atomic<bool> armed{false};
std::atomic<bool> wrong_thread_reported{false};
int desired_mode = -1;
unsigned char *game_base = nullptr;
using setter = void (*)(void *, const wchar_t *, unsigned, std::uint64_t);
using thread_check = bool (*)();
constexpr std::uintptr_t object_slots[] = {0xfd37178, 0xfd371d8, 0xfd371a8};

struct binding
{
    void *object = nullptr;
    int *data = nullptr;
    setter set = nullptr;
};

bool match_hex(std::uintptr_t offset, const char *hex)
{
    for (std::size_t index = 0; hex[index * 2] != '\0'; ++index)
    {
        const auto digit = [](char value) { return value <= '9' ? value - '0' : value - 'a' + 10; };
        const auto expected = static_cast<unsigned char>((digit(hex[index * 2]) << 4) | digit(hex[index * 2 + 1]));
        if (game_base[offset + index] != expected) return false;
    }
    return true;
}

bool valid_image()
{
    __try
    {
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(game_base);
        if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto *image = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(game_base + dos->e_lfanew);
        if (image->Signature != IMAGE_NT_SIGNATURE || image->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            image->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            image->FileHeader.TimeDateStamp != 0xa463911e || image->OptionalHeader.SizeOfImage != 0x10d43000)
            return false;
        if (std::memcmp(game_base + 0xc9db698, L"t.Streamline.Reflex.Enable", sizeof(L"t.Streamline.Reflex.Enable")) != 0 ||
            std::memcmp(game_base + 0xc9dbaa0, L"t.Streamline.Reflex.Mode", sizeof(L"t.Streamline.Reflex.Mode")) != 0 ||
            std::memcmp(game_base + 0xc9db8a8, L"t.Streamline.Reflex.Auto", sizeof(L"t.Streamline.Reflex.Auto")) != 0)
            return false;
        return match_hex(0x1169343, "488d154e23870bff5018488d0d1c3d590d4889051ddebc0e48890d0edebc0e488bc8488b10ff5218c1e809a8017407e8f9463600eb34488b0df8ddbc") &&
            match_hex(0x1169786, "488d151323870bff5018488d0dd938590d4889053adabc0e48890d2bdabc0e488bc8488b10ff5218c1e809a8017407e8b6423600eb34488b0d15dabc") &&
            match_hex(0x14eb630, "48895c24084889742410574883ec20418bf8488bf2488bd9e85336000084c0742b8bd7488bcbe81572feff84c0741d488bceff1500ece5098943504533c0488b038bd7488bcbff9008010000488b5c2430488b7424384883c4205fc3") &&
            match_hex(0x15212f0, "4883ec28803da17c510e00742c8b0d05f2d10e65488b042558000000bad0090000488b04c8833c0202740ee85003000084c075054883c428c3b0014883c428c3") &&
            match_hex(0x14dab00, "8b4118c3") && match_hex(0x14d0180, "488d4150c3") &&
            match_hex(0x1168f06, "488d159b29870bff5018488d0d5941590d4889058ae2bc0e48890d7be2bc0e488bc8488b10ff5218c1e809a8017407e8364b3600eb34488b0d65e2bc");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool resolve_binding(unsigned index, binding &target)
{
    __try
    {
        auto *object = *reinterpret_cast<unsigned char **>(game_base + object_slots[index]);
        if (object == nullptr) return false;
        auto **vtable = *reinterpret_cast<void ***>(object);
        if (vtable != reinterpret_cast<void **>(game_base + 0xb3f13a0) ||
            vtable[0xa8 / sizeof(void *)] != game_base + 0x14eb630 ||
            vtable[0x18 / sizeof(void *)] != game_base + 0x14dab00 ||
            vtable[0x68 / sizeof(void *)] != game_base + 0x14d0180)
            return false;
        auto *data = *reinterpret_cast<int **>(game_base + object_slots[index] + sizeof(void *));
        if (reinterpret_cast<unsigned char *>(data) != object + 0x50) return false;
        target = {object, data, reinterpret_cast<setter>(vtable[0xa8 / sizeof(void *)])};
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

variable read_variable(const binding &target)
{
    return {target.data[0], *reinterpret_cast<const unsigned *>(static_cast<unsigned char *>(target.object) + 0x18)};
}

int requested()
{
    wchar_t filename[32768];
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", filename, 32768);
    if (length == 0 || length >= 32768) return -1;
    if (wcscat_s(filename, L"\\HT\\Saved\\Config\\Windows\\Engine.ini") != 0) return -1;
    wchar_t enable[16]{}, mode[16]{}, automatic[16]{};
    GetPrivateProfileStringW(L"SystemSettings", L"t.Streamline.Reflex.Enable", L"", enable, 16, filename);
    GetPrivateProfileStringW(L"SystemSettings", L"t.Streamline.Reflex.Mode", L"", mode, 16, filename);
    GetPrivateProfileStringW(L"SystemSettings", L"t.Streamline.Reflex.Auto", L"", automatic, 16, filename);
    return configured_mode(enable, mode, automatic);
}
}

void prepare()
{
    desired_mode = requested();
    if (desired_mode < 0) return;
    game_base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    if (!valid_image())
    {
        mgpu::diag::warn("[MGPU][M359-NATIVE] request refused: game build or native interface signature mismatch");
        return;
    }
    armed.store(true, std::memory_order_release);
    mgpu::diag::info(desired_mode == 0
        ? "[MGPU][M359-NATIVE] Engine.ini requests Mode Off/Auto Off with integration enabled; awaiting game-thread Sleep boundary"
        : "[MGPU][M359-NATIVE] Engine.ini requests On; validated native CVar interface, awaiting game-thread Sleep boundary");
}

void on_source_sleep()
{
    if (!armed.load(std::memory_order_acquire)) return;
    if (game_base[0xfa38f9c] != 1 || !reinterpret_cast<thread_check>(game_base + 0x15212f0)())
    {
        if (!wrong_thread_reported.exchange(true, std::memory_order_relaxed))
            mgpu::diag::warn("[MGPU][M359-NATIVE] request pending: native Sleep caller is not the game thread");
        return;
    }
    if (!armed.exchange(false, std::memory_order_acq_rel)) return;
    binding variables[3];
    if (!resolve_binding(0, variables[0]) || !resolve_binding(1, variables[1]) || !resolve_binding(2, variables[2]))
    {
        mgpu::diag::warn("[MGPU][M359-NATIVE] request refused: native CVar binding mismatch");
        return;
    }
    const auto before_enable = read_variable(variables[0]);
    const auto before_mode = read_variable(variables[1]);
    const auto before_auto = read_variable(variables[2]);
    const auto result = request_mode(desired_mode,
        [&](unsigned index) { return read_variable(variables[index]); },
        [&](unsigned index, int value)
        {
            const wchar_t *text = value == 0 ? L"0" : (value == 1 ? L"1" : L"2");
            variables[index].set(variables[index].object, text, set_by_code, 0);
        });
    const auto after_enable = read_variable(variables[0]);
    const auto after_mode = read_variable(variables[1]);
    const auto after_auto = read_variable(variables[2]);
    char line[640];
    std::snprintf(line, sizeof(line),
        "[MGPU][M359-NATIVE] result=%u thread=%lu Enable=%d->%d Mode=%d->%d flags=0x%08x/0x%08x; "
        "Auto=%d->%d auto_flags=0x%08x requested_mode=%d; native SetByCode request only, SDK mode and latency still require verification",
        static_cast<unsigned>(result), GetCurrentThreadId(), before_enable.value, after_enable.value,
        before_mode.value, after_mode.value, after_enable.flags, after_mode.flags,
        before_auto.value, after_auto.value, after_auto.flags, desired_mode);
    mgpu::diag::info(line);
}
}
