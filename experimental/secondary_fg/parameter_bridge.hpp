#pragma once

#include "../../ext/ngx/nvsdk_ngx_params.h"

extern "C" NVSDK_NGX_Result probe_parameter_get_int(NVSDK_NGX_Parameter *, const char *, int *);
extern "C" NVSDK_NGX_Result probe_parameter_get_uint(NVSDK_NGX_Parameter *, const char *, unsigned int *);
extern "C" void probe_parameter_set_uint(NVSDK_NGX_Parameter *, const char *, unsigned int);
extern "C" void probe_parameter_set_ull(NVSDK_NGX_Parameter *, const char *, unsigned long long);
extern "C" void probe_parameter_set_float(NVSDK_NGX_Parameter *, const char *, float);
extern "C" void probe_parameter_set_resource(NVSDK_NGX_Parameter *, const char *, ID3D12Resource *);
extern "C" void probe_parameter_set_pointer(NVSDK_NGX_Parameter *, const char *, void *);

namespace mgpu::secondary_fg
{
class parameter_writer
{
public:
    explicit parameter_writer(NVSDK_NGX_Parameter *handle) : handle_(handle) {}

    void Set(const char *name, unsigned int value) const { probe_parameter_set_uint(handle_, name, value); }
    void Set(const char *name, unsigned long long value) const { probe_parameter_set_ull(handle_, name, value); }
    void Set(const char *name, float value) const { probe_parameter_set_float(handle_, name, value); }
    void Set(const char *name, ID3D12Resource *value) const { probe_parameter_set_resource(handle_, name, value); }
    void Set(const char *name, void *value) const { probe_parameter_set_pointer(handle_, name, value); }

private:
    NVSDK_NGX_Parameter *handle_;
};
}
