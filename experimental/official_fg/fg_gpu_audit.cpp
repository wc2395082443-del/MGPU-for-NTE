#include "fg_gpu_audit.hpp"
#include "../../ext/ngx/nvsdk_ngx.h"
#include "../../ext/ngx/nvsdk_ngx_defs_dlssg.h"
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstring>
#include <string>
#include <share.h>

namespace mgpu::official_fg::gpu_audit
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr unsigned capacity = 4096, queue_capacity = 16, hook_capacity = 4;
using execute_fn = void (STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
using queue_fence_fn = HRESULT (STDMETHODCALLTYPE *)(ID3D12CommandQueue *, ID3D12Fence *, UINT64);
using create_fn = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
using evaluate_fn = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *,
    const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback_C);
using release_fn = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle *);

std::uint64_t ticks()
{
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart;
}

struct record
{
    std::uint64_t id = 0, frame = 0, list = 0, cpu_begin = 0, cpu_end = 0, submitted = 0;
    std::uint64_t fence_value = 0, gpu_begin = 0, gpu_end = 0;
    std::uint64_t call_sequence = 0, call_source = 0;
    unsigned count = 0, index = 0, parameter_mask = 0, result = 0, queue = UINT_MAX;
    unsigned status = 0;
    unsigned thread = 0;
};
struct queue_data
{
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t next = 0, frequency = 0;
};
struct queue_hook
{
    void **table = nullptr;
    execute_fn original = nullptr;
    queue_fence_fn signal = nullptr, wait = nullptr;
};
struct fence_record
{
    std::uint64_t begin = 0, end = 0, queue = 0, fence = 0, value = 0;
    std::uint64_t caller_sequence = 0, caller_source = 0;
    unsigned thread = 0, wait = 0;
    HRESULT result = S_OK;
};
struct import_patch { void **slot = nullptr; void *original = nullptr, *replacement = nullptr; };
struct state
{
    std::mutex mutex;
    std::array<record, capacity> records{};
    std::array<queue_data, queue_capacity> queues{};
    std::array<queue_hook, hook_capacity> hooks{};
    std::array<const NVSDK_NGX_Handle *, 32> handles{};
    std::array<fence_record, 32768> fence_records{};
    std::vector<import_patch> imports;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> readback;
    std::uint64_t *mapped = nullptr;
    HMODULE common = nullptr;
    create_fn create = nullptr;
    evaluate_fn evaluate = nullptr;
    release_fn release = nullptr;
    HANDLE trigger = nullptr;
    FILE *events = nullptr;
    std::filesystem::path logs;
    unsigned used = 0, complete = 0, queue_count = 0, hook_count = 0;
    unsigned fence_count = 0;
    std::uint64_t failures = 0, overflow = 0, creates = 0, evaluations = 0, resolves = 0;
    std::uint64_t deadline = 0, qpc_frequency = 0;
    std::atomic<bool> capturing{false};
    std::atomic<unsigned> pending{0};
    bool attempted = false, ready = false, stopped = false, test_requested = false;
    std::uint64_t last_save = 0;
    std::uint64_t last_clock = 0;
    bool final_clock_taken = false;
};
state &data() { static auto *value = new state; return *value; }
thread_local std::uint64_t present_sequence = 0, present_source = 0;
#ifdef MGPU_FG_AUDIT_FOCUS_STUB
std::atomic<HWND> synthetic_focus{nullptr};
HWND WINAPI test_foreground() { return synthetic_focus.load(); }
#endif

void write_event(const char *kind, std::uint64_t first, std::uint64_t second, std::uint64_t third)
{
    auto &context = data();
    if (context.events) std::fprintf(context.events, "%s,%llu,%lu,%llu,%llu,%llu\n", kind,
        ticks(), GetCurrentThreadId(), first, second, third);
}

bool prepare()
{
    auto &context = data();
    if (context.ready) return true;
    if (!context.device) return false;
    D3D12_QUERY_HEAP_DESC query{}; query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; query.Count = capacity * 2;
    if (FAILED(context.device->CreateQueryHeap(&query, IID_PPV_ARGS(&context.queries)))) return false;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = capacity * 2 * sizeof(std::uint64_t);
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(context.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&context.readback)))) return false;
    D3D12_RANGE range{0, static_cast<SIZE_T>(buffer.Width)};
    if (FAILED(context.readback->Map(0, &range, reinterpret_cast<void **>(&context.mapped)))) return false;
    context.ready = true;
    return true;
}

