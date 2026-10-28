#include "../src/aligned_texture_policy.hpp"

int main()
{
    using namespace mgpu::aligned;
    tagged_resource tag{};
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = 2562;
    texture.Height = 1441;
    texture.DepthOrArraySize = texture.MipLevels = texture.SampleDesc.Count = 1;
    texture.Format = DXGI_FORMAT_R32_TYPELESS;
    if (texture_rejection(tag, texture) != nullptr) return 1;
    tag.width = 2562;
    tag.height = 1441;
    texture.MipLevels = 12;
    if (texture_rejection(tag, texture) != nullptr) return 2;
    tag.left = 1;
    if (texture_rejection(tag, texture) == nullptr) return 3;
    tag.left = 0;
    tag.width = 2560;
    if (texture_rejection(tag, texture) == nullptr) return 4;
    texture.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    if (texture_rejection(tag, texture, true) != nullptr) return 10;
    tag.width = 2563;
    if (texture_rejection(tag, texture, true) == nullptr) return 11;
    tag.width = tag.height = 0;
    texture.SampleDesc.Count = 4;
    if (texture_rejection(tag, texture) == nullptr) return 5;
    texture.SampleDesc.Count = 1;
    texture.DepthOrArraySize = 2;
    if (texture_rejection(tag, texture) == nullptr) return 6;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 0;
    if (texture_rejection(tag, texture) == nullptr) return 7;
    texture.MipLevels = 1;
    texture.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    if (texture_rejection(tag, texture) == nullptr) return 8;
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = 0x100000000ull + 2562;
    return texture_rejection(tag, texture) != nullptr ? 0 : 9;
}
