#pragma once

#include "official_metrics.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace mgpu::official_fg
{
inline std::array<std::wstring, 2> output_labels(const rate_snapshot &sample,
    std::uint64_t now, unsigned requested)
{
    wchar_t rate[96]{}, factor[96]{};
    if (sample.visible && fresh_metrics(sample, now, 2500)) {
        swprintf_s(rate, L"NR %.1f FPS  SDK OUT %.1f FPS", sample.real_fps, sample.sdk_fps);
        swprintf_s(factor, L"OBSERVED %.2fx   REQUESTED %ux", sample.multiplier, requested);
    } else {
        wcscpy_s(rate, L"NR -- FPS   SDK OUT -- FPS");
        swprintf_s(factor, L"MEASURING   REQUESTED %ux", requested);
    }
    return {rate, factor};
}

inline unsigned osd_pixel_bytes(DXGI_FORMAT format)
{
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) return 8;
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_R10G10B10A2_UNORM) return 4;
    throw std::runtime_error("unsupported OSD format");
}

inline void osd_pixel(void *destination, DXGI_FORMAT format, bool white)
{
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const std::uint16_t channel = white ? 0x3c00 : 0;
        const std::array<std::uint16_t, 4> pixel{channel, channel, channel, 0x3c00};
        std::memcpy(destination, pixel.data(), sizeof(pixel));
    } else {
        const std::uint32_t pixel = white ? 0xffffffffu :
            (format == DXGI_FORMAT_R10G10B10A2_UNORM ? 0xc0000000u : 0xff000000u);
        std::memcpy(destination, &pixel, sizeof(pixel));
    }
}

