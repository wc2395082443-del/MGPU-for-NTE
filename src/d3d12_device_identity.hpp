#pragma once

#include <d3d12.h>
#include <cstdint>

namespace mgpu::aligned
{
class device_identity
{
    inline static const GUID marker_guid =
        {0xd89d4e08, 0x7c92, 0x4832, {0xb6, 0x97, 0xd1, 0x5a, 0xa4, 0x54, 0x3e, 0x82}};
    IUnknown *identity_ = nullptr;
    LUID adapter_{};
    bool marked_ = false;

public:
    device_identity() = default;
    device_identity(const device_identity &) = delete;
    device_identity &operator=(const device_identity &) = delete;
    ~device_identity() { if (identity_ != nullptr) identity_->Release(); }

    explicit operator bool() const { return identity_ != nullptr; }
    bool marked() const { return marked_; }
    IUnknown *canonical() const { return identity_; }

    bool matches(ID3D12Object *device, LUID adapter) const
    {
        if (device == nullptr || identity_ == nullptr ||
            adapter.LowPart != adapter_.LowPart || adapter.HighPart != adapter_.HighPart) return false;
        IUnknown *identity = nullptr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))) || identity == nullptr) return false;
        const bool equal = identity == identity_;
        identity->Release();
        if (equal) return true;
        std::uintptr_t marker = 0;
        UINT bytes = sizeof marker;
        // Wrappers may have a different IUnknown but forward the native device's private data.
        return marked_ && SUCCEEDED(device->GetPrivateData(marker_guid, &bytes, &marker)) &&
            bytes == sizeof marker && marker == reinterpret_cast<std::uintptr_t>(this);
    }

    bool bind(ID3D12Object *device, LUID adapter)
    {
        if (identity_ != nullptr) return matches(device, adapter);
        IUnknown *identity = nullptr;
        if (device == nullptr || FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))) ||
            identity == nullptr) return false;
        identity_ = identity;
        adapter_ = adapter;
        const auto marker = reinterpret_cast<std::uintptr_t>(this);
        marked_ = SUCCEEDED(device->SetPrivateData(marker_guid, sizeof marker, &marker));
        return true;
    }
};
}
