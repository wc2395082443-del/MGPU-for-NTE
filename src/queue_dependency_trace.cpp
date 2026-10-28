#include "queue_dependency_trace.hpp"
#include "gpu_timestamp_span.hpp"
#include "continuous_trace.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <wrl/client.h>

namespace mgpu::queue_trace
{
namespace
{
constexpr unsigned capacity = 262144, object_capacity = 4096, hook_capacity = 32;
const GUID identity_guid = {0x37d7d54c,0x20b3,0x4539,{0xaf,0x24,0x71,0x84,0xc2,0x99,0x03,0x43}};
struct hook
{
    void **table = nullptr;
    std::atomic<void **> published{nullptr};
    std::array<std::atomic<void *>, 19> originals{};
};
struct state
{
    std::mutex metadata_mutex, hooks_mutex;
    std::array<record, object_capacity * 2> metadata{};
    unsigned metadata_count = 0, object_count = 0;
    std::array<hook, hook_capacity> queues{}, fences{};
    std::atomic<UINT64> session{0}, next{0}, overflow{0}, failures{0}, writers{0};
    std::unique_ptr<record[]> records;
    header file_header{};
    HANDLE output = INVALID_HANDLE_VALUE;
    UINT64 offset = 0;
    bool attempted = false, sealed = false, finished = false;
    bool continuous_mode = false;
    trace::continuous_stream<record, capacity> stream;
    wchar_t partial[MAX_PATH]{}, final[MAX_PATH]{};
};
state &data() { static auto *instance = new state; return *instance; }
thread_local UINT64 parent_call = 0;

UINT64 object_id(ID3D12DeviceChild *object, unsigned category)
{
    if (object == nullptr) return 0;
    auto &context = data();
    UINT64 identity = 0;
    UINT bytes = sizeof identity;
    if (SUCCEEDED(object->GetPrivateData(identity_guid, &bytes, &identity)) && bytes == sizeof identity && identity != 0)
        return identity;
    std::lock_guard<std::mutex> lock(context.metadata_mutex);
    bytes = sizeof identity;
    if (SUCCEEDED(object->GetPrivateData(identity_guid, &bytes, &identity)) && bytes == sizeof identity && identity != 0)
        return identity;
    if (context.object_count >= object_capacity || context.metadata_count >= context.metadata.size())
    { ++context.failures; return 0; }
    identity = ++context.object_count;
    if (FAILED(object->SetPrivateData(identity_guid, sizeof identity, &identity)))
    { ++context.failures; return 0; }
    record entry{};
    entry.operation = unsigned(kind::object);
    entry.object = identity;
    entry.other = category;
    entry.raw = reinterpret_cast<UINT64>(object);
    entry.begin = entry.end = timing::cpu_ticks();
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (SUCCEEDED(object->GetDevice(IID_PPV_ARGS(&device))))
    {
        const auto luid = device->GetAdapterLuid();
        entry.value = (UINT64(UINT(luid.HighPart)) << 32) | luid.LowPart;
        entry.auxiliary = reinterpret_cast<UINT64>(device.Get());
    }
    else ++context.failures;
    if (category == 1) entry.extra = unsigned(static_cast<ID3D12CommandQueue *>(object)->GetDesc().Type);
    context.metadata[context.metadata_count++] = entry;
    return identity;
}

struct scope
{
    record current{};
    record *entry = nullptr;
    UINT64 prior = parent_call;
    bool retained = false;
    scope(kind operation, UINT64 object = 0, UINT64 other = 0, UINT64 value = 0)
    {
        auto &context = data();
        const auto session = context.session.load(std::memory_order_acquire);
        if (session == 0) return;
        context.writers.fetch_add(1, std::memory_order_acq_rel);
        retained = true;
        if (session != context.session.load(std::memory_order_acquire)) return;
        const auto index = context.next.fetch_add(1, std::memory_order_relaxed);
        if (!context.continuous_mode && index >= capacity) { ++context.overflow; return; }
        entry = context.continuous_mode ? &current : &context.records[index];
        entry->sequence = index + 1;
        entry->parent = prior;
        entry->begin = timing::cpu_ticks();
        entry->thread = GetCurrentThreadId();
        entry->operation = unsigned(operation);
        entry->object = object;
        entry->other = other;
        entry->value = value;
        parent_call = entry->sequence;
    }
    ~scope()
    {
        if (entry != nullptr)
        {
            entry->end = timing::cpu_ticks();
            parent_call = prior;
            if (data().continuous_mode) data().stream.push(*entry);
        }
        if (retained) data().writers.fetch_sub(1, std::memory_order_release);
    }
};

using execute_fn = void (STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
using queue_fence_fn = HRESULT (STDMETHODCALLTYPE *)(ID3D12CommandQueue *, ID3D12Fence *, UINT64);
using completed_fn = UINT64 (STDMETHODCALLTYPE *)(ID3D12Fence *);
using event_fn = HRESULT (STDMETHODCALLTYPE *)(ID3D12Fence *, UINT64, HANDLE);
using signal_fn = HRESULT (STDMETHODCALLTYPE *)(ID3D12Fence *, UINT64);

template<unsigned Index>
void STDMETHODCALLTYPE execute(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    const auto original = reinterpret_cast<execute_fn>(data().queues[Index].originals[10].load(std::memory_order_acquire));
    if (!active()) { original(queue, count, lists); return; }
    scope call(kind::execute);
    if (call.entry != nullptr)
    {
        call.entry->object = object_id(queue, 1);
        call.entry->value = count;
        call.entry->raw = reinterpret_cast<UINT64>(queue);
        for (UINT index = 0; lists != nullptr && index < count; ++index)
        {
            scope command(kind::command, call.entry->object, reinterpret_cast<UINT64>(lists[index]), index);
            if (command.entry) command.entry->extra = count;
        }
    }
    if (call.entry) call.entry->extra = timing::cpu_ticks();
    original(queue, count, lists);
    if (call.entry) call.entry->auxiliary = timing::cpu_ticks();
}

template<unsigned Index, bool Wait>
HRESULT STDMETHODCALLTYPE queue_fence(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value)
{
    const auto original = reinterpret_cast<queue_fence_fn>(data().queues[Index].originals[Wait ? 15 : 14].load(std::memory_order_acquire));
    if (!active()) return original(queue, fence, value);
    scope call(Wait ? kind::queue_wait : kind::queue_signal);
    if (call.entry != nullptr)
    {
        observe_fence(fence);
        call.entry->object = object_id(queue, 1);
        call.entry->other = object_id(fence, 2);
        call.entry->value = value;
        call.entry->raw = reinterpret_cast<UINT64>(queue);
    }
    if (call.entry) call.entry->extra = timing::cpu_ticks();
    const auto result = original(queue, fence, value);
    if (call.entry) call.entry->auxiliary = timing::cpu_ticks();
    if (call.entry) call.entry->result = result;
    return result;
}

template<unsigned Index>
UINT64 STDMETHODCALLTYPE completed(ID3D12Fence *fence)
{
    const auto original = reinterpret_cast<completed_fn>(data().fences[Index].originals[8].load(std::memory_order_acquire));
    if (!active()) return original(fence);
    scope call(kind::completed);
    if (call.entry) { call.entry->object = object_id(fence, 2); call.entry->raw = reinterpret_cast<UINT64>(fence); }
    if (call.entry) call.entry->extra = timing::cpu_ticks();
    const auto result = original(fence);
    if (call.entry) call.entry->auxiliary = timing::cpu_ticks();
    if (call.entry) call.entry->value = result;
    return result;
}

template<unsigned Index>
HRESULT STDMETHODCALLTYPE set_event(ID3D12Fence *fence, UINT64 value, HANDLE event)
{
    const auto original = reinterpret_cast<event_fn>(data().fences[Index].originals[9].load(std::memory_order_acquire));
    if (!active()) return original(fence, value, event);
    scope call(kind::set_event);
    if (call.entry)
    {
        call.entry->object = object_id(fence, 2); call.entry->value = value;
        call.entry->other = reinterpret_cast<UINT64>(event); call.entry->raw = reinterpret_cast<UINT64>(fence);
    }
    if (call.entry) call.entry->extra = timing::cpu_ticks();
    const auto result = original(fence, value, event);
    if (call.entry) call.entry->auxiliary = timing::cpu_ticks();
    if (call.entry) call.entry->result = result;
    return result;
}

template<unsigned Index>
HRESULT STDMETHODCALLTYPE cpu_signal(ID3D12Fence *fence, UINT64 value)
{
    const auto original = reinterpret_cast<signal_fn>(data().fences[Index].originals[10].load(std::memory_order_acquire));
    if (!active()) return original(fence, value);
    scope call(kind::cpu_signal);
    if (call.entry) { call.entry->object = object_id(fence, 2); call.entry->value = value; call.entry->raw = reinterpret_cast<UINT64>(fence); }
    if (call.entry) call.entry->extra = timing::cpu_ticks();
    const auto result = original(fence, value);
    if (call.entry) call.entry->auxiliary = timing::cpu_ticks();
    if (call.entry) call.entry->result = result;
    return result;
}

bool patch(hook &target, unsigned slot, void *replacement)
{
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(replacement), &pinned)) return false;
    DWORD previous = 0;
    if (!VirtualProtect(target.table + slot, sizeof(void *), PAGE_READWRITE, &previous)) return false;
    target.originals[slot].store(target.table[slot], std::memory_order_release);
    InterlockedExchangePointer(target.table + slot, replacement);
    DWORD ignored = 0;
    const bool restored = VirtualProtect(target.table + slot, sizeof(void *), previous, &ignored) != FALSE;
    if (!restored) ++data().failures;
    return true;
}

template<size_t... Indices>
void install(void **table, bool queue, std::index_sequence<Indices...>)
{
    auto &context = data();
    auto &hooks = queue ? context.queues : context.fences;
    for (const auto &entry : hooks) if (entry.published.load(std::memory_order_acquire) == table) return;
    std::lock_guard<std::mutex> lock(context.hooks_mutex);
    for (const auto &entry : hooks) if (entry.table == table) return;
    const std::array<void *, hook_capacity> executes{reinterpret_cast<void *>(execute<Indices>)...};
    const std::array<void *, hook_capacity> waits{reinterpret_cast<void *>(queue_fence<Indices, true>)...};
    const std::array<void *, hook_capacity> signals{reinterpret_cast<void *>(queue_fence<Indices, false>)...};
    const std::array<void *, hook_capacity> reads{reinterpret_cast<void *>(completed<Indices>)...};
    const std::array<void *, hook_capacity> events{reinterpret_cast<void *>(set_event<Indices>)...};
    const std::array<void *, hook_capacity> cpu_signals{reinterpret_cast<void *>(cpu_signal<Indices>)...};
    for (unsigned index = 0; index < hooks.size(); ++index)
    {
        auto &entry = hooks[index];
        if (entry.table != nullptr) continue;
        entry.table = table;
        const bool first = patch(entry, queue ? 10 : 8, queue ? executes[index] : reads[index]);
        const bool second = patch(entry, queue ? 14 : 9, queue ? signals[index] : events[index]);
        const bool third = patch(entry, queue ? 15 : 10, queue ? waits[index] : cpu_signals[index]);
        if (!first || !second || !third) ++context.failures;
        entry.published.store(table, std::memory_order_release);
        return;
    }
    ++context.failures;
}

bool write(HANDLE file, const void *bytes, DWORD count)
{
    DWORD written = 0;
    return WriteFile(file, bytes, count, &written, nullptr) && written == count;
}
}

