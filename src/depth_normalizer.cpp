#include "depth_normalizer.hpp"
#include "queue_dependency_trace.hpp"
#include <cstring>

namespace mgpu::aligned
{
namespace
{
constexpr char depth_shader[] = R"(
ByteAddressBuffer sourceDepth : register(t0);
RWByteAddressBuffer outputDepth : register(u0);
cbuffer Layout : register(b0)
{
    uint sourcePitch, pixelStride, left, top, width, height, outputPitch;
};
[numthreads(16, 16, 1)]
void main(uint3 position : SV_DispatchThreadID)
{
    if (position.x >= width || position.y >= height) return;
    uint inputOffset = (top + position.y) * sourcePitch + (left + position.x) * pixelStride;
    uint outputOffset = position.y * outputPitch + position.x * 4;
    outputDepth.Store(outputOffset, sourceDepth.Load(inputOffset));
}
)";

D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                   D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}
}

HRESULT compile_depth_shader(ID3DBlob **shader)
{
    *shader = nullptr;
    static HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (compiler == nullptr) return HRESULT_FROM_WIN32(GetLastError());
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile"));
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = compile != nullptr
        ? compile(depth_shader, std::strlen(depth_shader), "aligned_depth", nullptr, nullptr, "main", "cs_5_0",
                  D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, shader, &errors)
        : E_NOINTERFACE;
    return result;
}

HRESULT depth_normalizer::initialize(ID3D12Device *device)
{
    if (device_ != nullptr) return S_OK;
    Microsoft::WRL::ComPtr<ID3DBlob> shader, serialized, errors;
    HRESULT result = compile_depth_shader(&shader);
    if (FAILED(result)) return result;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = sizeof(depth_conversion) / sizeof(UINT);
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 3;
    signature.pParameters = parameters;
    result = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(result)) return result;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    result = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root));
    if (FAILED(result)) return result;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    result = device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(result)) return result;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    result = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(result)) return result;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result)) return result;
    root_ = root;
    pipeline_ = pipeline;
    queue_ = queue;
    fence_ = fence;
    device_ = device;
    mgpu::queue_trace::observe_queue(queue_.Get());
    mgpu::queue_trace::observe_fence(fence_.Get());
    return S_OK;
}

bool depth_normalizer::complete(const depth_job &job) const
{
    if (job.value == 0) return true;
    if (fence_ == nullptr || job.value == UINT64_MAX) return false;
    const auto value = fence_->GetCompletedValue();
    return value != UINT64_MAX && value >= job.value;
}

HRESULT depth_normalizer::submit(const plane &source, const tagged_resource &region, plane &output,
                                 depth_job &job, std::uint64_t serial, ID3D12Fence *gate, UINT64 gate_value)
{
    if (!complete(job) || source.buffer == nullptr || serial == 0) return E_INVALIDARG;
    if ((gate == nullptr) != (gate_value == 0) || gate_value == UINT64_MAX) return E_INVALIDARG;
    depth_conversion layout{};
    if (!depth_region(region, source.description, layout)) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    HRESULT result = source.buffer->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(result)) return result;
    result = initialize(device.Get());
    if (FAILED(result)) return result;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = layout.width;
    description.Height = layout.height;
    description.DepthOrArraySize = description.MipLevels = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_R32_FLOAT;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device_->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    if (bytes == 0 || bytes == UINT64_MAX || bytes > 128ull * 1024 * 1024 ||
        !depth_conversion_layout(region, source, footprint.Footprint.RowPitch, layout)) return E_INVALIDARG;
    if (output.buffer == nullptr || output.bytes != bytes)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Microsoft::WRL::ComPtr<ID3D12Resource> created;
        result = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&created));
        if (FAILED(result)) return result;
        if (output.buffer != nullptr) output.buffer->Release();
        output.buffer = created.Detach();
    }
    output.description = description;
    output.footprint = footprint;
    output.bytes = bytes;
    if (job.allocator == nullptr)
    {
        result = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&job.allocator));
        if (FAILED(result)) return result;
    }
    result = job.allocator->Reset();
    if (FAILED(result)) return result;
    if (job.command == nullptr)
        result = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, job.allocator.Get(),
                                            pipeline_.Get(), IID_PPV_ARGS(&job.command));
    else result = job.command->Reset(job.allocator.Get(), pipeline_.Get());
    if (FAILED(result)) return result;
    job.timing.begin(device_.Get(), job.command.Get(), serial);
    D3D12_RESOURCE_BARRIER barriers[] = {
        transition(source.buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(output.buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    job.command->ResourceBarrier(2, barriers);
    job.command->SetComputeRootSignature(root_.Get());
    job.command->SetComputeRoot32BitConstants(0, sizeof layout / sizeof(UINT), &layout, 0);
    job.command->SetComputeRootShaderResourceView(1, source.buffer->GetGPUVirtualAddress());
    job.command->SetComputeRootUnorderedAccessView(2, output.buffer->GetGPUVirtualAddress());
    job.command->Dispatch((layout.width + 15) / 16, (layout.height + 15) / 16, 1);
    barriers[0] = transition(source.buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    barriers[1] = transition(output.buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    job.command->ResourceBarrier(2, barriers);
    job.timing.end(job.command.Get());
    result = job.command->Close();
    if (FAILED(result)) return result;
    if (gate != nullptr)
    {
        result = queue_->Wait(gate, gate_value);
        if (FAILED(result)) return result;
    }
    ID3D12CommandList *commands[] = {job.command.Get()};
    job.value = UINT64_MAX;
    const auto submitted_at = timing::cpu_ticks();
    queue_->ExecuteCommandLists(1, commands);
    result = queue_->Signal(fence_.Get(), ++next_);
    if (FAILED(result)) return result;
    job.value = next_;
    job.serial = serial;
    job.timing.submitted(queue_.Get(), submitted_at, &clock_);
    return S_OK;
}
}
