#pragma once

#include <windows.h>
#include <d3dcommon.h>
#include <cstddef>
#include <cstdio>

namespace mgpu::motion_audit
{
struct sample_stamp
{
    unsigned long long frame = 0;
    unsigned long long source = 0;

    unsigned long long source_at(unsigned long long expected_frame) const
    {
        return frame != 0 && frame == expected_frame ? source : 0;
    }
};

template <typename Resource>
void read_name(Resource *resource, char *output, size_t capacity)
{
    if (output == nullptr || capacity == 0) return;
    std::snprintf(output, capacity, "<unnamed>");
    if (resource == nullptr) return;

    wchar_t wide[192]{};
    UINT bytes = sizeof wide - sizeof(wchar_t);
    if (SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectNameW, &bytes, wide)) &&
        bytes != 0 && bytes < sizeof wide && bytes % sizeof(wchar_t) == 0)
    {
        wide[bytes / sizeof(wchar_t)] = L'\0';
        if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, output,
                               static_cast<int>(capacity), nullptr, nullptr) == 0)
            std::snprintf(output, capacity, "<name-too-long>");
    }
    else
    {
        char narrow[192]{};
        bytes = sizeof narrow - 1;
        if (SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectName, &bytes, narrow)) &&
            bytes != 0 && bytes < sizeof narrow)
        {
            narrow[bytes] = '\0';
            std::snprintf(output, capacity, "%s", narrow);
        }
    }
    for (size_t index = 0; output[index] != '\0'; ++index)
        if (static_cast<unsigned char>(output[index]) < 32u || output[index] == 127)
            output[index] = ' ';
}
}
