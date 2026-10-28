#include "../experimental/secondary_fg/dlss_fg_stage.hpp"
#include "../src/neural_snapshots.hpp"
#include <dxgi1_4.h>
#include <cstdio>
#include <map>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace mgpu::secondary_fg;

static unsigned checks = 0, evaluations = 0, expected_index = 1;
static std::uint64_t expected_frame = 0;
static bool bad_input = false;
static void require(bool condition) { ++checks; if (!condition) throw E_FAIL; }
static void require_hr(HRESULT status) { if (FAILED(status)) throw status; }

class parameters final : public NVSDK_NGX_Parameter
{
public:
    std::map<std::string, unsigned long long> numbers;
    std::map<std::string, float> floats;
    std::map<std::string, ID3D12Resource *> resources;
    std::map<std::string, void *> pointers;
    void Set(const char *key, unsigned long long value) override { numbers[key] = value; }
    void Set(const char *key, unsigned int value) override { numbers[key] = value; }
    void Set(const char *key, int value) override { numbers[key] = value; }
    void Set(const char *key, float value) override { floats[key] = value; }
    void Set(const char *, double) override {}
    void Set(const char *, ID3D11Resource *) override {}
    void Set(const char *key, ID3D12Resource *value) override { resources[key] = value; }
    void Set(const char *key, void *value) override { pointers[key] = value; }
    template <typename Value> NVSDK_NGX_Result number(const char *key, Value *value) const
    {
        const auto found = numbers.find(key);
        if (found == numbers.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *value = static_cast<Value>(found->second);
        return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char *key, unsigned long long *value) const override { return number(key, value); }
    NVSDK_NGX_Result Get(const char *key, unsigned int *value) const override { return number(key, value); }
    NVSDK_NGX_Result Get(const char *key, int *value) const override { return number(key, value); }
    NVSDK_NGX_Result Get(const char *, float *) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result Get(const char *, double *) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result Get(const char *, ID3D11Resource **) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result Get(const char *, ID3D12Resource **) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    NVSDK_NGX_Result Get(const char *, void **) const override { return NVSDK_NGX_Result_FAIL_InvalidParameter; }
    void Reset() override { numbers.clear(); floats.clear(); resources.clear(); pointers.clear(); }
};

static NVSDK_NGX_Result NVSDK_CONV allocate(NVSDK_NGX_Parameter **result)
{ *result = new parameters; return NVSDK_NGX_Result_Success; }
static NVSDK_NGX_Result NVSDK_CONV destroy(NVSDK_NGX_Parameter *value)
{ delete static_cast<parameters *>(value); return NVSDK_NGX_Result_Success; }
static NVSDK_NGX_Result NVSDK_CONV create(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **result)
{
    static std::uint64_t handle = 1;
    if (feature != NVSDK_NGX_Feature_FrameGeneration) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *result = reinterpret_cast<NVSDK_NGX_Handle *>(&handle);
    return NVSDK_NGX_Result_Success;
}
static NVSDK_NGX_Result NVSDK_CONV release(NVSDK_NGX_Handle *) { return NVSDK_NGX_Result_Success; }
static NVSDK_NGX_Result NVSDK_CONV evaluate(ID3D12GraphicsCommandList *commands, const NVSDK_NGX_Handle *,
    const NVSDK_NGX_Parameter *values, PFN_NVSDK_NGX_ProgressCallback_C)
{
    const auto &parameters = *static_cast<const class parameters *>(values);
    bad_input = bad_input || parameters.numbers.at(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID) != expected_frame ||
        parameters.numbers.at(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex) != expected_index++ ||
        parameters.numbers.at(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount) != 5u ||
        parameters.floats.at(NVSDK_NGX_DLSSG_Parameter_MvecScaleX) != 0.125f;
    auto *source = parameters.resources.at(NVSDK_NGX_DLSSG_Parameter_Backbuffer);
    auto *destination = parameters.resources.at(NVSDK_NGX_DLSSG_Parameter_OutputInterpolated);
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (auto &barrier : barriers) barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST};
    commands->ResourceBarrier(2, barriers);
    commands->CopyResource(destination, source);
    for (auto &barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands->ResourceBarrier(2, barriers);
    ++evaluations;
    return NVSDK_NGX_Result_Success;
}

struct unblock
{
    ComPtr<ID3D12Fence> fence;
    ~unblock() { if (fence) fence->Signal(1); }
};

int main()
{
    try
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> device;
        require_hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        require_hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        require_hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Fence> fence;
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        require_hr(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
        require_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        require_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
        require_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        const auto wait = [&](UINT64 value)
        {
            const auto deadline = GetTickCount64() + 5000;
            while (fence->GetCompletedValue() < value && GetTickCount64() < deadline) Sleep(1);
            require(fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= value);
        };
        const auto reset = [&] { require_hr(allocator->Reset()); require_hr(commands->Reset(allocator.Get(), nullptr)); };
        const auto submit = [&](UINT64 value)
        {
            require_hr(commands->Close());
            ID3D12CommandList *lists[]{commands.Get()};
            queue->ExecuteCommandLists(1, lists);
            require_hr(queue->Signal(fence.Get(), value));
        };
        parameters capability;
        capability.Set(NVSDK_NGX_Parameter_FrameGeneration_Available, 1);
        capability.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, 3u);
        ngx_api api{allocate, destroy, create, evaluate, release};
        configuration config;
        config.width = config.height = config.guide_width = config.guide_height = 8;
        config.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        config.adapter_luid = device->GetAdapterLuid();
        config.multiplier = 6;
        dlss_fg_stage stage;
        require(stage.create(device.Get(), commands.Get(), &capability, api, config) == NVSDK_NGX_Result_FAIL_FeatureNotSupported);
        require(!stage.recording() && !stage.live() && stage.maximum_generated_frames() == 3);
        capability.Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, 5u);
        require(stage.create(device.Get(), commands.Get(), &capability, api, config) == NVSDK_NGX_Result_Success);
        require(stage.maximum_generated_frames() == 5 && stage.capability_reported());
        submit(1);
        require(stage.track_submission(fence.Get(), 1));
        wait(1);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = 8;
        desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format = config.format;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto texture = [&]
        {
            ComPtr<ID3D12Resource> result;
            require_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&result)));
            return result;
        };
        auto color = texture(), depth = texture(), motion = texture();
        std::array<std::array<ComPtr<ID3D12Resource>, 5>, 3> generated;
        for (auto &slot : generated) for (auto &output : slot) output = texture();
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 16;
        desc.Height = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> hint;
        require_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&hint)));
        mgpu::neural_pipeline::snapshots snapshots;
        require_hr(snapshots.initialize(color.Get()));
        unblock display;
        require_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&display.fence)));
        snapshots.presented(0, display.fence.Get(), 1);
        for (unsigned epoch = 1; epoch <= 3; ++epoch)
        {
            reset();
            frame_resources frame;
            frame.contract.frame_id = frame.contract.depth_frame_id = frame.contract.motion_frame_id = epoch;
            frame.contract.capture_ns = 100000000ull + epoch * 30000000ull;
            frame.contract.neural_output = frame.contract.depth_valid = frame.contract.motion_valid = true;
            frame.contract.normalized_motion_scale_x = frame.contract.normalized_motion_scale_y = 0.125f;
            frame.neural_color = {color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            frame.depth = {depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            frame.motion = {motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            frame.disable_interpolation = {hint.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            const unsigned slot = epoch == 3 ? 0u : epoch;
            for (unsigned index = 0; index < 5; ++index)
                frame.interpolated[index] = {generated[slot][index].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            frame.generated_count = 5;
            decision decision;
            if (epoch == 2)
            {
                frame.contract.motion_frame_id = 1;
                require(NVSDK_NGX_FAILED(stage.record(commands.Get(), frame, decision)) && !stage.recording());
                frame.contract.motion_frame_id = epoch;
                frame.generated_count = 4;
                require(NVSDK_NGX_FAILED(stage.record(commands.Get(), frame, decision)) && !stage.recording());
                frame.generated_count = 5;
            }
            expected_frame = epoch;
            expected_index = 1;
            require(stage.record(commands.Get(), frame, decision) == NVSDK_NGX_Result_Success);
            require(!stage.complete(epoch, true));
            require_hr(snapshots.wait_for_read(queue.Get(), slot));
            submit(epoch + 1);
            require(stage.track_submission(fence.Get(), epoch + 1));
            if (epoch == 3)
            {
                Sleep(20);
                require(fence->GetCompletedValue() == epoch);
                require(!stage.complete(epoch, true));
                require_hr(display.fence->Signal(1));
            }
            wait(epoch + 1);
            require(stage.complete(epoch, true));
            if (epoch < 3) require(display.fence->GetCompletedValue() == 0);
        }
        require(evaluations == 15 && !bad_input);
        require(stage.release_completed());
        require(snapshots.reads_complete());
        snapshots.release();
        std::printf("PASS: %u software-D3D12 production-stage checks, 15 MOCK evaluations; no NVIDIA runtime loaded\n", checks);
        return 0;
    }
    catch (HRESULT status)
    {
        std::printf("FAIL: check=%u status=0x%08lx\n", checks, static_cast<unsigned long>(status));
        return 1;
    }
}
