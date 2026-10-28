#include "../src/motion_source_audit.hpp"
#include <cstring>
#include <cwchar>

struct named_resource
{
    const wchar_t *wide = nullptr;
    const char *narrow = nullptr;
    unsigned calls = 0;

    HRESULT GetPrivateData(REFGUID key, UINT *bytes, void *output)
    {
        ++calls;
        const void *value = nullptr;
        size_t required = 0;
        if (key == WKPDID_D3DDebugObjectNameW && wide != nullptr)
        {
            value = wide;
            required = std::wcslen(wide) * sizeof(wchar_t);
        }
        else if (key == WKPDID_D3DDebugObjectName && narrow != nullptr)
        {
            value = narrow;
            required = std::strlen(narrow);
        }
        if (value == nullptr) return E_FAIL;
        if (required > *bytes) { *bytes = static_cast<UINT>(required); return E_FAIL; }
        std::memcpy(output, value, required);
        *bytes = static_cast<UINT>(required);
        return S_OK;
    }
};

int main()
{
    unsigned failures = 0;
    const auto check = [&failures](bool passed, const char *name)
    {
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    char name[192];
    named_resource wide{L"SceneVelocity", "fallback"};
    mgpu::motion_audit::read_name(&wide, name, sizeof name);
    check(std::strcmp(name, "SceneVelocity") == 0, "wide resource label");
    check(wide.calls == 1, "no redundant lookup for available wide label");
    named_resource narrow{nullptr, "Velocity\nScratch\tBuffer"};
    mgpu::motion_audit::read_name(&narrow, name, sizeof name);
    check(std::strcmp(name, "Velocity Scratch Buffer") == 0, "narrow fallback sanitizes control bytes");
    named_resource absent;
    mgpu::motion_audit::read_name(&absent, name, sizeof name);
    check(std::strcmp(name, "<unnamed>") == 0, "missing names are not inferred");
    mgpu::motion_audit::read_name<named_resource>(nullptr, name, sizeof name);
    check(std::strcmp(name, "<unnamed>") == 0, "null resources are not accessed");
    char tiny[1] = {'x'};
    mgpu::motion_audit::read_name(&wide, tiny, sizeof tiny);
    check(tiny[0] == '\0', "small output stays terminated");
    mgpu::motion_audit::read_name(&wide, nullptr, 0);
    const mgpu::motion_audit::sample_stamp empty{};
    const mgpu::motion_audit::sample_stamp captured{300, 0x1234};
    check(empty.source_at(0) == 0, "uninitialized sample has no source");
    check(captured.source_at(300) == 0x1234, "sample identity matches captured seal");
    check(captured.source_at(306) == 0, "recycled slot cannot claim an older source");
    const auto pending = captured;
    const mgpu::motion_audit::sample_stamp recycled{306, 0x5678};
    check(pending.frame == 300 && pending.source == 0x1234 && recycled.source_at(300) == 0,
          "readback keeps the sampled frame instead of the later read frame");
    return failures == 0 ? 0 : 1;
}
