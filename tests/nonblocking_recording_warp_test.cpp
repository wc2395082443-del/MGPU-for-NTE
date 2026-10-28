#include "../src/game_work_trace.hpp"
#include "../src/nonblocking_work_trace.hpp"
#include "../src/frame_measurement.hpp"
#include "../src/log_queue.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::mutex log_mutex;
std::vector<std::string> log_lines;
std::atomic<unsigned> failure_count{0};
std::atomic<bool> go{false};
LONGLONG frequency = 0;
LONGLONG ticks() { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
void require(HRESULT result) { if (FAILED(result)) throw std::runtime_error("WARP API failed"); }
}
namespace mgpu::lq
{
bool running() { return true; }
void post(dest, const char *line)
{
    std::lock_guard<std::mutex> lock(log_mutex);
    log_lines.emplace_back(line);
}
}

int main(int count, char **arguments)
{
    try
    {
        bool capture = true;
        std::wstring adapter_filter;
        unsigned seconds = 2, operations = 16;
        for (int index = 1; index < count; ++index)
        {
            if (std::strcmp(arguments[index], "--baseline") == 0) capture = false;
            else if (std::strcmp(arguments[index], "--seconds") == 0 && index + 1 < count) seconds = unsigned(std::strtoul(arguments[++index], nullptr, 10));
            else if (std::strcmp(arguments[index], "--operations") == 0 && index + 1 < count) operations = unsigned(std::strtoul(arguments[++index], nullptr, 10));
            else if (std::strcmp(arguments[index], "--adapter") == 0 && index + 1 < count)
            {
                const auto value = arguments[++index];
                adapter_filter.assign(value, value + std::strlen(value));
            }
            else throw std::runtime_error("unknown argument");
        }
        if (!seconds || seconds > 120 || !operations || operations > 1024) throw std::runtime_error("invalid test bounds");
        LARGE_INTEGER clock{};
        QueryPerformanceFrequency(&clock);
        frequency = clock.QuadPart;
        using Microsoft::WRL::ComPtr;
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> device;
        require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        if (adapter_filter.empty()) require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        else
        {
            for (unsigned index = 0; ; ++index)
            {
                ComPtr<IDXGIAdapter1> candidate;
                if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 description{};
                require(candidate->GetDesc1(&description));
                if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && std::wstring(description.Description).find(adapter_filter) != std::wstring::npos)
                { adapter = candidate; break; }
            }
            if (!adapter) throw std::runtime_error("requested hardware adapter not found");
        }
        DXGI_ADAPTER_DESC adapter_description{};
        require(adapter->GetDesc(&adapter_description));
        char adapter_name[256]{};
        WideCharToMultiByte(CP_UTF8, 0, adapter_description.Description, -1, adapter_name, sizeof adapter_name, nullptr, nullptr);
        std::printf("ADAPTER %s LUID=%08lx:%08lx\n", adapter_name, static_cast<unsigned long>(adapter_description.AdapterLuid.HighPart), adapter_description.AdapterLuid.LowPart);
        require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        struct lane
        {
            ComPtr<ID3D12CommandQueue> queue;
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> command;
            ComPtr<ID3D12Fence> fence;
            ComPtr<ID3D12Resource> upload, readback;
            HANDLE event = nullptr;
            UINT64 completed = 0;
            UINT64 submissions = 0, cancellations = 0;
            std::vector<double> recording_us;
        };
        constexpr unsigned lane_count = 8;
        std::array<lane, lane_count> lanes;
        for (auto &lane : lanes)
        {
            D3D12_COMMAND_QUEUE_DESC queue{};
            require(device->CreateCommandQueue(&queue, IID_PPV_ARGS(&lane.queue)));
            require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&lane.allocator)));
            require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, lane.allocator.Get(), nullptr, IID_PPV_ARGS(&lane.command)));
            require(lane.command->Close());
            require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&lane.fence)));
            lane.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!lane.event) throw std::runtime_error("fence event failed");
            D3D12_RESOURCE_DESC resource{};
            resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resource.Width = 4096;
            resource.Height = resource.DepthOrArraySize = resource.MipLevels = resource.SampleDesc.Count = 1;
            resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&lane.upload)));
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&lane.readback)));
            void *mapped = nullptr;
            require(lane.upload->Map(0, nullptr, &mapped));
            std::memset(mapped, 0x5a, 4096);
            lane.upload->Unmap(0, nullptr);
            lane.recording_us.reserve(size_t(seconds) * 800);
        }
        HANDLE trigger = nullptr;
        if (capture)
        {
            mgpu::game_work::bind_device(device.Get());
            const auto name = L"Local\\MGPUFramePipeline." + std::to_wstring(GetCurrentProcessId());
            trigger = CreateEventW(nullptr, TRUE, TRUE, name.c_str());
            mgpu::measurement::poll();
            const auto ready_deadline = GetTickCount64() + 10000;
            while (!mgpu::game_work::active() && GetTickCount64() < ready_deadline && !mgpu::game_work::nonblocking::finished()) Sleep(1);
            if (!mgpu::game_work::active()) throw std::runtime_error("continuous capture failed to initialize");
            mgpu::game_work::observe_native_list(lanes[0].command.Get());
        }
        const auto begin = ticks();
        const auto deadline = begin + LONGLONG(seconds) * frequency;
        std::vector<std::thread> producers;
        for (unsigned lane_index = 0; lane_index < lane_count; ++lane_index)
        {
            producers.emplace_back([&, lane_index] {
                auto &lane = lanes[lane_index];
                HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 2, TIMER_ALL_ACCESS);
                try
                {
                    while (!go.load(std::memory_order_acquire)) SwitchToThread();
                    while (ticks() < deadline)
                    {
                        require(lane.allocator->Reset());
                        for (unsigned cancelled = 0; cancelled < 2; ++cancelled)
                        {
                            require(lane.command->Reset(lane.allocator.Get(), nullptr));
                            mgpu::game_work::reset(lane.command.Get());
                            lane.command->CopyBufferRegion(lane.readback.Get(), 0, lane.upload.Get(), 0, 16);
                            require(lane.command->Close());
                            ++lane.cancellations;
                        }
                        const auto record_begin = ticks();
                        require(lane.command->Reset(lane.allocator.Get(), nullptr));
                        mgpu::game_work::reset(lane.command.Get());
                        const auto source = unsigned(lane.submissions * lane_count + lane_index + 1);
                        mgpu::game_work::tag(lane.command.Get(), source, 1, 53, 1280, 720);
                        for (unsigned operation = 0; operation < operations; ++operation)
                            lane.command->CopyBufferRegion(lane.readback.Get(), 0, lane.upload.Get(), 0, 16);
                        require(lane.command->Close());
                        lane.recording_us.push_back(double(ticks() - record_begin) * 1e6 / frequency);
                        ID3D12CommandList *command = lane.command.Get();
                        mgpu::game_work::before_submit(lane.queue.Get(), 1, &command);
                        lane.queue->ExecuteCommandLists(1, &command);
                        mgpu::game_work::after_submit(lane.queue.Get(), 1, &command);
                        require(lane.queue->Signal(lane.fence.Get(), ++lane.completed));
                        require(lane.fence->SetEventOnCompletion(lane.completed, lane.event));
                        if (WaitForSingleObject(lane.event, 10000) != WAIT_OBJECT_0) throw std::runtime_error("test GPU fence timed out");
                        ++lane.submissions;
                        const auto scheduled = begin + LONGLONG(lane.submissions) * frequency / 750;
                        const auto remaining = scheduled - ticks();
                        if (remaining > 0 && timer)
                        {
                            LARGE_INTEGER due{};
                            due.QuadPart = -std::max<LONGLONG>(1, remaining * 10000000 / frequency);
                            if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, 10);
                        }
                    }
                    require(lane.command->Reset(lane.allocator.Get(), nullptr));
                    require(lane.command->Close());
                    mgpu::game_work::destroy(lane.command.Get());
                }
                catch (const std::exception &error) { std::printf("producer failure: %s\n", error.what()); ++failure_count; }
                if (timer) CloseHandle(timer);
            });
        }
        go.store(true, std::memory_order_release);
        unsigned next_progress = 10;
        while (ticks() < deadline)
        {
            if (capture) mgpu::measurement::poll();
            const auto elapsed_seconds = unsigned((ticks() - begin) / frequency);
            if (elapsed_seconds >= next_progress)
            {
                const auto progress = mgpu::game_work::nonblocking::snapshot();
                std::printf("PROGRESS seconds=%u jobs=%llu emitted=%llu rejected=%llu invalid=%llu fine_refused=%llu\n", elapsed_seconds,
                    progress.created, progress.emitted, progress.rejected, progress.invalid, progress.fine_refused);
                std::fflush(stdout);
                next_progress += 10;
            }
            Sleep(20);
        }
        for (auto &producer : producers) producer.join();
        const auto elapsed = double(ticks() - begin) / frequency;
        if (capture)
        {
            ResetEvent(trigger);
            const auto drain_deadline = GetTickCount64() + 20000;
            while (!mgpu::game_work::nonblocking::drained() && GetTickCount64() < drain_deadline) { mgpu::measurement::poll(); Sleep(2); }
            if (!mgpu::game_work::nonblocking::drained()) throw std::runtime_error("asynchronous GPU drain timed out");
            for (unsigned retry = 0; retry < 100; ++retry) { mgpu::measurement::poll(); Sleep(2); }
            CloseHandle(trigger);
        }
        UINT64 submitted = 0, cancelled = 0;
        std::vector<double> durations;
        for (auto &lane : lanes)
        {
            submitted += lane.submissions;
            cancelled += lane.cancellations;
            durations.insert(durations.end(), lane.recording_us.begin(), lane.recording_us.end());
            void *mapped = nullptr;
            require(lane.readback->Map(0, nullptr, &mapped));
            for (unsigned index = 0; index < 16; ++index) if (static_cast<unsigned char *>(mapped)[index] != 0x5a) throw std::runtime_error("GPU copy output changed");
            lane.readback->Unmap(0, nullptr);
            CloseHandle(lane.event);
        }
        std::sort(durations.begin(), durations.end());
        const auto metrics = mgpu::game_work::nonblocking::snapshot();
        std::printf("{\"capture\":%s,\"seconds\":%.6f,\"threads\":%u,\"operations_per_job\":%u,\"submissions\":%llu,\"cancellations\":%llu,\"record_p50_us\":%.3f,\"record_p95_us\":%.3f,\"record_p99_us\":%.3f,\"created\":%llu,\"emitted\":%llu,\"cancelled_reclaimed\":%llu,\"rejected\":%llu,\"invalid\":%llu,\"replayed\":%llu,\"fine_refused\":%llu,\"truncated\":%llu,\"peak_live\":%u,\"coarse_backed\":%u,\"fine_backed\":%u}\n",
            capture ? "true" : "false", elapsed, lane_count, operations, submitted, cancelled, durations[durations.size() / 2], durations[durations.size() * 95 / 100], durations[durations.size() * 99 / 100],
            metrics.created, metrics.emitted, metrics.cancelled, metrics.rejected, metrics.invalid, metrics.replayed, metrics.fine_refused, metrics.truncated,
            metrics.peak_live, metrics.coarse_backed, metrics.fine_backed);
        for (const auto &line : log_lines) if (line.find("pipeline_export=") != std::string::npos || line.find("queue_trace_export=") != std::string::npos || line.find("kind=status") != std::string::npos) std::puts(line.c_str());
        if (failure_count || (capture && (metrics.emitted != submitted || metrics.rejected || metrics.invalid || metrics.replayed || metrics.fine_refused || metrics.truncated)))
            throw std::runtime_error("nonblocking recording lost coverage or changed output");
        if (seconds == 120 && elapsed < 120) throw std::runtime_error("full duration not reached");
        return 0;
    }
    catch (const std::exception &error) { std::printf("FAIL %s\n", error.what()); return 1; }
}