template<unsigned Hook, bool Wait>
HRESULT STDMETHODCALLTYPE queue_fence(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value)
{
    auto &context = data();
    const auto original = Wait ? context.hooks[Hook].wait : context.hooks[Hook].signal;
    const bool capture = context.capturing.load(std::memory_order_acquire);
    const auto before = capture ? ticks() : 0;
    const auto result = original(queue, fence, value);
    if (capture) {
        const auto after = ticks();
        std::lock_guard<std::mutex> lock(context.mutex);
        if (context.fence_count < context.fence_records.size()) {
            context.fence_records[context.fence_count++] = {before, after,
                reinterpret_cast<std::uint64_t>(queue), reinterpret_cast<std::uint64_t>(fence), value,
                present_sequence, present_source, GetCurrentThreadId(), Wait ? 1u : 0u, result};
        } else ++context.overflow;
    }
    return result;
}

template<unsigned Hook>
void STDMETHODCALLTYPE execute(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    auto &context = data();
    const auto original = context.hooks[Hook].original;
    if (!context.pending.load(std::memory_order_acquire)) { original(queue, count, lists); return; }
    std::array<unsigned, capacity> selected{};
    unsigned selected_count = 0, queue_index = UINT_MAX;
    {
        std::lock_guard<std::mutex> lock(context.mutex);
        for (unsigned index = 0; index < context.used; ++index) {
            auto &entry = context.records[index];
            if (entry.status != 2) continue;
            for (UINT position = 0; position < count; ++position) {
                if (entry.list != reinterpret_cast<std::uint64_t>(lists[position])) continue;
                selected[selected_count++] = index; entry.status = 3;
                context.pending.fetch_sub(1, std::memory_order_release); break;
            }
        }
        if (selected_count) {
            for (unsigned index = 0; index < context.queue_count; ++index)
                if (context.queues[index].queue.Get() == queue) { queue_index = index; break; }
            if (queue_index == UINT_MAX && context.queue_count < queue_capacity) {
                queue_index = context.queue_count++;
                auto &destination = context.queues[queue_index];
                destination.queue = queue;
                if (FAILED(context.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&destination.fence))) ||
                    FAILED(queue->GetTimestampFrequency(&destination.frequency))) {
                    ++context.failures; queue_index = UINT_MAX;
                }
            }
            if (queue_index != UINT_MAX && (!context.queues[queue_index].fence || !context.queues[queue_index].frequency))
                queue_index = UINT_MAX;
        }
    }
    original(queue, count, lists);
    if (!selected_count) return;
    std::lock_guard<std::mutex> lock(context.mutex);
    if (queue_index == UINT_MAX) {
        for (unsigned index = 0; index < selected_count; ++index) context.records[selected[index]].status = 7;
        ++context.failures; return;
    }
    auto &destination = context.queues[queue_index];
    const auto value = ++destination.next;
    const auto stamp = ticks();
    std::uint64_t gpu = 0, cpu = 0;
    const auto before = ticks();
    const auto calibration = queue->GetClockCalibration(&gpu, &cpu);
    const auto after = ticks();
    if (context.events) std::fprintf(context.events, "clock,%llu,%lu,%u,%llu,%llu,%llu,%llu,%llu,%ld\n",
        stamp, GetCurrentThreadId(), queue_index, gpu, cpu, destination.frequency, before, after, calibration);
    const auto result = context.hooks[Hook].signal(queue, destination.fence.Get(), value);
    for (unsigned index = 0; index < selected_count; ++index) {
        auto &entry = context.records[selected[index]];
        entry.queue = queue_index; entry.fence_value = value; entry.submitted = stamp;
        entry.status = SUCCEEDED(result) ? 4 : 7;
    }
    if (FAILED(result)) ++context.failures;
}

