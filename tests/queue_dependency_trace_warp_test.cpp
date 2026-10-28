#include "../src/queue_dependency_trace.hpp"
#include "../src/gpu_timestamp_span.hpp"
#include <dxgi1_4.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using execute_function = void (STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
execute_function driver_execute = nullptr;
UINT64 linked_session = 0;
void STDMETHODCALLTYPE linked_execute(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    if (linked_session != 0 && count != 0)
        mgpu::queue_trace::job(queue, static_cast<ID3D12GraphicsCommandList *>(lists[0]), linked_session, 17);
    driver_execute(queue, count, lists);
}
void require(HRESULT status) { if (FAILED(status)) throw status; }
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
int main(int argc, char **)
{
    using namespace mgpu::queue_trace;
    try
    {
        ComPtr<IDXGIFactory4> factory;
        require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> adapter;
        require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
        ComPtr<ID3D12Device> device;
        require(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        D3D12_COMMAND_QUEUE_DESC description{};
        ComPtr<ID3D12CommandQueue> renderer, transfer;
        require(device->CreateCommandQueue(&description, IID_PPV_ARGS(&renderer)));
        description.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        require(device->CreateCommandQueue(&description, IID_PPV_ARGS(&transfer)));
        ComPtr<ID3D12Fence> gate, arrival, alias_fence, done;
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        require(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&arrival)));
        require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
        HANDLE shared = nullptr;
        require(device->CreateSharedHandle(arrival.Get(), nullptr, GENERIC_ALL, nullptr, &shared));
        require(device->OpenSharedHandle(shared, IID_PPV_ARGS(&alias_fence)));
        CloseHandle(shared);
        auto **execute_slot = *reinterpret_cast<void ***>(renderer.Get()) + 10;
        driver_execute = reinterpret_cast<execute_function>(*execute_slot);
        DWORD protection = 0, ignored = 0;
        check(VirtualProtect(execute_slot, sizeof(void *), PAGE_READWRITE, &protection) != FALSE, "test forwarder protection");
        InterlockedExchangePointer(execute_slot, reinterpret_cast<void *>(linked_execute));
        check(VirtualProtect(execute_slot, sizeof(void *), protection, &ignored) != FALSE, "test protection restored");
        observe_queue(renderer.Get()); observe_queue(transfer.Get());
        observe_fence(gate.Get()); observe_fence(done.Get());
        alias(arrival.Get(), alias_fence.Get());
        require(gate->Signal(1));
        check(gate->GetCompletedValue() == 1 && !active(), "inactive hook must forward unchanged");
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command;
        require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        require(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&command)));
        require(command->Close());
        const auto session = mgpu::timing::cpu_ticks();
        poll(session);
        check(active(), "trace must activate");
        require(renderer->Wait(gate.Get(), 2));
        linked_session = session;
        ID3D12CommandList *lists[]{command.Get()};
        renderer->ExecuteCommandLists(1, lists);
        require(renderer->Signal(arrival.Get(), 7));
        require(transfer->Wait(alias_fence.Get(), 7));
        require(transfer->Signal(done.Get(), 11));
        check(done->GetCompletedValue() == 0, "queue waits must actually block GPU progress, not the submitting CPU");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(event != nullptr, "event creation");
        require(done->SetEventOnCompletion(11, event));
        check(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "instrumentation must not release gate");
        std::array<std::thread, 4> readers;
        for (auto &reader : readers)
            reader = std::thread([&] { for (unsigned index = 0; index < 1000; ++index) check(gate->GetCompletedValue() == 1, "concurrent sample value"); });
        for (auto &reader : readers) reader.join();
        if (argc > 1)
            for (unsigned index = 0; index < 262144; ++index) check(gate->GetCompletedValue() == 1, "overflow does not change API result");
        require(gate->Signal(2));
        check(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "real dependency chain completes");
        CloseHandle(event);
        check(done->GetCompletedValue() == 11 && alias_fence->GetCompletedValue() == 7, "forwarded signal values preserved");
        poll(0);
        for (unsigned retry = 0; retry < 100 && export_path()[0] == L'\0'; ++retry) { Sleep(1); poll(0); }
        check(export_path()[0] != L'\0', "binary export must finish after writers drain");
        HANDLE file = CreateFileW(export_path(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        check(file != INVALID_HANDLE_VALUE, "export readable");
        header envelope{};
        DWORD bytes = 0;
        check(ReadFile(file, &envelope, sizeof envelope, &bytes, nullptr) && bytes == sizeof envelope, "header read");
        check(envelope.session == session && (argc > 1 ? envelope.overflow > 0 : envelope.overflow == 0) && envelope.failures == 0 && envelope.pending == 0, "valid export envelope or explicit overflow");
        std::vector<record> metadata(envelope.metadata), records(envelope.count);
        check(ReadFile(file, metadata.data(), DWORD(metadata.size() * sizeof(record)), &bytes, nullptr) && bytes == metadata.size() * sizeof(record), "metadata read");
        check(ReadFile(file, records.data(), DWORD(records.size() * sizeof(record)), &bytes, nullptr) && bytes == records.size() * sizeof(record), "records read");
        CloseHandle(file);
        unsigned executes = 0, waits = 0, signals = 0, samples = 0, events = 0, cpu_signals = 0, jobs = 0;
        for (unsigned index = 0; index < records.size(); ++index)
        {
            const auto &entry = records[index];
            check(entry.sequence == index + 1 && entry.end >= entry.begin && entry.begin >= envelope.begin, "record publication/clock integrity");
            executes += entry.operation == unsigned(kind::execute);
            waits += entry.operation == unsigned(kind::queue_wait);
            signals += entry.operation == unsigned(kind::queue_signal);
            samples += entry.operation == unsigned(kind::completed);
            events += entry.operation == unsigned(kind::set_event);
            cpu_signals += entry.operation == unsigned(kind::cpu_signal);
            jobs += entry.operation == unsigned(kind::job);
            if (entry.operation == unsigned(kind::job))
                check(entry.parent != 0 && records[entry.parent-1].operation == unsigned(kind::execute),
                    "actual submission observer must link its job to the enclosing batch");
        }
        check(argc > 1 || (executes == 1 && waits == 2 && signals == 2 && samples == 4003 && events == 1 && cpu_signals == 1 && jobs == 1),
              "exact API counts: no invented or lost calls");
        unsigned aliases = 0;
        for (const auto &entry : metadata) aliases += entry.operation == unsigned(kind::alias);
        check(aliases == 1, "shared fence aliases retained");
        require(gate->Signal(3));
        check(!active() && gate->GetCompletedValue() == 3, "stopped recording remains a transparent forwarder");
        check(CopyFileW(export_path(), argc > 1 ? L"queue-trace-overflow.bin" : L"queue-trace-warp.bin", FALSE) != FALSE, "retain exact fixture for decoder");
        std::printf("PASS: %llu records; exact API forwarding, delayed GPU dependencies, shared aliases, concurrent readers, bounded export; no hardware performance claim\n", envelope.count);
        return 0;
    }
    catch (HRESULT result) { std::printf("FAIL HRESULT %08lx\n", static_cast<unsigned long>(result)); }
    catch (const std::exception &error) { std::printf("FAIL %s\n", error.what()); }
    return 1;
}
