#include <stddef.h>
#include "parameter_bridge.hpp"

#if !defined(_MSC_VER) || !defined(_M_X64)
#error Build this translation unit with the Windows x64 MSVC C++ ABI.
#endif

extern "C" NVSDK_NGX_Result probe_parameter_get_int(NVSDK_NGX_Parameter *parameters,
    const char *name, int *value)
{
    return parameters->Get(name, value);
}

extern "C" void probe_parameter_set_uint(NVSDK_NGX_Parameter *parameters,
    const char *name, unsigned int value)
{
    parameters->Set(name, value);
}

extern "C" NVSDK_NGX_Result probe_parameter_get_uint(NVSDK_NGX_Parameter *parameters,
    const char *name, unsigned int *value)
{
    return parameters->Get(name, value);
}

extern "C" void probe_parameter_set_ull(NVSDK_NGX_Parameter *parameters,
    const char *name, unsigned long long value)
{
    parameters->Set(name, value);
}

extern "C" void probe_parameter_set_resource(NVSDK_NGX_Parameter *parameters,
    const char *name, ID3D12Resource *value)
{
    parameters->Set(name, value);
}

extern "C" void probe_parameter_set_float(NVSDK_NGX_Parameter *parameters,
    const char *name, float value)
{
    parameters->Set(name, value);
}

extern "C" void probe_parameter_set_pointer(NVSDK_NGX_Parameter *parameters,
    const char *name, void *value)
{
    parameters->Set(name, value);
}
