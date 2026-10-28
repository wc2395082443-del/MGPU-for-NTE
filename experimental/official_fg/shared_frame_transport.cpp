#include "shared_frame_transport.hpp"
#include <dxgi1_4.h>
#include <utility>

namespace mgpu::official_fg
{
namespace
{
bool valid_description(const plane_description &description, unsigned plane)
{
    if (!description.width || !description.height || description.width > 16384 || description.height > 16384) return false;
    const auto format = static_cast<DXGI_FORMAT>(description.format);
    if (plane == 0) return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM;
    if (plane == 1) return format == DXGI_FORMAT_R32_FLOAT || format == DXGI_FORMAT_R16_FLOAT;
    return format == DXGI_FORMAT_R16G16_FLOAT || format == DXGI_FORMAT_R32G32_FLOAT;
}

bool matches(ID3D12Resource *texture, const plane_description &expected)
{
    if (!texture) return false;
    const auto description = texture->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && description.Width == expected.width &&
        description.Height == expected.height && description.Format == static_cast<DXGI_FORMAT>(expected.format) &&
        description.MipLevels == 1 && description.DepthOrArraySize == 1 && description.SampleDesc.Count == 1;
}

void transition(ID3D12GraphicsCommandList *commands, ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &barrier);
}
}

shared_frame_transport::~shared_frame_transport()
{
    for (auto handle : owned_handles_) CloseHandle(handle);
}

D3D12_RESOURCE_DESC shared_frame_transport::texture_description(const plane_description &description)
{
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    result.Width = description.width;
    result.Height = description.height;
    result.DepthOrArraySize = 1;
    result.MipLevels = 1;
    result.Format = static_cast<DXGI_FORMAT>(description.format);
    result.SampleDesc.Count = 1;
    result.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return result;
}

HRESULT shared_frame_transport::export_handle(ID3D12DeviceChild *object, std::uint64_t &output)
{
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE handle = nullptr;
    const auto result = device_->CreateSharedHandle(object, &attributes, GENERIC_ALL, nullptr, &handle);
    if (FAILED(result)) return result;
    owned_handles_.push_back(handle);
    output = reinterpret_cast<std::uintptr_t>(handle);
    return S_OK;
}

HRESULT shared_frame_transport::create(ID3D12Device *device,
    const std::array<plane_description, shared_planes> &descriptions, std::uint64_t generation)
{
    if (!device || device_ || generation == 0) return E_INVALIDARG;
    for (unsigned plane = 0; plane < shared_planes; ++plane)
        if (!valid_description(descriptions[plane], plane)) return E_INVALIDARG;
    device_ = device;
    producer_ = true;
    manifest_.byte_size = sizeof(shared_manifest);
    manifest_.generation = generation;
    manifest_.descriptions = descriptions;
    const auto adapter = device->GetAdapterLuid();
    manifest_.adapter_low = adapter.LowPart;
    manifest_.adapter_high = adapter.HighPart;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (unsigned slot = 0; slot < shared_slots; ++slot) {
        for (unsigned plane = 0; plane < shared_planes; ++plane) {
            const auto description = texture_description(descriptions[plane]);
            auto result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &description,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&textures_[slot][plane]));
            if (FAILED(result)) return result;
            result = export_handle(textures_[slot][plane].Get(), manifest_.resources[slot][plane]);
            if (FAILED(result)) return result;
        }
    }
    auto result = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ready_));
    if (SUCCEEDED(result)) result = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&released_));
    if (SUCCEEDED(result)) result = export_handle(ready_.Get(), manifest_.ready_fence);
    if (SUCCEEDED(result)) result = export_handle(released_.Get(), manifest_.released_fence);
    return result;
}