bool observe_queue(ID3D12CommandQueue *queue)
{
    auto &context = data();
    auto **table = *reinterpret_cast<void ***>(queue);
    for (unsigned index = 0; index < context.hook_count; ++index) if (context.hooks[index].table == table) return true;
    if (context.hook_count == hook_capacity) return false;
    const auto index = context.hook_count;
    const execute_fn functions[] = {execute<0>, execute<1>, execute<2>, execute<3>};
    const queue_fence_fn signals[] = {queue_fence<0,false>,queue_fence<1,false>,queue_fence<2,false>,queue_fence<3,false>};
    const queue_fence_fn waits[] = {queue_fence<0,true>,queue_fence<1,true>,queue_fence<2,true>,queue_fence<3,true>};
    auto &hook = context.hooks[index]; hook.table = table;
    hook.original = reinterpret_cast<execute_fn>(table[10]);
    hook.signal = reinterpret_cast<queue_fence_fn>(table[14]);
    hook.wait = reinterpret_cast<queue_fence_fn>(table[15]);
    DWORD protection = 0, ignored = 0;
    if (!VirtualProtect(table + 10, sizeof(void *), PAGE_READWRITE, &protection)) return false;
    InterlockedExchangePointer(table + 10, reinterpret_cast<void *>(functions[index]));
    VirtualProtect(table + 10, sizeof(void *), protection, &ignored);
    for (unsigned slot : {14u,15u}) {
        if (!VirtualProtect(table + slot, sizeof(void *), PAGE_READWRITE, &protection)) { ++context.failures; continue; }
        InterlockedExchangePointer(table + slot, reinterpret_cast<void *>(slot == 14 ? signals[index] : waits[index]));
        VirtualProtect(table + slot, sizeof(void *), protection, &ignored);
    }
    ++context.hook_count; return true;
}

bool known_handle(const NVSDK_NGX_Handle *handle)
{
    auto &context = data();
    for (auto *candidate : context.handles) if (candidate && candidate == handle) return true;
    return false;
}

NVSDK_NGX_Result NVSDK_CONV create_feature(ID3D12GraphicsCommandList *commands, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter *parameters, NVSDK_NGX_Handle **handle)
{
    const auto result = data().create(commands, feature, parameters, handle);
    if (feature == NVSDK_NGX_Feature_FrameGeneration && NVSDK_NGX_SUCCEED(result) && handle && *handle) {
        std::lock_guard<std::mutex> lock(data().mutex);
        ++data().creates;
        for (auto &candidate : data().handles) if (!candidate) { candidate = *handle; return result; }
        ++data().failures;
    }
    return result;
}

NVSDK_NGX_Result NVSDK_CONV release_feature(NVSDK_NGX_Handle *handle)
{
    const auto result = data().release(handle);
    std::lock_guard<std::mutex> lock(data().mutex);
    for (auto &candidate : data().handles) if (candidate == handle) candidate = nullptr;
    return result;
}

NVSDK_NGX_Result NVSDK_CONV evaluate_feature(ID3D12GraphicsCommandList *commands, const NVSDK_NGX_Handle *handle,
    const NVSDK_NGX_Parameter *parameters, PFN_NVSDK_NGX_ProgressCallback_C callback)
{
    bool selected = false;
    { std::lock_guard<std::mutex> lock(data().mutex); selected = known_handle(handle); if (selected) ++data().evaluations; }
    if (!selected || !data().capturing.load(std::memory_order_acquire)) return data().evaluate(commands, handle, parameters, callback);
    unsigned long long frame = 0;
    unsigned count = 0, index = 0, mask = 0;
    if (parameters) {
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, &frame))) mask |= 1;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, &count))) mask |= 2;
        if (NVSDK_NGX_SUCCEED(parameters->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, &index))) mask |= 4;
    }
    const auto ticket = begin(commands, frame, count, index, mask);
    const auto result = data().evaluate(commands, handle, parameters, callback);
    end(ticket, commands, static_cast<unsigned>(result));
    return result;
}

FARPROC WINAPI resolve(HMODULE module, LPCSTR name)
{
    const auto original = GetProcAddress(module, name);
    if (!original || reinterpret_cast<std::uintptr_t>(name) <= 0xffff) return original;
    const bool create = std::strcmp(name, "NVSDK_NGX_D3D12_CreateFeature") == 0;
    const bool evaluate = std::strcmp(name, "NVSDK_NGX_D3D12_EvaluateFeature") == 0;
    const bool release = std::strcmp(name, "NVSDK_NGX_D3D12_ReleaseFeature") == 0;
    if (!create && !evaluate && !release) return original;
    std::lock_guard<std::mutex> lock(data().mutex);
    ++data().resolves;
    void *replacement = nullptr;
    if (create && (!data().create || data().create == reinterpret_cast<create_fn>(original))) {
        data().create = reinterpret_cast<create_fn>(original); replacement = reinterpret_cast<void *>(create_feature);
    } else if (evaluate && (!data().evaluate || data().evaluate == reinterpret_cast<evaluate_fn>(original))) {
        data().evaluate = reinterpret_cast<evaluate_fn>(original); replacement = reinterpret_cast<void *>(evaluate_feature);
    } else if (release && (!data().release || data().release == reinterpret_cast<release_fn>(original))) {
        data().release = reinterpret_cast<release_fn>(original); replacement = reinterpret_cast<void *>(release_feature);
    } else ++data().failures;
    std::printf("[FG-GPU-AUDIT] resolve %s original=%p wrapped=%u\n", name, original, replacement ? 1u : 0u);
    return replacement ? reinterpret_cast<FARPROC>(replacement) : original;
}

