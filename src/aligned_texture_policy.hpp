#pragma once

#include "aligned_input.hpp"
#include "tagged_motion_policy.hpp"
#include "depth_plane_policy.hpp"

namespace mgpu::aligned
{
inline const char *texture_rejection(const tagged_resource &tag, const D3D12_RESOURCE_DESC &description,
                                     bool depth = false)
{
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return "not Texture2D";
    if (description.DepthOrArraySize != 1) return "texture array is unsupported";
    if (description.MipLevels == 0) return "missing mip levels";
    if (description.SampleDesc.Count != 1) return "multisampled texture is unsupported";
    if (description.Width == 0 || description.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        description.Height == 0 || description.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return "invalid texture dimensions";
    if (depth)
    {
        depth_conversion region{};
        if (depth_pixel_stride(description.Format) == 0) return "unsupported depth plane encoding";
        if (!depth_region(tag, description, region)) return "depth region outside base mip";
    }
    else if (!mgpu::tagged_motion::whole_extent(tag.top, tag.left, tag.width, tag.height,
                                          static_cast<unsigned>(description.Width), description.Height))
        return "tag extent does not cover base mip";
    return nullptr;
}
}
