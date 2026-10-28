#include <stddef.h>
#include "parameter_bridge.hpp"

#if !defined(_MSC_VER) || !defined(_M_X64)
#error The fake SDK parameter object must also use the Windows x64 MSVC ABI.
#endif

namespace
{
class fake_parameters final : public NVSDK_NGX_Parameter
{
public:
    void *expected_pointer = nullptr;
    mutable unsigned calls = 0;
    mutable unsigned unexpected = 0;

    void Set(const char *name, unsigned long long value) override
    { if (name[0] == 'v' && value == 0x123456789abcdef0ull) calls |= 1; else ++unexpected; }
    void Set(const char *name, float value) override
    { if (name[0] == 'v' && value == 0.5f) calls |= 2; else ++unexpected; }
    void Set(const char *, double) override { ++unexpected; }
    void Set(const char *name, unsigned int value) override
    { if (name[0] == 'v' && value == 12345u) calls |= 4; else ++unexpected; }
    void Set(const char *, int) override { ++unexpected; }
    void Set(const char *, ID3D11Resource *) override { ++unexpected; }
    void Set(const char *name, ID3D12Resource *value) override
    { if (name[0] == 'v' && value == expected_pointer) calls |= 8; else ++unexpected; }
    void Set(const char *name, void *value) override
    { if (name[0] == 'v' && value == expected_pointer) calls |= 16; else ++unexpected; }

    NVSDK_NGX_Result Get(const char *, unsigned long long *) const override { return wrong_get(); }
    NVSDK_NGX_Result Get(const char *, float *) const override { return wrong_get(); }
    NVSDK_NGX_Result Get(const char *, double *) const override { return wrong_get(); }
    NVSDK_NGX_Result Get(const char *name, unsigned int *value) const override
    {
        if (name[0] != 'v' || value == nullptr) return wrong_get();
        calls |= 64;
        *value = 5u;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char *name, int *value) const override
    {
        if (name[0] != 'v' || value == nullptr) return wrong_get();
        calls |= 32;
        *value = 87654;
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char *, ID3D11Resource **) const override { return wrong_get(); }
    NVSDK_NGX_Result Get(const char *, ID3D12Resource **) const override { return wrong_get(); }
    NVSDK_NGX_Result Get(const char *, void **) const override { return wrong_get(); }
    void Reset() override { ++unexpected; }

private:
    NVSDK_NGX_Result wrong_get() const
    {
        ++unexpected;
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
};
}

extern "C" int test_parameter_bridge()
{
    unsigned sentinel = 0;
    fake_parameters parameters;
    parameters.expected_pointer = &sentinel;
    const mgpu::secondary_fg::parameter_writer writer(&parameters);
    writer.Set("value", 0x123456789abcdef0ull);
    writer.Set("value", 0.5f);
    writer.Set("value", 12345u);
    writer.Set("value", reinterpret_cast<ID3D12Resource *>(&sentinel));
    writer.Set("value", static_cast<void *>(&sentinel));
    int value = 0;
    const auto result = probe_parameter_get_int(&parameters, "value", &value);
    unsigned maximum = 0;
    const auto unsigned_result = probe_parameter_get_uint(&parameters, "value", &maximum);
    return result == NVSDK_NGX_Result_Success && value == 87654 &&
        unsigned_result == NVSDK_NGX_Result_Success && maximum == 5 &&
        parameters.calls == 127 && parameters.unexpected == 0 ? 0 : 1;
}