void patch_imports(HMODULE module)
{
    auto *base = reinterpret_cast<unsigned char *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return;
    const auto *header = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (header->Signature != IMAGE_NT_SIGNATURE || header->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;
    const auto &directory = header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || directory.VirtualAddress + directory.Size > header->OptionalHeader.SizeOfImage) return;
    auto *descriptors = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    const auto count = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (unsigned index = 0; index < count && descriptors[index].Name; ++index) {
        const auto offset = descriptors[index].FirstThunk;
        if (!offset || offset >= header->OptionalHeader.SizeOfImage) continue;
        auto *thunk = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + offset);
        const auto maximum = (header->OptionalHeader.SizeOfImage - offset) / sizeof(IMAGE_THUNK_DATA64);
        for (unsigned position = 0; position < maximum && thunk[position].u1.Function; ++position) {
            auto **slot = reinterpret_cast<void **>(&thunk[position].u1.Function);
            void *replacement = nullptr;
            if (*slot == reinterpret_cast<void *>(GetProcAddress)) replacement = reinterpret_cast<void *>(resolve);
#ifdef MGPU_FG_AUDIT_FOCUS_STUB
            if (*slot == reinterpret_cast<void *>(GetForegroundWindow)) replacement = reinterpret_cast<void *>(test_foreground);
#endif
            if (!replacement) continue;
            DWORD protection = 0, ignored = 0;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection)) { ++data().failures; continue; }
            data().imports.push_back({slot, *slot, replacement});
            InterlockedExchangePointer(slot, replacement);
            VirtualProtect(slot, sizeof(void *), protection, &ignored);
        }
    }
}

void save()
{
    auto &context = data();
    if (context.logs.empty()) return;
    FILE *file = _wfsopen((context.logs / L"fg-gpu-evaluations.csv").c_str(), L"wb", _SH_DENYNO);
    if (!file) { ++context.failures; return; }
    std::fprintf(file, "id,backbuffer_frame_id,multi_frame_count,multi_frame_index,parameter_mask,command_list,cpu_begin_qpc,cpu_end_qpc,submitted_qpc,queue_index,queue_pointer,queue_type,completion_fence_value,status,result,gpu_begin,gpu_end,gpu_frequency,gpu_elapsed_ms,caller_sequence,caller_source,thread\n");
    for (unsigned index = 0; index < context.used; ++index) {
        const auto &entry = context.records[index];
        const auto *queue = entry.queue < context.queue_count ? &context.queues[entry.queue] : nullptr;
        const auto frequency = queue ? queue->frequency : 0;
        const double elapsed = entry.status == 5 && frequency ? double(entry.gpu_end - entry.gpu_begin) * 1000 / double(frequency) : -1;
        std::fprintf(file, "%llu,%llu,%u,%u,%u,%llu,%llu,%llu,%llu,%u,%llu,%u,%llu,%u,%u,%llu,%llu,%llu,%.9f,%llu,%llu,%u\n",
            entry.id, entry.frame, entry.count, entry.index, entry.parameter_mask, entry.list,
            entry.cpu_begin, entry.cpu_end, entry.submitted, entry.queue,
            queue ? reinterpret_cast<std::uint64_t>(queue->queue.Get()) : 0,
            queue ? static_cast<unsigned>(queue->queue->GetDesc().Type) : UINT_MAX, entry.fence_value, entry.status, entry.result,
            entry.gpu_begin, entry.gpu_end, frequency, elapsed, entry.call_sequence, entry.call_source, entry.thread);
    }
    std::fclose(file);
    file = _wfsopen((context.logs / L"fg-gpu-queue-fences.csv").c_str(), L"wb", _SH_DENYNO);
    if (file) {
        std::fprintf(file, "cpu_begin_qpc,cpu_end_qpc,queue,fence,value,thread,is_wait,result,caller_sequence,caller_source\n");
        for (unsigned index = 0; index < context.fence_count; ++index) {
            const auto &entry = context.fence_records[index];
            std::fprintf(file, "%llu,%llu,%llu,%llu,%llu,%u,%u,%ld,%llu,%llu\n",entry.begin,entry.end,entry.queue,
                entry.fence,entry.value,entry.thread,entry.wait,entry.result,entry.caller_sequence,entry.caller_source);
        }
        std::fclose(file);
    }
    file = _wfsopen((context.logs / L"fg-gpu-audit-status.json").c_str(), L"wb", _SH_DENYNO);
    if (file) {
        std::fprintf(file, "{\"schema\":1,\"qpc_frequency\":%llu,\"resolved\":%llu,\"fg_creates\":%llu,\"fg_evaluations_seen\":%llu,\"recorded\":%u,\"completed\":%u,\"overflow\":%llu,\"failures\":%llu,\"hooks\":%u,\"queues\":%u,\"scope\":\"GPU timestamps surrounding NGX FrameGeneration EvaluateFeature on its supplied command list; elapsed includes preemption and internal barriers, excludes work dispatched on independent queues; not per-image scanout proof\"}",
            context.qpc_frequency, context.resolves, context.creates, context.evaluations,
            context.used, context.complete, context.overflow, context.failures, context.hook_count, context.queue_count);
        std::fclose(file);
    }
    if (context.events) std::fflush(context.events);
}
}

