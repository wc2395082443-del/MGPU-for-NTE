#include "source_fg_guard.hpp"
#include "diag.hpp"
#include "../experimental/official_fg/official_bridge.hpp"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <cwchar>

namespace mgpu::source_fg_guard
{
namespace
{
std::atomic<unsigned char *> validated_base{nullptr};

bool image_matches(unsigned char *base)
{
    __try {
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096)
            return false;
        const auto *image = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        if (image->Signature != IMAGE_NT_SIGNATURE || image->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            image->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            image->FileHeader.TimeDateStamp != 0x155d3c56 || image->OptionalHeader.SizeOfImage != 0x11628000)
            return false;
        constexpr unsigned char constructor[] = {
            0x48, 0x8d, 0x15, 0x66, 0x0c, 0xe0, 0x0b, 0xff, 0x50, 0x20,
            0x48, 0x8d, 0x0d, 0xdc, 0x20, 0xd9, 0x0d,
            0x48, 0x89, 0x05, 0xed, 0x12, 0x44, 0x0f};
        constexpr unsigned char accessor[] = {0x48, 0x8d, 0x41, 0x50, 0xc3};
        return std::memcmp(base + 0x11689b3, constructor, sizeof constructor) == 0 &&
            std::memcmp(base + 0x14fa700, accessor, sizeof accessor) == 0 &&
            std::memcmp(base + 0xcf69620, L"r.Streamline.ForceTagging", sizeof(L"r.Streamline.ForceTagging")) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool requested()
{
    wchar_t path[32768]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", path, 32768);
    if (!length || length >= 32768 || wcscat_s(path, L"\\HT\\Saved\\Config\\Windows\\Engine.ini") != 0)
        return false;
    wchar_t value[16]{};
    GetPrivateProfileStringW(L"SystemSettings", L"r.Streamline.ForceTagging", L"", value, 16, path);
    return std::wcscmp(value, L"1") == 0;
}

bool tagging_enabled(unsigned char *base)
{
    __try {
        auto *object = *reinterpret_cast<unsigned char **>(base + 0x105a9cb8);
        if (!object) return false;
        auto **table = *reinterpret_cast<void ***>(object);
        return table == reinterpret_cast<void **>(base + 0xb97ff70) &&
            table[0x60 / sizeof(void *)] == base + 0x14fa700 &&
            (*reinterpret_cast<unsigned *>(object + 0x18) & 0x00ffffffu) == 0x20 &&
            object[0x50] == 1 && object[0x51] == 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

void prepare()
{
    if (!mgpu::official_bridge::selected() || !requested()) return;
    auto *base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    if (!image_matches(base)) {
        mgpu::diag::warn("[MGPU][SOURCE-FG-OFF] guard refused: expected build 155d3c56/image 11628000 or ForceTagging binding differs");
        return;
    }
    validated_base.store(base, std::memory_order_release);
    mgpu::diag::info("[MGPU][SOURCE-FG-OFF] read-only game binding validated: build=155d3c56 image=11628000; source options guarded by live ForceTagging; no CVar setters");
}

bool ready()
{
    auto *base = validated_base.load(std::memory_order_acquire);
    return base && mgpu::official_bridge::selected() && tagging_enabled(base);
}
}
