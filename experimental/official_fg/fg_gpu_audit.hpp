#pragma once

#include <windows.h>
#include <d3d12.h>
#include <filesystem>
#include <cstdint>

namespace mgpu::official_fg::gpu_audit
{
void install(const std::filesystem::path &runtime, const std::filesystem::path &logs);
void attach_device(ID3D12Device *device);
void poll();
void stop();
std::uint64_t begin(ID3D12GraphicsCommandList *commands, std::uint64_t frame,
    unsigned count, unsigned index, unsigned parameter_mask);
void end(std::uint64_t ticket, ID3D12GraphicsCommandList *commands, unsigned result);
void present(std::uint64_t sequence, std::uint32_t source);
void finish_present();
void start_test_capture();
unsigned completed_records();
#ifdef MGPU_FG_AUDIT_FOCUS_STUB
void test_focus_window(HWND window);
#endif
}
