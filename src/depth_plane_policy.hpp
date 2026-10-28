#pragma once

#include "aligned_input.hpp"

namespace mgpu::aligned
{
struct depth_conversion
{
    UINT source_pitch = 0, pixel_stride = 0, left = 0, top = 0;
    UINT width = 0, height = 0, output_pitch = 0;
};
static_assert(sizeof(depth_conversion) == 7 * sizeof(UINT));

inline UINT depth_pixel_stride(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return 4;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return 8;
    default: return 0;
    }
}

inline bool depth_region(const tagged_resource &tag, const D3D12_RESOURCE_DESC &description,
                         depth_conversion &result)
{
    result = {};
    if (description.Width == 0 || description.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        description.Height == 0 || description.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) return false;
    result.left = tag.left;
    result.top = tag.top;
    result.width = tag.width;
    result.height = tag.height;
    if (tag.width == 0 && tag.height == 0 && tag.left == 0 && tag.top == 0)
    {
        result.width = static_cast<UINT>(description.Width);
        result.height = description.Height;
    }
    return result.width != 0 && result.height != 0 && result.left < description.Width &&
        result.top < description.Height && result.width <= description.Width - result.left &&
        result.height <= description.Height - result.top;
}

inline UINT64 depth_source_offset(const depth_conversion &layout, UINT column, UINT row)
{
    return UINT64(layout.top + row) * layout.source_pitch + UINT64(layout.left + column) * layout.pixel_stride;
}

inline bool depth_conversion_layout(const tagged_resource &tag, const plane &source,
                                    UINT output_pitch, depth_conversion &result)
{
    if (!depth_region(tag, source.description, result)) return false;
    const auto &footprint = source.footprint;
    const auto stride = depth_pixel_stride(footprint.Footprint.Format);
    const auto resource_stride = depth_pixel_stride(source.description.Format);
    if (stride == 0 || resource_stride == 0 || (resource_stride == 4 && stride != 4) || footprint.Offset != 0 ||
        footprint.Footprint.Width != source.description.Width ||
        footprint.Footprint.Height != source.description.Height || footprint.Footprint.Depth != 1 ||
        footprint.Footprint.RowPitch < source.description.Width * stride ||
        footprint.Footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0 ||
        output_pitch < result.width * sizeof(float) || output_pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0)
        return false;
    result.source_pitch = footprint.Footprint.RowPitch;
    result.pixel_stride = stride;
    result.output_pitch = output_pitch;
    const auto last = depth_source_offset(result, result.width - 1, result.height - 1) + sizeof(float);
    return last <= source.bytes && last <= UINT_MAX;
}
}
