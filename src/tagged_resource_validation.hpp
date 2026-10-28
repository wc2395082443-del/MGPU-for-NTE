#pragma once

#include <d3d12.h>

namespace mgpu::tagged_motion
{
inline bool query_resource(void *native, ID3D12Resource *&resource,
                           D3D12_RESOURCE_DESC &description)
{
    resource = nullptr;
    description = {};
    if (native == nullptr) return false;
    bool valid = false;
    __try
    {
        auto *unknown = static_cast<IUnknown *>(native);
        if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&resource))) && resource != nullptr)
        {
            description = resource->GetDesc();
            valid = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!valid && resource != nullptr)
    {
        __try { resource->Release(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        resource = nullptr;
    }
    return valid;
}
}
