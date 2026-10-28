#include "../src/depth_normalizer.hpp"

int main()
{
    using namespace mgpu::aligned;
    plane source{};
    source.description.Width = 2564;
    source.description.Height = 1444;
    source.description.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    source.footprint.Footprint = {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, 2564, 1444, 1, 20736};
    source.bytes = 20736ull * 1443 + 2564 * 8;
    tagged_resource region{};
    region.width = 2562;
    region.height = 1441;
    depth_conversion layout{};
    if (!depth_conversion_layout(region, source, 10496, layout)) return 1;
    if (layout.pixel_stride != 8 || layout.width != 2562 || layout.height != 1441 ||
        depth_source_offset(layout, 2561, 1440) != 1440ull * 20736 + 2561 * 8) return 2;
    region.left = 2;
    region.top = 3;
    if (!depth_conversion_layout(region, source, 10496, layout)) return 3;
    region.left = 3;
    if (depth_conversion_layout(region, source, 10496, layout)) return 4;
    region.left = 2;
    source.bytes -= 8;
    if (depth_conversion_layout(region, source, 10496, layout)) return 5;
    source.bytes += 8;
    source.footprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    source.footprint.Footprint.RowPitch = 10496;
    if (!depth_conversion_layout(region, source, 10496, layout) || layout.pixel_stride != 4) return 6;
    source.description.Format = DXGI_FORMAT_R32_FLOAT;
    if (!depth_conversion_layout(region, source, 10496, layout) || layout.pixel_stride != 4) return 7;
    source.footprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    if (depth_conversion_layout(region, source, 10496, layout)) return 11;
    source.footprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    if (depth_pixel_stride(DXGI_FORMAT_R24G8_TYPELESS) != 0) return 8;
    region.width = UINT_MAX;
    if (depth_conversion_layout(region, source, 10496, layout)) return 9;
    Microsoft::WRL::ComPtr<ID3DBlob> shader;
    return SUCCEEDED(compile_depth_shader(&shader)) && shader != nullptr ? 0 : 10;
}
