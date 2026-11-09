#include "../src/source_fg_guard.cpp"
#include <cstdio>

namespace mgpu::diag
{
void info(const char *) {}
void warn(const char *) {}
}

namespace mgpu::official_bridge
{
bool test_selected = true;
bool selected() { return test_selected; }
}

int main()
{
    using namespace mgpu::source_fg_guard;
    unsigned failures = 0;
    const auto check = [&](bool value, const char *name) {
        if (!value) { ++failures; std::printf("FAIL %s\n", name); }
    };
    auto *base = static_cast<unsigned char *>(VirtualAlloc(nullptr, 0x11628000, MEM_RESERVE, PAGE_NOACCESS));
    if (!base) return 2;
    const auto commit = [&](std::size_t offset) {
        return VirtualAlloc(base + (offset & ~std::size_t(4095)), 4096, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    };
    for (const auto offset : {0u, 0x11689b3u, 0x14fa700u, 0xcf69620u, 0x105a9cb8u})
        if (!commit(offset)) { VirtualFree(base, 0, MEM_RELEASE); return 3; }
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 128;
    auto *image = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + 128);
    image->Signature = IMAGE_NT_SIGNATURE;
    image->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    image->FileHeader.TimeDateStamp = 0x155d3c56;
    image->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    image->OptionalHeader.SizeOfImage = 0x11628000;
    constexpr unsigned char constructor[] = {
        0x48, 0x8d, 0x15, 0x66, 0x0c, 0xe0, 0x0b, 0xff, 0x50, 0x20,
        0x48, 0x8d, 0x0d, 0xdc, 0x20, 0xd9, 0x0d,
        0x48, 0x89, 0x05, 0xed, 0x12, 0x44, 0x0f};
    constexpr unsigned char accessor[] = {0x48, 0x8d, 0x41, 0x50, 0xc3};
    std::memcpy(base + 0x11689b3, constructor, sizeof constructor);
    std::memcpy(base + 0x14fa700, accessor, sizeof accessor);
    std::memcpy(base + 0xcf69620, L"r.Streamline.ForceTagging", sizeof(L"r.Streamline.ForceTagging"));
    check(image_matches(base), "exact supported image accepted");
    image->FileHeader.TimeDateStamp ^= 1;
    check(!image_matches(base), "changed game build refused");
    image->FileHeader.TimeDateStamp ^= 1;
    image->OptionalHeader.SizeOfImage ^= 4096;
    check(!image_matches(base), "changed image size refused");
    image->OptionalHeader.SizeOfImage ^= 4096;
    image->FileHeader.TimeDateStamp = 0xa463911e;
    image->OptionalHeader.SizeOfImage = 0x10d43000;
    check(!image_matches(base), "obsolete game identity refused");
    image->FileHeader.TimeDateStamp = 0x155d3c56;
    image->OptionalHeader.SizeOfImage = 0x11628000;
    base[0x11689b3] ^= 1;
    check(!image_matches(base), "changed constructor refused");
    base[0x11689b3] ^= 1;
    base[0x11689c7] ^= 1;
    check(!image_matches(base), "changed constructor object slot refused");
    base[0x11689c7] ^= 1;
    base[0x14fa703] ^= 1;
    check(!image_matches(base), "changed value accessor refused");
    base[0x14fa703] ^= 1;
    base[0xcf69620] ^= 1;
    check(!image_matches(base), "changed CVar identity refused");
    base[0xcf69620] ^= 1;
    check(!image_matches(nullptr), "unreadable image refused");
    check(!tagging_enabled(base), "uninitialized tagging refused");
    alignas(void *) unsigned char object[128]{};
    *reinterpret_cast<unsigned char **>(base + 0x105a9cb8) = object;
    *reinterpret_cast<void ***>(object) = reinterpret_cast<void **>(base + 0xb97ff70);
    if (!commit(0xb97ff70)) { VirtualFree(base, 0, MEM_RELEASE); return 4; }
    reinterpret_cast<void **>(base + 0xb97ff70)[0x60 / sizeof(void *)] = base + 0x14fa700;
    *reinterpret_cast<unsigned *>(object + 0x18) = 0x0d000020;
    object[0x50] = object[0x51] = 1;
    check(tagging_enabled(base), "both game and render tagging copies enabled");
    validated_base.store(base);
    check(ready(), "validated official route enables rewrite guard");
    *reinterpret_cast<void ***>(object) = reinterpret_cast<void **>(base + 0xb97ff78);
    check(!ready(), "different CVar type refused");
    *reinterpret_cast<void ***>(object) = reinterpret_cast<void **>(base + 0xb97ff70);
    reinterpret_cast<void **>(base + 0xb97ff70)[0x60 / sizeof(void *)] = base + 0x14fa710;
    check(!ready(), "different CVar getter refused");
    reinterpret_cast<void **>(base + 0xb97ff70)[0x60 / sizeof(void *)] = base + 0x14fa700;
    object[0x50] = 0;
    check(!ready(), "game tagging lost stops rewrite immediately");
    object[0x50] = 1;
    object[0x51] = 0;
    check(!ready(), "render tagging lost stops rewrite immediately");
    object[0x51] = 1;
    object[0x51] = 2;
    check(!ready(), "non-boolean tagging value refused");
    object[0x51] = 1;
    mgpu::official_bridge::test_selected = false;
    check(!ready(), "non-helper route stops rewrite");
    mgpu::official_bridge::test_selected = true;
    *reinterpret_cast<unsigned *>(object + 0x18) = 0x21;
    check(!ready(), "unknown tagging flags refused");
    validated_base.store(nullptr);
    check(!ready(), "missing image validation refuses rewrite");
    VirtualFree(base, 0, MEM_RELEASE);
    std::printf("guard fixture failures=%u\n", failures);
    return failures ? 1 : 0;
}