class output_osd
{
    using resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct upload_slot
    {
        resource texture;
        std::byte *mapped = nullptr;
        std::uint64_t generation = 0;
        ~upload_slot() { if (texture && mapped) texture->Unmap(0, nullptr); }
    };
public:
    output_osd(ID3D12Device *device, unsigned width, unsigned height, DXGI_FORMAT format)
        : format_(format), bytes_(osd_pixel_bytes(format)), x_(std::min(16u, width / 16)),
          y_(std::min(16u, height / 16)), width_(std::min(640u, width - x_)),
          height_(std::min(76u, height - y_))
    {
        if (!width_ || !height_) throw std::runtime_error("empty OSD extent");
        D3D12_RESOURCE_DESC text{};
        text.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        text.Width = width_;
        text.Height = height_;
        text.DepthOrArraySize = text.MipLevels = 1;
        text.SampleDesc.Count = 1;
        text.Format = format_;
        UINT64 size = 0;
        device->GetCopyableFootprints(&text, 0, 1, 0, &text_layout_, nullptr, nullptr, &size);
        for (auto &slot : slots_) {
            slot.texture = upload(device, size);
            D3D12_RANGE range{0, 0};
            checked(slot.texture->Map(0, &range, reinterpret_cast<void **>(&slot.mapped)));
        }
        auto mask = text;
        mask.Width = width;
        mask.Height = height;
        mask.Format = DXGI_FORMAT_R8_UNORM;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &mask,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mask_)));
        device->GetCopyableFootprints(&mask, 0, 1, 0, &mask_layout_, nullptr, nullptr, &size);
        mask_upload_ = upload(device, size);
        void *mapped = nullptr;
        D3D12_RANGE range{0, 0};
        checked(mask_upload_->Map(0, &range, &mapped));
        std::memset(mapped, 0, static_cast<size_t>(size));
        for (unsigned row = y_; row < y_ + height_; ++row)
            std::memset(static_cast<std::byte *>(mapped) + mask_layout_.Offset +
                row * mask_layout_.Footprint.RowPitch + x_, 255, width_);
        mask_upload_->Unmap(0, nullptr);
        pixels_.resize(static_cast<size_t>(width_) * height_);
    }
    void record(ID3D12GraphicsCommandList *commands, ID3D12Resource *backbuffer, unsigned slot_index,
        const rate_snapshot &sample, std::uint64_t now, unsigned requested)
    {
        const auto labels = output_labels(sample, now, requested);
        if (labels != labels_) {
            if (!raster(labels)) {
                std::fill(pixels_.begin(), pixels_.end(), 0);
                if (!raster_failed_) std::printf("[M352-OSD] GDI text unavailable; NR/FG unchanged\n");
                raster_failed_ = true;
            }
            labels_ = labels;
            ++generation_;
        }
        auto &slot = slots_.at(slot_index);
        if (slot.generation != generation_) {
            for (unsigned row = 0; row < height_; ++row)
                for (unsigned column = 0; column < width_; ++column)
                    osd_pixel(slot.mapped + text_layout_.Offset + row * text_layout_.Footprint.RowPitch + column * bytes_,
                        format_, (pixels_[row * width_ + column] & 0x00ffffffu) != 0);
            slot.generation = generation_;
        }
        if (!mask_initialized_) {
            copy(commands, mask_.Get(), mask_upload_.Get(), mask_layout_, 0, 0);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {mask_.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            commands->ResourceBarrier(1, &barrier);
            mask_initialized_ = true;
        }
        copy(commands, backbuffer, slot.texture.Get(), text_layout_, x_, y_);
    }
    ID3D12Resource *mask() const { return mask_.Get(); }
    unsigned left() const { return x_; }
    unsigned top() const { return y_; }
    unsigned width() const { return width_; }
    unsigned height() const { return height_; }
private:
    static void checked(HRESULT result)
    {
        if (FAILED(result)) throw std::runtime_error("output OSD resource failure");
    }
    static resource upload(ID3D12Device *device, UINT64 bytes)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resource result;
        checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&result)));
        return result;
    }
    static void copy(ID3D12GraphicsCommandList *commands, ID3D12Resource *target, ID3D12Resource *upload,
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &layout, unsigned left, unsigned top)
    {
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = upload;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = layout;
        destination.pResource = target;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&destination, left, top, 0, &source, nullptr);
    }
    bool raster(const std::array<std::wstring, 2> &labels)
    {
        struct drawing
        {
            HDC context = CreateCompatibleDC(nullptr);
            HBITMAP bitmap = nullptr;
            HFONT font = nullptr;
            HGDIOBJ old_bitmap = nullptr, old_font = nullptr;
            ~drawing()
            {
                if (old_font) SelectObject(context, old_font);
                if (old_bitmap) SelectObject(context, old_bitmap);
                if (font) DeleteObject(font);
                if (bitmap) DeleteObject(bitmap);
                if (context) DeleteDC(context);
            }
        } draw;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width_;
        info.bmiHeader.biHeight = -static_cast<LONG>(height_);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        draw.bitmap = CreateDIBSection(draw.context, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        const int font_height = static_cast<int>(std::min(28u, std::max(8u, width_ / 23)));
        draw.font = CreateFontW(-font_height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
        if (!draw.context || !draw.bitmap || !draw.font) return false;
        draw.old_bitmap = SelectObject(draw.context, draw.bitmap);
        draw.old_font = SelectObject(draw.context, draw.font);
        if (!PatBlt(draw.context, 0, 0, width_, height_, BLACKNESS)) return false;
        SetBkMode(draw.context, TRANSPARENT);
        SetTextColor(draw.context, RGB(255, 255, 255));
        if (!TextOutW(draw.context, 8, 4, labels[0].c_str(), static_cast<int>(labels[0].size())) ||
            !TextOutW(draw.context, 8, 8 + font_height, labels[1].c_str(), static_cast<int>(labels[1].size())) ||
            !GdiFlush()) return false;
        std::memcpy(pixels_.data(), bits, pixels_.size() * sizeof(std::uint32_t));
        return true;
    }
    DXGI_FORMAT format_;
    unsigned bytes_, x_, y_, width_, height_;
    std::array<upload_slot, 3> slots_;
    resource mask_, mask_upload_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT text_layout_{}, mask_layout_{};
    bool mask_initialized_ = false;
    bool raster_failed_ = false;
    std::uint64_t generation_ = 0;
    std::vector<std::uint32_t> pixels_;
    std::array<std::wstring, 2> labels_;
};
}
