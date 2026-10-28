#include "../src/game_work_trace.hpp"
#include "../src/nonblocking_work_trace.hpp"
#include "../src/log_queue.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::mutex lines_mutex;
std::vector<std::string> lines;
void require(HRESULT result) { if (FAILED(result)) throw std::runtime_error("WARP API failure"); }
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
}
namespace mgpu::lq
{
bool running() { return true; }
void post(dest, const char *line) { std::lock_guard<std::mutex> lock(lines_mutex); lines.emplace_back(line); }
}

int main(int count, char **arguments)
{
    try
    {
        check(count == 2, "boundary scenario required");
        const std::string scenario = arguments[1];
        using Microsoft::WRL::ComPtr;
        using namespace mgpu::game_work;
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> device;
        require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command;
        require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&command)));
        require(command->Close());
        ComPtr<ID3D12Fence> gate, completed;
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completed)));
        std::vector<ComPtr<ID3D12CommandQueue>> queues(scenario == "queue-limit" ? 33 : 1);
        D3D12_COMMAND_QUEUE_DESC description{};
        for (auto &queue : queues) require(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue)));
        bind_device(device.Get());
        poll(361, true);
        const auto ready_deadline = GetTickCount64() + 10000;
        while (!active() && !nonblocking::finished() && GetTickCount64() < ready_deadline) Sleep(1);
        check(active(), "background initialization failed");
        observe_native_list(command.Get());
        const auto record = [&] {
            require(command->Reset(allocator.Get(), nullptr));
            tag(command.Get(), 1, 1, 53, 1280, 720);
            require(command->Close());
        };
        UINT64 fence_value = 0;
        const auto wait_gpu = [&](ID3D12CommandQueue *queue) {
            require(queue->Signal(completed.Get(), ++fence_value));
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            require(completed->SetEventOnCompletion(fence_value, event));
            const auto result = WaitForSingleObject(event, 5000);
            CloseHandle(event);
            check(result == WAIT_OBJECT_0, "real GPU work did not complete");
        };
        const auto submit = [&](ID3D12CommandQueue *queue) {
            ID3D12CommandList *list = command.Get();
            before_submit(queue, 1, &list);
            queue->ExecuteCommandLists(1, &list);
            after_submit(queue, 1, &list);
        };
        if (scenario == "unsubmitted") record();
        else if (scenario == "replay")
        {
            record();
            submit(queues[0].Get());
            wait_gpu(queues[0].Get());
            const auto deadline = GetTickCount64() + 3000;
            while (!nonblocking::snapshot().emitted && GetTickCount64() < deadline) Sleep(1);
            submit(queues[0].Get());
            wait_gpu(queues[0].Get());
        }
        else if (scenario == "queue-limit")
        {
            for (auto &queue : queues)
            {
                record();
                submit(queue.Get());
                wait_gpu(queue.Get());
            }
        }
        else if (scenario == "delayed-retirement")
        {
            record();
            require(queues[0]->Wait(gate.Get(), 1));
            submit(queues[0].Get());
            Sleep(100);
            check(nonblocking::snapshot().emitted == 0, "query results read before GPU retirement");
            destroy(command.Get());
            Sleep(30);
            check(nonblocking::snapshot().live == 1, "submitted query storage recycled before GPU retirement");
            require(gate->Signal(1));
            wait_gpu(queues[0].Get());
        }
        else if (scenario == "stop-during-scope")
        {
            require(command->Reset(allocator.Get(), nullptr));
            marker(command.Get(), phase::ngx_begin);
            poll(0, true);
            marker(command.Get(), phase::ngx_end);
            require(command->Close());
            submit(queues[0].Get());
            wait_gpu(queues[0].Get());
        }
        else throw std::runtime_error("unknown boundary scenario");
        poll(0, true);
        const auto deadline = GetTickCount64() + 8000;
        while (!nonblocking::finished() && GetTickCount64() < deadline) { poll(0, true); Sleep(1); }
        check(nonblocking::finished(), "background stop was not bounded");
        poll(0, true);
        const auto metrics = nonblocking::snapshot();
        if (scenario == "unsubmitted")
            check(!nonblocking::drained() && metrics.drain_timeouts == 1 && metrics.live == 1, "unsubmitted work falsely declared complete or reused");
        else if (scenario == "replay") check(!nonblocking::drained() && metrics.replayed == 1, "replay not explicitly rejected");
        else if (scenario == "queue-limit") check(!nonblocking::drained() && metrics.invalid == 1, "unknown queue not explicitly rejected");
        else check(nonblocking::drained() && metrics.emitted == 1 && !metrics.invalid && !metrics.rejected, "valid delayed work was not drained");
        std::printf("PASS %s emitted=%llu invalid=%llu replayed=%llu drain_timeouts=%llu live=%u\n", scenario.c_str(), metrics.emitted, metrics.invalid, metrics.replayed, metrics.drain_timeouts, metrics.live);
        return 0;
    }
    catch (const std::exception &error) { std::printf("FAIL %s\n", error.what()); return 1; }
}
