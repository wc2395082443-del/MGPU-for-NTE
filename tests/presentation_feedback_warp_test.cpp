#include "../src/presentation_feedback.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;
static void require(HRESULT status) { if (FAILED(status)) throw status; }

int main()
{
    try
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> device;
        require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        ComPtr<ID3D12CommandQueue> queue;
        require(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = description.Height = 8;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> created;
        require(factory->CreateSwapChainForComposition(queue.Get(), &description, nullptr, &created));
        ComPtr<IDXGISwapChain2> chain;
        require(created.As(&chain));
        mgpu::presentation::feedback feedback;
        if (feedback.initialize(chain.Get(), false) != E_INVALIDARG || feedback.active()) return 1;
        require(feedback.initialize(chain.Get(), true));
        UINT maximum = 0;
        require(chain->GetMaximumFrameLatency(&maximum));
        if (maximum != 1 || !feedback.active()) return 2;
        const auto credit = feedback.acquire(0);
        if (credit != WAIT_OBJECT_0 && credit != WAIT_TIMEOUT) return 3;
        if (credit == WAIT_OBJECT_0 && feedback.acquire(0) != WAIT_OBJECT_0) return 4;
        require(chain->ResizeBuffers(2, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, description.Flags));
        require(feedback.initialize(chain.Get(), true));
        DXGI_FRAME_STATISTICS statistics{};
        mgpu::presentation::stamp matched{};
        const auto status = feedback.statistics(chain.Get(), statistics, matched);
        if (matched.seal != 0 || feedback.submissions() != 0) return 5;
        std::printf("PASS: WARP-only composition chain, explicit waitable ownership, max latency1, resize preserves flag; statistics=0x%08X, no display/NVIDIA inference test\n", unsigned(status));
        return 0;
    }
    catch (HRESULT status)
    {
        std::printf("FAIL: software presentation contract0x%08X\n", unsigned(status));
        return 1;
    }
}