void install(const std::filesystem::path &runtime, const std::filesystem::path &logs)
{
    auto &context = data();
    context.logs = logs;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); context.qpc_frequency = frequency.QuadPart;
    context.events = _wfsopen((logs / L"fg-gpu-api.csv").c_str(), L"wb", _SH_DENYNO);
    context.trigger = CreateEventW(nullptr, TRUE, FALSE, (L"Local\\MGPU-FG-GpuTrace-" + std::to_wstring(GetCurrentProcessId())).c_str());
    context.common = LoadLibraryExW((runtime / L"sl.common.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (context.common) patch_imports(context.common);
    else ++context.failures;
#ifdef MGPU_FG_AUDIT_FOCUS_STUB
    const auto dlssg = LoadLibraryExW((runtime / L"sl.dlss_g.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (dlssg) patch_imports(dlssg);
    std::printf("[FG-GPU-AUDIT-TEST] synthetic foreground only inside test SDK imports; no desktop focus changes\n");
#endif
    std::printf("[FG-GPU-AUDIT] IAT hooks=%zu event=Local\\MGPU-FG-GpuTrace-%lu; no timestamps until explicit trigger\n",
        context.imports.size(), GetCurrentProcessId());
}

void attach_device(ID3D12Device *device)
{
    auto &context = data();
    context.device = device;
    for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE}) {
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC description{}; description.Type = type;
        if (FAILED(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue))) || !observe_queue(queue.Get())) ++context.failures;
    }
    if (!prepare()) ++context.failures;
}

void poll()
{
    auto &context = data();
    if (context.stopped) return;
    std::lock_guard<std::mutex> lock(context.mutex);
    bool changed = false;
    if (!context.attempted && (context.test_requested || (context.trigger && WaitForSingleObject(context.trigger, 0) == WAIT_OBJECT_0))) {
        context.attempted = true; context.deadline = GetTickCount64() + 10000;
        context.capturing.store(context.ready, std::memory_order_release); changed = true;
        write_event("capture_begin", context.ready, 0, 0);
    }
    if (context.capturing.load() && (GetTickCount64() >= context.deadline || context.used >= capacity ||
        (!context.test_requested && context.trigger && WaitForSingleObject(context.trigger, 0) != WAIT_OBJECT_0))) {
        context.capturing.store(false, std::memory_order_release); changed = true;
        write_event("capture_end", context.used, 0, 0);
    }
    for (unsigned index = 0; index < context.used; ++index) {
        auto &entry = context.records[index];
        if (entry.status != 4 || entry.queue >= context.queue_count) continue;
        const auto completed = context.queues[entry.queue].fence->GetCompletedValue();
        if (completed == UINT64_MAX) { entry.status = 7; ++context.failures; changed = true; continue; }
        if (completed < entry.fence_value) continue;
        entry.gpu_begin = context.mapped[index * 2]; entry.gpu_end = context.mapped[index * 2 + 1];
        if (!entry.gpu_begin || entry.gpu_end < entry.gpu_begin) { entry.status = 7; ++context.failures; }
        else { entry.status = 5; ++context.complete; }
        changed = true;
    }
    if (context.attempted && ((context.capturing.load() && GetTickCount64() - context.last_clock >= 100) ||
        (!context.capturing.load() && (!context.final_clock_taken || changed)))) {
        for (unsigned index = 0; index < context.queue_count; ++index) {
            auto &queue = context.queues[index];
            std::uint64_t gpu = 0, cpu = 0;
            const auto before = ticks();
            const auto result = queue.queue->GetClockCalibration(&gpu, &cpu);
            const auto after = ticks();
            if (context.events) std::fprintf(context.events, "clock,%llu,%lu,%u,%llu,%llu,%llu,%llu,%llu,%ld\n",
                before, GetCurrentThreadId(), index, gpu, cpu, queue.frequency, before, after, result);
        }
        context.last_clock = GetTickCount64();
        if (!context.capturing.load()) context.final_clock_taken = true;
    }
    if (changed && (!context.capturing.load() || GetTickCount64() - context.last_save >= 500)) {
        save(); context.last_save = GetTickCount64();
    }
}

