#include "output_osd.hpp"
#include <dxgi1_6.h>
#include <fstream>
#include <cstdio>

using namespace mgpu::official_fg;
using Microsoft::WRL::ComPtr;

namespace
{
void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
void checked(HRESULT result) { check(SUCCEEDED(result), "D3D12 WARP failure"); }
void transition(ID3D12GraphicsCommandList *commands, ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    commands->ResourceBarrier(1, &barrier);
}
void preview(const std::vector<std::uint32_t> &pixels, unsigned width, unsigned height)
{
    BITMAPFILEHEADER header{};
    BITMAPINFOHEADER info{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size() * 4);
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::ofstream file("output-osd-warp.bmp", std::ios::binary);
    file.write(reinterpret_cast<const char *>(&header), sizeof(header));
    file.write(reinterpret_cast<const char *>(&info), sizeof(info));
    file.write(reinterpret_cast<const char *>(pixels.data()), pixels.size() * 4);
}
void test_format(ID3D12Device *device, DXGI_FORMAT format)
{
    constexpr unsigned width = 800, height = 180;
    output_osd osd(device, width, height, format);
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_description{};
    checked(device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = description.MipLevels = description.SampleDesc.Count = 1;
    description.Format = format;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> target;
    checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
    D3D12_DESCRIPTOR_HEAP_DESC descriptor{};
    descriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    descriptor.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv;
    checked(device->CreateDescriptorHeap(&descriptor, IID_PPV_ARGS(&rtv)));
    device->CreateRenderTargetView(target.Get(), nullptr, rtv->GetCPUDescriptorHandleForHeapStart());
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_layout{}, mask_layout{};
    UINT64 color_size = 0, mask_size = 0;
    device->GetCopyableFootprints(&description, 0, 1, 0, &color_layout, nullptr, nullptr, &color_size);
    auto mask_description = osd.mask()->GetDesc();
    device->GetCopyableFootprints(&mask_description, 0, 1, color_size, &mask_layout, nullptr, nullptr, &mask_size);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = color_size + mask_size + 512;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    ComPtr<ID3D12Fence> fence;
    checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    struct event_owner { HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr); ~event_owner() { CloseHandle(value); } } event;
    check(event.value != nullptr, "fence event");
    const auto bytes = osd_pixel_bytes(format);
    std::vector<std::byte> previous;
    for (unsigned frame = 0; frame < 7; ++frame) {
        if (frame) {
            checked(allocator->Reset());
            checked(commands->Reset(allocator.Get(), nullptr));
            transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        const float black[4]{0, 0, 0, 1};
        commands->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(), black, 0, nullptr);
        transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        rate_snapshot sample{};
        sample.end_ms = 2000;
        sample.visible = sample.valid = true;
        sample.real_fps = 33.1;
        sample.sdk_fps = 194.8;
        sample.multiplier = 5.89;
        osd.record(commands.Get(), target.Get(), frame % 3, sample, frame == 6 ? 6000 : 2100, 6);
        transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(commands.Get(), osd.mask(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const auto copy = [&](ID3D12Resource *source, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &layout) {
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = source;
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource = readback.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint = layout;
            commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        };
        copy(target.Get(), color_layout);
        copy(osd.mask(), mask_layout);
        transition(commands.Get(), osd.mask(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        checked(commands->Close());
        ID3D12CommandList *lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        checked(queue->Signal(fence.Get(), frame + 1));
        checked(fence->SetEventOnCompletion(frame + 1, event.value));
        check(WaitForSingleObject(event.value, 3000) == WAIT_OBJECT_0, "WARP fence timeout");
        void *mapped = nullptr;
        checked(readback->Map(0, nullptr, &mapped));
        const auto *data = static_cast<const std::byte *>(mapped);
        std::vector<std::uint32_t> image(width * height);
        unsigned whites = 0;
        std::array<std::byte, 8> white{}, black_pixel{};
        osd_pixel(white.data(), format, true);
        osd_pixel(black_pixel.data(), format, false);
        for (unsigned row = 0; row < height; ++row) {
            for (unsigned column = 0; column < width; ++column) {
                const bool inside = column >= osd.left() && column < osd.left() + osd.width() &&
                    row >= osd.top() && row < osd.top() + osd.height();
                const auto mask = data[mask_layout.Offset + row * mask_layout.Footprint.RowPitch + column];
                check(mask == (inside ? std::byte{255} : std::byte{0}), "OSD alpha mask footprint mismatch");
                const auto *pixel = data + color_layout.Offset + row * color_layout.Footprint.RowPitch + column * bytes;
                const bool lit = std::memcmp(pixel, white.data(), bytes) == 0;
                check(lit || std::memcmp(pixel, black_pixel.data(), bytes) == 0, "unsupported/nonopaque OSD pixel");
                check(inside || !lit, "OSD modified pixels outside its rectangle");
                whites += lit ? 1 : 0;
                image[row * width + column] = lit ? 0xffffffffu : 0xff000000u;
            }
        }
        check(whites > 1000, "empty/unreadable OSD text");
        if (frame == 0) previous.assign(data, data + color_size);
        if (frame > 0 && frame < 6) check(std::memcmp(previous.data(), data, color_size) == 0, "slot reuse changed constant text");
        if (frame == 6) check(std::memcmp(previous.data(), data, color_size) != 0, "stale FPS remained onscreen");
        if (format == DXGI_FORMAT_R10G10B10A2_UNORM && frame == 0) preview(image, width, height);
        D3D12_RANGE no_write{0, 0};
        readback->Unmap(0, &no_write);
    }
}
}

int main()
{
    try {
        rate_snapshot sample{};
        sample.valid = sample.visible = true;
        sample.end_ms = 1000;
        sample.real_fps = 33;
        sample.sdk_fps = 165;
        sample.multiplier = 5;
        const auto labels = output_labels(sample, 1100, 6);
        check(labels[0].find(L"165.0") != std::wstring::npos && labels[0].find(L"SDK OUT") != std::wstring::npos,
            "OSD must use measured SDK rate, not requested factor");
        check(labels[1].find(L"5.00x") != std::wstring::npos, "OSD observed multiplier");
        check(output_labels(sample, 4000, 6)[0].find(L"--") != std::wstring::npos, "stale OSD invalidation");
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> device;
        checked(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        checked(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        checked(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
        for (auto format : {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT}) test_format(device.Get(), format);
        std::puts("WARP four formats x seven frames: text, alpha, bounds, slot reuse, stale rates PASS");
        return 0;
    } catch (const std::exception &error) {
        std::printf("FAIL %s\n", error.what());
        return 1;
    }
}