HRESULT shared_frame_transport::open(ID3D12Device *device, const shared_manifest &manifest)
{
    if (!device || device_ || manifest.magic != transport_magic || manifest.version != transport_version ||
        manifest.byte_size != sizeof(shared_manifest) || manifest.slots != shared_slots || !manifest.generation ||
        !manifest.ready_fence || !manifest.released_fence) return E_INVALIDARG;
    const auto adapter = device->GetAdapterLuid();
    if (adapter.LowPart != manifest.adapter_low || adapter.HighPart != manifest.adapter_high) return E_INVALIDARG;
    for (unsigned plane = 0; plane < shared_planes; ++plane)
        if (!valid_description(manifest.descriptions[plane], plane)) return E_INVALIDARG;
    device_ = device;
    manifest_ = manifest;
    for (unsigned slot = 0; slot < shared_slots; ++slot) {
        for (unsigned plane = 0; plane < shared_planes; ++plane) {
            const auto handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(manifest.resources[slot][plane]));
            if (!handle || handle == INVALID_HANDLE_VALUE) return E_INVALIDARG;
            const auto result = device->OpenSharedHandle(handle, IID_PPV_ARGS(&textures_[slot][plane]));
            if (FAILED(result)) return result;
            if (!matches(textures_[slot][plane].Get(), manifest.descriptions[plane])) return E_INVALIDARG;
        }
    }
    auto result = device->OpenSharedHandle(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(manifest.ready_fence)),
        IID_PPV_ARGS(&ready_));
    if (SUCCEEDED(result)) result = device->OpenSharedHandle(
        reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(manifest.released_fence)), IID_PPV_ARGS(&released_));
    return result;
}

std::vector<HANDLE> shared_frame_transport::inheritable_handles() const { return owned_handles_; }

ID3D12Resource *shared_frame_transport::texture(unsigned slot, unsigned plane) const
{
    return slot < shared_slots && plane < shared_planes ? textures_[slot][plane].Get() : nullptr;
}

bool shared_frame_transport::same_device(ID3D12DeviceChild *object) const
{
    if (!device_ || !object) return false;
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    if (FAILED(object->GetDevice(IID_PPV_ARGS(&owner)))) return false;
    Microsoft::WRL::ComPtr<IUnknown> actual, expected;
    return SUCCEEDED(owner.As(&actual)) && SUCCEEDED(device_.As(&expected)) && actual.Get() == expected.Get();
}

HRESULT shared_frame_transport::record_copy(ID3D12GraphicsCommandList *commands, unsigned slot,
    const std::array<texture_binding, shared_planes> &bindings, bool writing) const
{
    if (slot >= shared_slots || !same_device(commands) || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        writing != producer_ || !ready_ || !released_) return E_INVALIDARG;
    for (unsigned plane = 0; plane < shared_planes; ++plane)
        if (!same_device(bindings[plane].texture) || !matches(bindings[plane].texture, manifest_.descriptions[plane]) ||
            bindings[plane].texture == textures_[slot][plane].Get()) return E_INVALIDARG;
    for (unsigned plane = 0; plane < shared_planes; ++plane) {
        auto *shared = textures_[slot][plane].Get();
        auto *other = bindings[plane].texture;
        const auto shared_state = writing ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
        const auto other_state = writing ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
        transition(commands, shared, D3D12_RESOURCE_STATE_COMMON, shared_state);
        transition(commands, other, bindings[plane].state, other_state);
        commands->CopyResource(writing ? shared : other, writing ? other : shared);
        transition(commands, other, other_state, bindings[plane].state);
        transition(commands, shared, shared_state, D3D12_RESOURCE_STATE_COMMON);
    }
    return S_OK;
}

HRESULT shared_frame_transport::record_write(ID3D12GraphicsCommandList *commands, unsigned slot,
    const std::array<texture_binding, shared_planes> &sources) const
{
    return record_copy(commands, slot, sources, true);
}

HRESULT shared_frame_transport::record_read(ID3D12GraphicsCommandList *commands, unsigned slot,
    const std::array<texture_binding, shared_planes> &destinations) const
{
    return record_copy(commands, slot, destinations, false);
}

HRESULT shared_frame_transport::signal_ready(ID3D12CommandQueue *queue, std::uint64_t sequence) const
{
    return producer_ && same_device(queue) && ready_ && sequence && sequence != UINT64_MAX
        ? queue->Signal(ready_.Get(), sequence) : E_INVALIDARG;
}

HRESULT shared_frame_transport::wait_ready(ID3D12CommandQueue *queue, std::uint64_t sequence) const
{
    return !producer_ && same_device(queue) && ready_ && sequence && sequence != UINT64_MAX
        ? queue->Wait(ready_.Get(), sequence) : E_INVALIDARG;
}

HRESULT shared_frame_transport::signal_released(ID3D12CommandQueue *queue, std::uint64_t sequence) const
{
    return !producer_ && same_device(queue) && released_ && sequence && sequence != UINT64_MAX
        ? queue->Signal(released_.Get(), sequence) : E_INVALIDARG;
}
}