std::uint64_t begin(ID3D12GraphicsCommandList *commands, std::uint64_t frame, unsigned count, unsigned index, unsigned mask)
{
    auto &context = data();
    if (!commands || !context.capturing.load(std::memory_order_acquire)) return 0;
    std::lock_guard<std::mutex> lock(context.mutex);
    if (!context.ready || context.used >= capacity) { ++context.overflow; return 0; }
    const auto type = commands->GetType();
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) { ++context.failures; return 0; }
    ComPtr<ID3D12Device> owner;
    if (FAILED(commands->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != context.device.Get()) { ++context.failures; return 0; }
    const auto slot = context.used++;
    auto &entry = context.records[slot];
    entry.id = slot + 1; entry.frame = frame; entry.count = count; entry.index = index;
    entry.parameter_mask = mask; entry.list = reinterpret_cast<std::uint64_t>(commands);
    entry.cpu_begin = ticks(); entry.call_sequence = present_sequence; entry.call_source = present_source;
    entry.thread = GetCurrentThreadId();
    commands->EndQuery(context.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
    entry.status = 1;
    return entry.id;
}

void end(std::uint64_t ticket, ID3D12GraphicsCommandList *commands, unsigned result)
{
    if (!ticket) return;
    auto &context = data(); std::lock_guard<std::mutex> lock(context.mutex);
    if (ticket > context.used) { ++context.failures; return; }
    auto &entry = context.records[ticket - 1];
    if (entry.status != 1 || entry.list != reinterpret_cast<std::uint64_t>(commands)) { ++context.failures; return; }
    entry.cpu_end = ticks(); entry.result = result;
    commands->EndQuery(context.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, unsigned(ticket - 1) * 2 + 1);
    commands->ResolveQueryData(context.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        unsigned(ticket - 1) * 2, 2, context.readback.Get(), (ticket - 1) * 2 * sizeof(std::uint64_t));
    entry.status = 2;
    context.pending.fetch_add(1, std::memory_order_release);
}

void present(std::uint64_t sequence, std::uint32_t source)
{
    present_sequence = sequence; present_source = source;
    std::lock_guard<std::mutex> lock(data().mutex);
    if (data().capturing.load()) write_event("present_begin", sequence, source, 0);
}
void finish_present()
{
    std::lock_guard<std::mutex> lock(data().mutex);
    if (data().capturing.load()) write_event("present_end", present_sequence, present_source, 0);
    present_sequence = present_source = 0;
}
void start_test_capture() { data().test_requested = true; poll(); }
unsigned completed_records() { std::lock_guard<std::mutex> lock(data().mutex); return data().complete; }
#ifdef MGPU_FG_AUDIT_FOCUS_STUB
void test_focus_window(HWND window) { synthetic_focus.store(window); }
#endif

void stop()
{
    auto &context = data();
    if (context.stopped) return;
    context.capturing.store(false, std::memory_order_release);
    poll();
    std::lock_guard<std::mutex> lock(context.mutex);
    save(); context.stopped = true;
    if (context.events) { std::fclose(context.events); context.events = nullptr; }
    if (context.trigger) { CloseHandle(context.trigger); context.trigger = nullptr; }
}
}