bool active() { return data().session.load(std::memory_order_acquire) != 0; }

void observe_queue(ID3D12CommandQueue *queue)
{
    if (queue == nullptr) return;
    object_id(queue, 1);
    install(*reinterpret_cast<void ***>(queue), true, std::make_index_sequence<hook_capacity>{});
}

void observe_fence(ID3D12Fence *fence)
{
    if (fence == nullptr) return;
    object_id(fence, 2);
    install(*reinterpret_cast<void ***>(fence), false, std::make_index_sequence<hook_capacity>{});
}

void alias(ID3D12Fence *first, ID3D12Fence *second)
{
    if (first == nullptr || second == nullptr) return;
    observe_fence(first); observe_fence(second);
    const auto first_id = object_id(first, 2), second_id = object_id(second, 2);
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.metadata_mutex);
    for (unsigned index = 0; index < context.metadata_count; ++index)
    {
        const auto &entry = context.metadata[index];
        if (entry.operation == unsigned(kind::alias) && entry.object == first_id && entry.other == second_id) return;
    }
    if (context.metadata_count == context.metadata.size()) { ++context.failures; return; }
    auto &entry = context.metadata[context.metadata_count++];
    entry.operation = unsigned(kind::alias); entry.object = first_id; entry.other = second_id;
    entry.begin = entry.end = timing::cpu_ticks();
}

