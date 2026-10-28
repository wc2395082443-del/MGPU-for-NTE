#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace mgpu::neural_pipeline
{
class difference_audit
{
public:
    struct difference { double mean = 0, changed_fraction = 0; };
    HRESULT record(ID3D12GraphicsCommandList *command, ID3D12Resource *input, ID3D12Resource *output)
    {
        if (pending_) return E_UNEXPECTED;
        const auto description = output->GetDesc();
        if (description.Format != DXGI_FORMAT_R10G10B10A2_UNORM &&
            description.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return S_FALSE;
        if (readback_[0] == nullptr)
        {
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            HRESULT result = output->GetDevice(IID_PPV_ARGS(&device));
            if (FAILED(result)) return result;
            auto region = description;
            region.Width = (std::min)(description.Width, UINT64{64});
            region.Height = (std::min)(description.Height, UINT{64});
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&region, 0, 1, 0, &layout_, nullptr, nullptr, &bytes);
            stride_ = (bytes + 511ull) & ~511ull;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = stride_ * 3;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> created;
            for (auto &entry : created)
            {
                result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&entry));
                if (FAILED(result)) return result;
            }
            readback_ = std::move(created);
        }
        for (unsigned side = 0; side != 2; ++side)
        {
            ID3D12Resource *source = side == 0 ? input : output;
            D3D12_RESOURCE_BARRIER transition{};
            transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
            command->ResourceBarrier(1, &transition);
            for (unsigned region = 0; region != 3; ++region)
            {
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.pResource = source;
                from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.pResource = readback_[side].Get();
                to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                to.PlacedFootprint = layout_;
                to.PlacedFootprint.Offset = stride_ * region;
                const UINT left = UINT(description.Width - layout_.Footprint.Width) / 2u;
                const UINT top = (description.Height - layout_.Footprint.Height) * (region + 1u) / 4u;
                const D3D12_BOX box{left, top, 0, left + layout_.Footprint.Width, top + layout_.Footprint.Height, 1};
                command->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            }
            std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
            command->ResourceBarrier(1, &transition);
        }
        pending_ = true;
        return S_OK;
    }
    bool collect(std::array<difference, 3> &result)
    {
        if (!pending_) return false;
        void *input = nullptr, *output = nullptr;
        const D3D12_RANGE range{0, SIZE_T(stride_ * 3)};
        if (FAILED(readback_[0]->Map(0, &range, &input))) return false;
        if (FAILED(readback_[1]->Map(0, &range, &output)))
        {
            const D3D12_RANGE empty{};
            readback_[0]->Unmap(0, &empty);
            return false;
        }
        const unsigned bits = layout_.Footprint.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? 10u : 8u;
        const unsigned mask = (1u << bits) - 1u;
        for (unsigned region = 0; region != 3; ++region)
        {
            double total = 0;
            unsigned changed = 0;
            for (UINT row = 0; row != layout_.Footprint.Height; ++row)
                for (UINT column = 0; column != layout_.Footprint.Width; ++column)
                {
                    const auto offset = stride_ * region + row * layout_.Footprint.RowPitch + column * 4;
                    unsigned original = 0, neural = 0;
                    std::memcpy(&original, static_cast<const char *>(input) + offset, 4);
                    std::memcpy(&neural, static_cast<const char *>(output) + offset, 4);
                    bool differs = false;
                    for (unsigned channel = 0; channel != 3; ++channel)
                    {
                        const int delta = int((original >> (channel * bits)) & mask) - int((neural >> (channel * bits)) & mask);
                        total += std::abs(delta) / double(mask);
                        differs = differs || delta != 0;
                    }
                    if (differs) ++changed;
                }
            const double pixels = double(layout_.Footprint.Width) * layout_.Footprint.Height;
            result[region] = {total / (pixels * 3.0), changed / pixels};
        }
        const D3D12_RANGE empty{};
        readback_[0]->Unmap(0, &empty);
        readback_[1]->Unmap(0, &empty);
        pending_ = false;
        return true;
    }
    void release() { readback_ = {}; pending_ = false; }
private:
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> readback_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout_{};
    UINT64 stride_ = 0;
    bool pending_ = false;
};
}
