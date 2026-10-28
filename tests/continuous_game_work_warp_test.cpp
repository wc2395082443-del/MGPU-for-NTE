#include "../src/game_work_trace.hpp"
#include "../src/nonblocking_work_trace.hpp"
#include "../src/log_queue.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace { std::vector<std::string> lines; std::mutex line_mutex; }
namespace mgpu::lq
{
bool running() { return true; }
void post(dest, const char *line) { std::lock_guard<std::mutex> lock(line_mutex); lines.emplace_back(line); }
}

int main()
{
    using Microsoft::WRL::ComPtr;
    const auto require = [](HRESULT result) { if (FAILED(result)) throw std::runtime_error("WARP API failure"); };
    try
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> device;
        require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC description{};
        ComPtr<ID3D12CommandQueue> queue;
        require(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12Fence> fence;
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!completed) throw std::runtime_error("fence event");
        constexpr unsigned batch_size = 64, batches = 192;
        std::vector<ComPtr<ID3D12CommandAllocator>> allocators(batch_size);
        std::vector<ComPtr<ID3D12GraphicsCommandList>> commands(batch_size);
        std::vector<ID3D12CommandList *> lists(batch_size);
        for (unsigned index = 0; index < batch_size; ++index)
        {
            require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[index])));
            require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[index].Get(), nullptr, IID_PPV_ARGS(&commands[index])));
            require(commands[index]->Close());
            lists[index] = commands[index].Get();
        }
        mgpu::game_work::bind_device(device.Get());
        mgpu::game_work::poll(712, true);
        const auto ready_deadline = GetTickCount64() + 10000;
        while (!mgpu::game_work::active() && GetTickCount64() < ready_deadline && !mgpu::game_work::nonblocking::finished()) Sleep(1);
        if (!mgpu::game_work::active())
        {
            for (const auto &line : lines) std::puts(line.c_str());
            throw std::runtime_error("continuous recording inactive at start");
        }
        mgpu::game_work::observe_native_list(commands[0].Get());
        for (unsigned batch = 0; batch < batches; ++batch)
        {
            for (unsigned index = 0; index < batch_size; ++index)
            {
                require(allocators[index]->Reset());
                require(commands[index]->Reset(allocators[index].Get(), nullptr));
                mgpu::game_work::reset(commands[index].Get());
                mgpu::game_work::tag(commands[index].Get(), batch * batch_size + index + 1, 1, 53, 1280, 720);
                mgpu::game_work::marker(commands[index].Get(), mgpu::game_work::phase::graphics);
                require(commands[index]->Close());
            }
            mgpu::game_work::before_submit(queue.Get(), batch_size, lists.data());
            queue->ExecuteCommandLists(batch_size, lists.data());
            mgpu::game_work::after_submit(queue.Get(), batch_size, lists.data());
            require(queue->Signal(fence.Get(), batch + 1));
            require(fence->SetEventOnCompletion(batch + 1, completed));
            if (WaitForSingleObject(completed, 5000) != WAIT_OBJECT_0) throw std::runtime_error("GPU fence timed out");
            mgpu::game_work::poll(712, true);
            if (!mgpu::game_work::active()) throw std::runtime_error("continuous native recording stopped early");
        }
        for (unsigned batch = 0; batch < batches; ++batch)
        {
            for (unsigned index = 0; index < batch_size; ++index)
            {
                require(commands[index]->Reset(allocators[index].Get(), nullptr));
                require(commands[index]->Close());
            }
            mgpu::game_work::poll(712, true);
            if (!mgpu::game_work::active()) throw std::runtime_error("cancelled command churn stopped recording");
            if (batch % 2 == 0)
                for (unsigned index = 0; index < batch_size; ++index)
                    mgpu::game_work::destroy(commands[index].Get());
        }
        for (unsigned index = 0; index < batch_size; ++index)
        {
            require(commands[index]->Reset(allocators[index].Get(), nullptr));
            mgpu::game_work::tag(commands[index].Get(), batch_size * batches + index + 1, 1, 53, 1280, 720);
            require(commands[index]->Close());
        }
        mgpu::game_work::before_submit(queue.Get(), batch_size, lists.data());
        queue->ExecuteCommandLists(batch_size, lists.data());
        mgpu::game_work::after_submit(queue.Get(), batch_size, lists.data());
        require(queue->Signal(fence.Get(), batches + 1));
        require(fence->SetEventOnCompletion(batches + 1, completed));
        if (WaitForSingleObject(completed, 5000) != WAIT_OBJECT_0) throw std::runtime_error("post-cancellation GPU fence timed out");
        mgpu::game_work::poll(712, true);
        const auto deadline = GetTickCount64() + 10000;
        while (!mgpu::game_work::nonblocking::drained() && GetTickCount64() < deadline) { mgpu::game_work::poll(0, true); Sleep(2); }
        mgpu::game_work::poll(0, true);
        if (!mgpu::game_work::nonblocking::drained()) throw std::runtime_error("asynchronous recording did not drain");
        CloseHandle(completed);
        std::set<unsigned long long> identities;
        unsigned jobs = 0;
        bool recycled = false, drained = false;
        for (const auto &line : lines)
        {
            if (line.find("record_budget_exhausted") != std::string::npos) throw std::runtime_error("old lifetime job budget exhausted");
            unsigned long long session = 0, job = 0;
            if (std::sscanf(line.c_str(), "[MGPU][M337] kind=job session=%llu job=%llu", &session, &job) == 2)
            {
                if (!identities.insert(job).second) throw std::runtime_error("GPU query generation identity reused");
                ++jobs;
            }
            unsigned long long reuse_count = 0;
            const auto *reuse_field = std::strstr(line.c_str(), " recycled=");
            if (reuse_field && std::sscanf(reuse_field, " recycled=%llu", &reuse_count) == 1)
                recycled |= reuse_count >= 4096;
            drained |= line.find("reason=submitted_jobs_drained") != std::string::npos;
        }
        if (jobs != batch_size * (batches + 1) || !recycled || !drained)
        {
            std::printf("jobs=%u recycled=%u drained=%u\n", jobs, unsigned(recycled), unsigned(drained));
            throw std::runtime_error("pool reuse/drain/count contract");
        }
        std::printf("PASS: %u WARP submissions plus 12288 unsubmitted closed cancellations, duplicate reset callbacks, bounded pool and unique query generations\n", jobs);
        return 0;
    }
    catch (const std::exception &error)
    {
        const auto metrics = mgpu::game_work::nonblocking::snapshot();
        std::printf("FAIL %s created=%llu emitted=%llu cancelled=%llu invalid=%llu rejected=%llu live=%u drain_timeouts=%llu\n", error.what(),
            metrics.created, metrics.emitted, metrics.cancelled, metrics.invalid, metrics.rejected, metrics.live, metrics.drain_timeouts);
        for (const auto &line : lines) if (line.find("kind=status") != std::string::npos) std::puts(line.c_str());
        return 1;
    }
}