void job(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *command, UINT64 session, UINT64 job_id)
{
    if (!active()) return;
    scope call(kind::job);
    if (!call.entry) return;
    call.entry->object = object_id(queue, 1);
    call.entry->other = reinterpret_cast<UINT64>(command);
    call.entry->value = job_id; call.entry->extra = session;
}

void poll(UINT64 session, bool continuous)
{
    auto &context = data();
    if (!context.attempted && session != 0)
    {
        context.attempted = true;
        context.continuous_mode = continuous;
        if (!continuous)
        {
            context.records.reset(new (std::nothrow) record[capacity]);
            if (!context.records) { ++context.failures; context.finished = true; return; }
        }
        context.file_header.session = session;
        context.file_header.frequency = timing::cpu_frequency();
        context.file_header.begin = timing::cpu_ticks();
        wchar_t temporary[MAX_PATH]{};
        const auto count = GetTempPathW(MAX_PATH, temporary);
        if (count == 0 || count >= MAX_PATH ||
            _snwprintf_s(context.final, MAX_PATH, _TRUNCATE, L"%sMGPUQueueTrace.%lu.%llu.bin", temporary, GetCurrentProcessId(), session) < 0 ||
            _snwprintf_s(context.partial, MAX_PATH, _TRUNCATE, L"%s.partial", context.final) < 0)
        { ++context.failures; context.finished = true; return; }
        if (continuous && !context.stream.start(context.final, session))
        { ++context.failures; context.finished = true; return; }
        context.session.store(session, std::memory_order_release);
    }
    if (active() && (session != context.file_header.session ||
        timing::cpu_ticks() - context.file_header.begin > (context.continuous_mode ? 165u : 5u) * context.file_header.frequency))
    {
        if (session != 0) ++context.failures;
        context.session.store(0, std::memory_order_release);
        context.file_header.end = timing::cpu_ticks();
    }
    if (!context.attempted || active() || context.finished || context.writers.load(std::memory_order_acquire) != 0) return;
    if (context.continuous_mode)
    {
        if (!context.sealed)
        {
            context.sealed = true;
            std::lock_guard<std::mutex> lock(context.metadata_mutex);
            for (unsigned index = 0; index < context.metadata_count; ++index)
                context.stream.push(context.metadata[index]);
            record audit{};
            audit.operation = 0;
            audit.begin = context.file_header.begin;
            audit.end = context.file_header.end;
            audit.object = context.overflow.load();
            audit.other = context.failures.load();
            audit.value = context.writers.load();
            audit.extra = context.next.load();
            context.stream.push(audit);
            context.stream.finish();
        }
        if (context.stream.done())
        {
            context.file_header.failures = context.failures.load();
            context.finished = true;
        }
        return;
    }
    if (!context.sealed)
    {
        context.sealed = true;
        context.file_header.count = (std::min)(context.next.load(), UINT64(capacity));
        context.file_header.overflow = context.overflow.load();
        context.file_header.failures = context.failures.load();
        context.output = CreateFileW(context.partial, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::lock_guard<std::mutex> lock(context.metadata_mutex);
        context.file_header.metadata = context.metadata_count;
        if (context.output == INVALID_HANDLE_VALUE || !write(context.output, &context.file_header, sizeof(header)) ||
            !write(context.output, context.metadata.data(), context.metadata_count * sizeof(record)))
        {
            if (context.output != INVALID_HANDLE_VALUE) CloseHandle(context.output);
            context.output = INVALID_HANDLE_VALUE; context.finished = true; ++context.failures; return;
        }
    }
    const auto total_bytes = context.file_header.count * sizeof(record);
    const auto count = DWORD((std::min)(total_bytes - context.offset, UINT64(1024 * 1024)));
    if (count != 0 && !write(context.output, reinterpret_cast<const char *>(context.records.get()) + context.offset, count))
    { CloseHandle(context.output); context.output = INVALID_HANDLE_VALUE; context.finished = true; ++context.failures; return; }
    context.offset += count;
    if (context.offset == total_bytes)
    {
        CloseHandle(context.output); context.output = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(context.partial, context.final, 0)) ++context.failures;
        context.finished = true;
    }
}

const wchar_t *export_path()
{
    auto &context = data();
    if (context.continuous_mode)
        return context.finished && context.stream.done() && !context.stream.failures() ? context.final : L"";
    return context.finished && context.sealed && context.failures.load() == context.file_header.failures
        ? context.final : L"";
}
}
