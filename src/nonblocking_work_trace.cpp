#include "nonblocking_work_trace.hpp"
#include "frame_measurement.hpp"
#include "gpu_timestamp_span.hpp"
#include "native_work_observer.hpp"
#include "queue_dependency_trace.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mgpu::game_work::nonblocking
{
namespace
{
constexpr unsigned job_capacity = 32768, mark_capacity = 64;
constexpr unsigned fine_capacity = 4096, operation_capacity = 1024;
constexpr unsigned queue_capacity = 32, fine_stride = operation_capacity * 2;
const GUID job_guid = {0x68d1b672,0x8f23,0x49db,{0xa4,0x48,0xa9,0x30,0x78,0x04,0x43,0x62}};
const GUID device_guid = {0x445fe0b8,0x4b51,0x4160,{0x99,0x4d,0x45,0xc1,0x71,0x0a,0x79,0x03}};
enum class stage : unsigned { free, recording, closed, submitting, queued, submitted, emitted, cancelled, invalid };

template<unsigned Capacity>
class free_pool
{
    std::unique_ptr<std::array<std::atomic<unsigned>, Capacity>> next_;
    std::atomic<UINT64> head_{0};
public:
    void initialize(unsigned count = Capacity)
    {
        next_ = std::make_unique<std::array<std::atomic<unsigned>, Capacity>>();
        for (unsigned index = 0; index < count; ++index) (*next_)[index].store(index + 1 < count ? index + 2 : 0);
        head_.store(count ? 1 : 0, std::memory_order_release);
    }
    unsigned acquire()
    {
        auto head = head_.load(std::memory_order_acquire);
        for (unsigned attempt = 0; attempt < 64; ++attempt)
        {
            const auto slot = unsigned(head);
            if (!slot) return UINT_MAX;
            const auto next = (*next_)[slot - 1].load(std::memory_order_relaxed);
            const auto replacement = ((head >> 32) + 1) << 32 | next;
            if (head_.compare_exchange_weak(head, replacement, std::memory_order_acq_rel)) return slot - 1;
        }
        return UINT_MAX;
    }
    void release(unsigned slot)
    {
        auto head = head_.load(std::memory_order_acquire);
        do
        {
            (*next_)[slot].store(unsigned(head), std::memory_order_relaxed);
        } while (!head_.compare_exchange_weak(head, ((head >> 32) + 1) << 32 | (slot + 1), std::memory_order_release, std::memory_order_acquire));
    }
};

struct mark_record
{
    phase category = phase::unclassified;
    unsigned width = 0, height = 0;
    UINT64 pipeline = 0;
};
struct operation_record
{
    UINT64 pipeline = 0, object = 0, arg0 = 0, arg1 = 0, arg2 = 0;
    unsigned kind = 0;
};
struct fine_record { std::array<operation_record, operation_capacity> operations; };
struct job_record
{
    std::atomic<stage> status{stage::free};
    std::atomic<bool> reset_seen{false};
    std::atomic<bool> observed_work{false};
    std::atomic<UINT64> identity{0};
    ID3D12GraphicsCommandList *command = nullptr;
    unsigned storage = 0, fine_storage = UINT_MAX, type = 0, marks = 0, operations = 0;
    unsigned frame = 0, viewport = 0, frame_known = 0, width = 0, height = 0;
    unsigned overflow = 0, fine_open = UINT_MAX, seen = 0, scope_skipped = 0;
    bool truncated = false, native_start = false, native_end = false, fine_selected = false;
    phase scope = phase::unclassified;
    UINT64 pipeline = 0, ngx_handle = 0, fence_value = 0;
    unsigned queue = UINT_MAX;
    timing::calibration clock{};
    std::array<mark_record, mark_capacity> points{};
    std::array<unsigned, native_work::operation_count> counts{};
    std::array<unsigned, 4> callbacks{};
};
struct queue_record
{
    std::atomic<ID3D12CommandQueue *> identity{nullptr};
    std::atomic<bool> ready{false};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    UINT64 next = 0;
    UINT64 last_clock_query = 0;
    timing::queue_clock_cache clock;
};
struct query_storage
{
    static constexpr unsigned page_count = 262144;
    struct page
    {
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        UINT64 *values = nullptr;
    };
    std::array<page, 64> pages;
    std::atomic<unsigned> backed{0};
    unsigned maximum = 0;
    HRESULT last_result = S_OK;
    UINT64 retry_after = 0, growth_failures = 0;
    bool grow(ID3D12Device *device)
    {
        const auto count = backed.load(std::memory_order_relaxed);
        if (count >= maximum || timing::cpu_ticks() < retry_after) return false;
        page target;
        const auto fail = [&] {
            ++growth_failures;
            retry_after = timing::cpu_ticks() + timing::cpu_frequency();
            return false;
        };
        D3D12_QUERY_HEAP_DESC description{};
        description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        description.Count = std::min(page_count, maximum - count);
        last_result = device->CreateQueryHeap(&description, IID_PPV_ARGS(&target.heap));
        if (FAILED(last_result)) return fail();
        D3D12_HEAP_PROPERTIES memory{};
        memory.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC resource{};
        resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resource.Width = UINT64(description.Count) * sizeof(UINT64);
        resource.Height = resource.DepthOrArraySize = resource.MipLevels = resource.SampleDesc.Count = 1;
        resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        last_result = device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &resource,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&target.readback));
        if (FAILED(last_result)) return fail();
        const D3D12_RANGE range{0, SIZE_T(resource.Width)};
        last_result = target.readback->Map(0, &range, reinterpret_cast<void **>(&target.values));
        if (FAILED(last_result)) return fail();
        pages[count / page_count] = std::move(target);
        backed.store(count + description.Count, std::memory_order_release);
        return true;
    }
    bool initialize(ID3D12Device *device, unsigned count, unsigned initial)
    {
        maximum = count;
        while (backed.load(std::memory_order_relaxed) < initial)
            if (!grow(device)) return false;
        return true;
    }
    ID3D12QueryHeap *heap_at(unsigned index) const { return pages[index / page_count].heap.Get(); }
    ID3D12Resource *buffer_at(unsigned index) const { return pages[index / page_count].readback.Get(); }
    unsigned local_index(unsigned index) const { return index % page_count; }
    UINT64 *values_at(unsigned index) const { return pages[index / page_count].values + index % page_count; }
};
struct state
{
    std::atomic<bool> engaged{false}, ready{false}, enabled{false}, stopping{false}, drained{false}, finished{false};
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    std::unique_ptr<std::array<job_record, job_capacity>> jobs;
    std::unique_ptr<std::array<fine_record, fine_capacity>> fine;
    free_pool<job_capacity> available_jobs;
    free_pool<fine_capacity> available_fine;
    query_storage coarse_queries, fine_queries;
    std::array<queue_record, queue_capacity> queues;
    std::mutex initialization;
    std::thread worker;
    UINT64 session = 0, started = 0;
    HANDLE ready_event = nullptr;
    std::atomic<UINT64> generation{0}, created{0}, recycled{0}, cancelled{0}, rejected{0}, fine_refused{0};
    std::atomic<UINT64> emitted{0};
    std::atomic<UINT64> invalid{0}, replayed{0}, encoding_errors{0}, fine_truncated{0}, queue_contention{0};
    std::atomic<UINT64> growth_failures{0}, drain_timeouts{0}, stopped_at{0};
    std::atomic<unsigned> high_water{0}, live{0}, peak_live{0};
    std::atomic<unsigned> fine_live{0};
};
state &data() { static auto *context = new state; return *context; }
thread_local unsigned native_depth = 0;
thread_local unsigned submission_depth = 0;

void emit(const std::string &line) { measurement::emit(line.c_str()); }
void emit_parts(const std::string &line, const char *kind, UINT64 identity)
{
    if (line.size() < 1900) { emit(line); return; }
    const auto count = (line.size() + 1299) / 1300;
    if (count > 1024) { data().encoding_errors.fetch_add(1); return; }
    for (size_t index = 0; index < count; ++index)
    {
        char header[220];
        std::snprintf(header, sizeof header, "[MGPU][%s] kind=%s_part session=%llu job=%llu part=%zu parts=%zu payload=",
            kind[0] == 'j' ? "M337" : "M339", kind, data().session, identity, index, count);
        emit(std::string(header) + line.substr(index * 1300, 1300));
    }
}
void status(const char *reason)
{
    auto &context = data();
    char line[1400];
    std::snprintf(line, sizeof line,
        "[MGPU][M337] kind=status session=%llu reason=%s jobs=%u rejected=%llu replayed=%llu max_jobs=%u max_marks=%u "
        "invalid=%llu encoding_errors=%llu fine_selection_refused=%llu fine_operation_limit=%u query_stride=%u "
        "cumulative_jobs=%llu recycled=%llu cancelled_total=%llu live=%u peak_live=%u fine_truncated=%llu queue_contention=%llu "
        "continuous=1 nonblocking=1 hot_path_mutex=0 fine_pool=%u provenance=etw_only growth_failures=%llu drain_timeouts=%llu",
        context.session, reason, context.high_water.load(), context.rejected.load(), context.replayed.load(), job_capacity, mark_capacity,
        context.invalid.load(), context.encoding_errors.load(), context.fine_refused.load(), operation_capacity, fine_stride,
        context.created.load(), context.recycled.load(), context.cancelled.load(), context.live.load(), context.peak_live.load(),
        context.fine_truncated.load(), context.queue_contention.load(), fine_capacity, context.growth_failures.load(), context.drain_timeouts.load());
    measurement::emit(line);
}

job_record *find(ID3D12GraphicsCommandList *command)
{
    if (!command || !data().ready.load(std::memory_order_acquire)) return nullptr;
    UINT64 identity = 0;
    UINT bytes = sizeof identity;
    if (FAILED(command->GetPrivateData(job_guid, &bytes, &identity)) || bytes != sizeof identity || !identity) return nullptr;
    const unsigned slot = unsigned(identity);
    if (!slot || slot > job_capacity) return nullptr;
    auto &entry = (*data().jobs)[slot - 1];
    return entry.identity.load(std::memory_order_acquire) == identity ? &entry : nullptr;
}

bool eligible(ID3D12GraphicsCommandList *command)
{
    if (!command || !active()) return false;
    if (command->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && command->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE) return false;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(command->GetDevice(IID_PPV_ARGS(&device)))) return false;
    if (device.Get() == data().device.Get()) return true;
    UINT64 marker = 0;
    UINT bytes = sizeof marker;
    return SUCCEEDED(device->GetPrivateData(device_guid, &bytes, &marker)) && bytes == sizeof marker && marker == reinterpret_cast<UINT64>(&data());
}

job_record *record(ID3D12GraphicsCommandList *command)
{
    if (auto *entry = find(command)) return entry->status.load(std::memory_order_acquire) == stage::recording ? entry : nullptr;
    if (!eligible(command)) return nullptr;
    auto &context = data();
    const auto slot = context.available_jobs.acquire();
    if (slot == UINT_MAX) { context.rejected.fetch_add(1, std::memory_order_relaxed); return nullptr; }
    auto &entry = (*context.jobs)[slot];
    entry.command = command;
    entry.storage = slot;
    entry.fine_storage = UINT_MAX;
    entry.type = unsigned(command->GetType());
    entry.marks = entry.operations = entry.frame = entry.viewport = entry.frame_known = entry.width = entry.height = 0;
    entry.overflow = entry.seen = entry.scope_skipped = 0;
    entry.fine_open = UINT_MAX;
    entry.truncated = entry.native_start = entry.native_end = entry.fine_selected = false;
    entry.scope = phase::unclassified;
    entry.pipeline = entry.ngx_handle = entry.fence_value = 0;
    entry.queue = UINT_MAX;
    entry.clock = {};
    entry.counts.fill(0);
    entry.callbacks.fill(0);
    entry.reset_seen.store(false, std::memory_order_relaxed);
    entry.observed_work.store(false, std::memory_order_relaxed);
    const auto identity = (context.generation.fetch_add(1, std::memory_order_relaxed) + 1) << 32 | (slot + 1);
    entry.identity.store(identity, std::memory_order_release);
    if (FAILED(command->SetPrivateData(job_guid, sizeof identity, &identity)))
    {
        context.invalid.fetch_add(1);
        entry.status.store(stage::free, std::memory_order_release);
        context.available_jobs.release(slot);
        return nullptr;
    }
    entry.status.store(stage::recording, std::memory_order_release);
    auto high = context.high_water.load(std::memory_order_relaxed);
    while (high < slot + 1 && !context.high_water.compare_exchange_weak(high, slot + 1, std::memory_order_relaxed)) {}
    const auto live = context.live.fetch_add(1, std::memory_order_relaxed) + 1;
    auto peak = context.peak_live.load(std::memory_order_relaxed);
    while (peak < live && !context.peak_live.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
    context.created.fetch_add(1, std::memory_order_relaxed);
    return &entry;
}

void append(job_record &entry, phase category, UINT64 pipeline, unsigned width, unsigned height, bool force = false)
{
    if (category == phase::ngx_begin || category == phase::sl_sr_begin)
    {
        entry.observed_work.store(true, std::memory_order_release);
        if (entry.scope != phase::unclassified) { ++entry.overflow; return; }
        entry.scope = category;
        if (category == phase::ngx_begin) entry.ngx_handle = pipeline;
    }
    else if (entry.scope != phase::unclassified)
    {
        if (!((entry.scope == phase::ngx_begin && category == phase::ngx_end) ||
              (entry.scope == phase::sl_sr_begin && category == phase::sl_sr_end))) return;
        entry.scope = phase::unclassified;
    }
    if (category == phase::target_change) { entry.width = width; entry.height = height; return; }
    if (!force && (category == phase::graphics || category == phase::compute) && pipeline) { entry.pipeline = pipeline; return; }
    if (!force && entry.marks && entry.points[entry.marks - 1].category == category &&
        (category == phase::graphics || category == phase::compute || category == phase::indirect)) return;
    if (entry.marks >= mark_capacity - 1) { ++entry.overflow; return; }
    if (category == phase::graphics || category == phase::compute || category == phase::indirect)
    { pipeline = entry.pipeline; width = entry.width; height = entry.height; }
    const auto offset = entry.storage * mark_capacity + entry.marks;
    entry.command->EndQuery(data().coarse_queries.heap_at(offset), D3D12_QUERY_TYPE_TIMESTAMP, data().coarse_queries.local_index(offset));
    entry.points[entry.marks++] = {category, width, height, pipeline};
}

unsigned native_before(ID3D12GraphicsCommandList *command, native_work::operation kind, ID3D12Object *object, UINT64 arg0, UINT64 arg1, UINT64 arg2)
{
    if (!active()) return UINT_MAX;
    auto *entry = find(command);
    if (!entry || entry->status.load(std::memory_order_acquire) != stage::recording) return UINT_MAX;
    const auto kind_index = unsigned(kind) - 1;
    if (kind_index >= entry->counts.size()) return UINT_MAX;
    ++entry->seen;
    entry->observed_work.store(true, std::memory_order_release);
    ++entry->counts[kind_index];
    if (entry->scope != phase::unclassified) { ++entry->scope_skipped; return UINT_MAX; }
    if (entry->fine_storage == UINT_MAX)
    {
        entry->fine_storage = data().available_fine.acquire();
        if (entry->fine_storage == UINT_MAX) { data().fine_refused.fetch_add(1); return UINT_MAX; }
        data().fine_live.fetch_add(1, std::memory_order_relaxed);
        entry->fine_selected = true;
    }
    if (entry->operations >= operation_capacity || entry->fine_open != UINT_MAX)
    {
        if (!entry->truncated) data().fine_truncated.fetch_add(1);
        entry->truncated = true;
        return UINT_MAX;
    }
    const auto token = entry->operations++;
    auto &operation = (*data().fine)[entry->fine_storage].operations[token];
    operation = {entry->pipeline, reinterpret_cast<UINT64>(object), arg0, arg1, arg2, unsigned(kind)};
    entry->fine_open = token;
    const auto query = entry->fine_storage * fine_stride + token * 2;
    command->EndQuery(data().fine_queries.heap_at(query), D3D12_QUERY_TYPE_TIMESTAMP, data().fine_queries.local_index(query));
    return token;
}
void native_after(ID3D12GraphicsCommandList *command, unsigned token)
{
    auto *entry = find(command);
    if (!entry || entry->status.load(std::memory_order_acquire) != stage::recording || token != entry->fine_open || token >= entry->operations) return;
    const auto query = entry->fine_storage * fine_stride + token * 2 + 1;
    command->EndQuery(data().fine_queries.heap_at(query), D3D12_QUERY_TYPE_TIMESTAMP, data().fine_queries.local_index(query));
    entry->fine_open = UINT_MAX;
}
void native_pipeline(ID3D12GraphicsCommandList *command, ID3D12PipelineState *pipeline)
{
    if (auto *entry = find(command))
        if (entry->status.load(std::memory_order_acquire) == stage::recording) entry->pipeline = reinterpret_cast<UINT64>(pipeline);
}

using reset_fn = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);
using close_fn = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *);
struct native_hook
{
    std::atomic<void **> table{nullptr};
    std::atomic<reset_fn> original_reset{nullptr};
    std::atomic<close_fn> original_close{nullptr};
};
std::array<native_hook, 8> hooks{};
std::mutex hook_initialization;

bool hooked(ID3D12GraphicsCommandList *command)
{
    if (!command) return false;
    const auto table = *reinterpret_cast<void ***>(command);
    for (const auto &hook : hooks) if (hook.table.load(std::memory_order_acquire) == table) return true;
    return false;
}
void reset_generation(ID3D12GraphicsCommandList *command)
{
    auto *entry = find(command);
    if (!entry) return;
    const auto identity = entry->identity.load(std::memory_order_acquire);
    auto current = entry->status.load(std::memory_order_acquire);
    while ((current == stage::recording || current == stage::closed) &&
        !entry->status.compare_exchange_weak(current, stage::cancelled, std::memory_order_acq_rel)) {}
    if (current == stage::submitting) data().invalid.fetch_add(1);
    command->SetPrivateData(job_guid, 0, nullptr);
    native_work::select(command, false);
    if (entry->identity.load(std::memory_order_acquire) == identity) entry->reset_seen.store(true, std::memory_order_release);
}
template<unsigned Index>
HRESULT STDMETHODCALLTYPE native_reset(ID3D12GraphicsCommandList *command, ID3D12CommandAllocator *allocator, ID3D12PipelineState *pipeline)
{
    const bool outer = native_depth++ == 0;
    const auto result = hooks[Index].original_reset.load(std::memory_order_acquire)(command, allocator, pipeline);
    --native_depth;
    if (outer && SUCCEEDED(result) && engaged())
    {
        reset_generation(command);
        if (auto *entry = active() ? record(command) : nullptr)
        {
            entry->native_start = true;
            entry->pipeline = reinterpret_cast<UINT64>(pipeline);
            append(*entry, phase::unclassified, 0, 0, 0);
            if (!native_work::select(command, true)) data().fine_refused.fetch_add(1);
        }
    }
    return result;
}
template<unsigned Index>
HRESULT STDMETHODCALLTYPE native_close(ID3D12GraphicsCommandList *command)
{
    const bool outer = native_depth++ == 0;
    if (outer) close(command);
    const auto result = hooks[Index].original_close.load(std::memory_order_acquire)(command);
    --native_depth;
    if (outer && FAILED(result))
        if (auto *entry = find(command)) { data().invalid.fetch_add(1); entry->status.store(stage::invalid, std::memory_order_release); }
    return result;
}

unsigned queue_index(ID3D12CommandQueue *queue)
{
    if (!queue) return UINT_MAX;
    auto &context = data();
    for (unsigned index = 0; index < queue_capacity; ++index)
        if (context.queues[index].identity.load(std::memory_order_acquire) == queue) return index;
    for (unsigned index = 0; index < queue_capacity; ++index)
    {
        auto &entry = context.queues[index];
        if (entry.identity.load(std::memory_order_acquire) == queue) return index;
        ID3D12CommandQueue *empty = nullptr;
        if (!entry.identity.compare_exchange_strong(empty, queue, std::memory_order_acq_rel))
        { if (empty == queue) return index; continue; }
        entry.queue = queue;
        entry.ready.store(true, std::memory_order_release);
        return index;
    }
    return UINT_MAX;
}

void encode(job_record &entry)
{
    auto &context = data();
    const auto identity = entry.identity.load(std::memory_order_acquire);
    const auto *values = context.coarse_queries.values_at(entry.storage * mark_capacity);
    const auto frequency = entry.clock.gpu_frequency;
    if (!frequency || !entry.marks || !values[0]) { context.invalid.fetch_add(1); return; }
    for (unsigned index = 1; index < entry.marks; ++index)
        if (values[index] < values[index - 1]) { context.invalid.fetch_add(1); return; }
    char header[1600];
    std::snprintf(header, sizeof header,
        "[MGPU][M337] kind=job session=%llu job=%llu queue=%p type=%u engine_frame=%u frame_known=%u viewport=%u "
        "overflow=%u start_ms=%.6f frequency=%llu marks=%u ngx_handle=%llu ngx_feature=4294967295 ngx_known=0 "
        "reset_origin=0 submit_origin=0 native_start=%u native_end=%u command=%p fine_selected=%u fine_candidate=1 callback_record=%u data=",
        context.session, identity, context.queues[entry.queue].identity.load(), entry.type, entry.frame, entry.frame_known, entry.viewport,
        entry.overflow, entry.clock.cpu_ms(values[0]), frequency, entry.marks, entry.ngx_handle,
        unsigned(entry.native_start), unsigned(entry.native_end), entry.command, unsigned(entry.fine_selected),
        unsigned(std::any_of(entry.callbacks.begin(), entry.callbacks.end(), [](unsigned count) { return count != 0; })));
    std::string line = header;
    char encoded[220];
    for (unsigned index = 0; index < entry.marks; ++index)
    {
        const auto &point = entry.points[index];
        std::snprintf(encoded, sizeof encoded, "%s%u:%u:%u:%llx", index ? "," : "", unsigned(point.category), point.width, point.height, values[index] - values[0]);
        line += encoded;
    }
    emit_parts(line, "job", identity);
    if (entry.fine_selected)
    {
        std::snprintf(header, sizeof header,
            "[MGPU][M339] kind=fine session=%llu job=%llu count=%u truncated=%u mixed=1 timing=queries mixed_continues=1 data=",
            context.session, identity, entry.operations, unsigned(entry.truncated));
        line = header;
        const auto *fine_values = context.fine_queries.values_at(entry.fine_storage * fine_stride);
        for (unsigned index = 0; index < entry.operations; ++index)
        {
            const auto first = fine_values[index * 2], last = fine_values[index * 2 + 1];
            if (first < values[0] || last < first || last > values[entry.marks - 1]) { context.invalid.fetch_add(1); return; }
            const auto &operation = (*context.fine)[entry.fine_storage].operations[index];
            std::snprintf(encoded, sizeof encoded, "%s%llx:%llx:%u:0:%llx:%llx:%llx:%llx:%llx:0:0",
                index ? "," : "", first - values[0], last - values[0], operation.kind, operation.pipeline, operation.object,
                operation.arg0, operation.arg1, operation.arg2);
            line += encoded;
        }
        emit_parts(line, "fine", identity);
    }
    std::snprintf(header, sizeof header,
        "[MGPU][M339] kind=seen session=%llu job=%llu total=%u budget_skipped=0 scope_skipped=%u mixed=1 truncated=%u cutoff=0 mixed_continues=1 counts=",
        context.session, identity, entry.seen, entry.scope_skipped, unsigned(entry.truncated));
    line = header;
    for (unsigned index = 0; index < entry.counts.size(); ++index)
    { std::snprintf(encoded, sizeof encoded, "%s%u", index ? "," : "", entry.counts[index]); line += encoded; }
    emit(line);
}

void release(job_record &entry)
{
    auto &context = data();
    if (entry.fine_storage != UINT_MAX) { context.fine_live.fetch_sub(1, std::memory_order_relaxed); context.available_fine.release(entry.fine_storage); }
    entry.status.store(stage::free, std::memory_order_release);
    context.live.fetch_sub(1, std::memory_order_relaxed);
    context.recycled.fetch_add(1, std::memory_order_relaxed);
    context.available_jobs.release(entry.storage);
}
void worker_loop()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    auto &context = data();
    for (;;)
    {
        bool pending = false;
        const auto coarse_ready = context.coarse_queries.backed.load(std::memory_order_acquire) / mark_capacity;
        if (context.live.load(std::memory_order_relaxed) + 1024 > coarse_ready && context.coarse_queries.grow(context.device.Get()))
            for (unsigned slot = coarse_ready; slot < context.coarse_queries.backed.load() / mark_capacity; ++slot) context.available_jobs.release(slot);
        const auto fine_ready = context.fine_queries.backed.load(std::memory_order_acquire) / fine_stride;
        if (context.fine_live.load(std::memory_order_relaxed) + 256 > fine_ready && context.fine_queries.grow(context.device.Get()))
            for (unsigned slot = fine_ready; slot < context.fine_queries.backed.load() / fine_stride; ++slot) context.available_fine.release(slot);
        context.growth_failures.store(context.coarse_queries.growth_failures + context.fine_queries.growth_failures, std::memory_order_relaxed);
        std::array<std::vector<unsigned>, queue_capacity> queued;
        for (unsigned index = 0, high = context.high_water.load(std::memory_order_acquire); index < high; ++index)
        {
            auto &entry = (*context.jobs)[index];
            if (entry.status.load(std::memory_order_acquire) == stage::queued && entry.queue < queue_capacity)
                queued[entry.queue].push_back(index);
        }
        for (unsigned index = 0; index < queue_capacity; ++index)
        {
            auto &timeline = context.queues[index];
            if (queued[index].empty() || !timeline.ready.load(std::memory_order_acquire)) continue;
            if (!timeline.fence)
            {
                Microsoft::WRL::ComPtr<ID3D12Device> device;
                if (FAILED(timeline.queue->GetDevice(IID_PPV_ARGS(&device))) ||
                    FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&timeline.fence))))
                {
                    context.invalid.fetch_add(1);
                    for (auto slot : queued[index]) (*context.jobs)[slot].status.store(stage::invalid, std::memory_order_release);
                    continue;
                }
                measurement::queue(context.session, "m361_game_work", timeline.queue.Get());
            }
            const auto clock = timeline.clock.sample(timeline.queue.Get(), timing::cpu_ticks());
            if (clock.query_before != timeline.last_clock_query)
            {
                timeline.last_clock_query = clock.query_before;
                char line[550];
                std::snprintf(line, sizeof line,
                    "[MGPU][M360] kind=clock session=%llu queue=%p gpu=%llu cpu=%llu gpu_frequency=%llu cpu_frequency=%llu query_before=%llu query_after=%llu submitted_qpc=%llu",
                    context.session, timeline.queue.Get(), clock.gpu, clock.cpu, clock.gpu_frequency, clock.cpu_frequency,
                    clock.query_before, clock.query_after, timing::cpu_ticks());
                measurement::emit(line);
            }
            const auto value = ++timeline.next;
            const auto result = timeline.queue->Signal(timeline.fence.Get(), value);
            for (auto slot : queued[index])
            {
                auto &entry = (*context.jobs)[slot];
                if (entry.status.load(std::memory_order_acquire) != stage::queued) continue;
                entry.clock = clock;
                entry.fence_value = value;
                if (FAILED(result)) { context.invalid.fetch_add(1); entry.status.store(stage::invalid, std::memory_order_release); }
                else entry.status.store(stage::submitted, std::memory_order_release);
            }
        }
        std::array<UINT64, queue_capacity> completed{};
        for (unsigned index = 0; index < queue_capacity; ++index)
            if (context.queues[index].fence) completed[index] = context.queues[index].fence->GetCompletedValue();
        for (unsigned index = 0, high = context.high_water.load(std::memory_order_acquire); index < high; ++index)
        {
            auto &entry = (*context.jobs)[index];
            const auto current = entry.status.load(std::memory_order_acquire);
            if (current == stage::submitted)
            {
                pending = true;
                if (entry.queue >= queue_capacity || completed[entry.queue] == UINT64_MAX || completed[entry.queue] < entry.fence_value) continue;
                encode(entry);
                context.emitted.fetch_add(1, std::memory_order_relaxed);
                entry.status.store(stage::emitted, std::memory_order_release);
            }
            else if (current == stage::submitting || current == stage::queued) pending = true;
            else if (current == stage::closed || (current == stage::recording && entry.observed_work.load(std::memory_order_acquire))) pending = true;
            const auto final = entry.status.load(std::memory_order_acquire);
            if ((final == stage::emitted || final == stage::cancelled) && entry.reset_seen.load(std::memory_order_acquire))
            {
                if (final == stage::cancelled) context.cancelled.fetch_add(1, std::memory_order_relaxed);
                release(entry);
            }
        }
        if (context.stopping.load(std::memory_order_acquire) && !pending)
        {
            status("recording_stopped");
            const bool valid = !context.invalid.load() && !context.replayed.load() && !context.rejected.load() &&
                !context.fine_refused.load() && !context.encoding_errors.load() && !context.fine_truncated.load();
            status(valid ? "submitted_jobs_drained" : "trace_incomplete");
            context.drained.store(valid, std::memory_order_release);
            context.finished.store(true, std::memory_order_release);
            return;
        }
        const auto stopped = context.stopped_at.load(std::memory_order_acquire);
        if (stopped && timing::cpu_ticks() - stopped > 5 * timing::cpu_frequency())
        {
            context.drain_timeouts.fetch_add(1);
            status("trace_incomplete");
            context.finished.store(true, std::memory_order_release);
            return;
        }
        Sleep(1);
    }
}
}

bool engaged() { return data().engaged.load(std::memory_order_acquire); }
bool active() { return data().enabled.load(std::memory_order_acquire) && !data().stopping.load(std::memory_order_acquire); }
bool drained() { return data().drained.load(std::memory_order_acquire); }
bool finished() { return data().finished.load(std::memory_order_acquire); }
counters snapshot()
{
    auto &context = data();
    return {context.created.load(), context.emitted.load(), context.cancelled.load(), context.rejected.load(), context.invalid.load(),
            context.replayed.load(), context.fine_refused.load(), context.fine_truncated.load(), context.live.load(), context.peak_live.load(),
            context.coarse_queries.backed.load() / mark_capacity, context.fine_queries.backed.load() / fine_stride,
            context.growth_failures.load(), context.drain_timeouts.load()};
}
void bind_device(ID3D12Device *device)
{
    if (!device) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.initialization);
    if (context.device) return;
    context.device = device;
    const auto event_name = L"Local\\MGPUFramePipelineReady." + std::to_wstring(GetCurrentProcessId());
    context.ready_event = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
    const auto marker = reinterpret_cast<UINT64>(&context);
    device->SetPrivateData(device_guid, sizeof marker, &marker);
}
void poll(UINT64 session)
{
    auto &context = data();
    if (!engaged() && session)
    {
        context.engaged.store(true, std::memory_order_release);
        context.session = session;
        context.started = timing::cpu_ticks();
        try
        {
          context.worker = std::thread([] {
          SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
          auto &context = data();
          try
          {
            context.jobs = std::make_unique<std::array<job_record, job_capacity>>();
            context.fine = std::make_unique<std::array<fine_record, fine_capacity>>();
            context.available_jobs.initialize(0);
            context.available_fine.initialize(0);
            if (!context.device || !context.coarse_queries.initialize(context.device.Get(), job_capacity * mark_capacity, job_capacity * mark_capacity) ||
                !context.fine_queries.initialize(context.device.Get(), fine_capacity * fine_stride, fine_capacity * fine_stride))
            {
                char diagnostic[300];
                std::snprintf(diagnostic, sizeof diagnostic, "[MGPU][M361] kind=initialization_error device=%p coarse_hr=0x%08x fine_hr=0x%08x", context.device.Get(), unsigned(context.coarse_queries.last_result), unsigned(context.fine_queries.last_result));
                measurement::emit(diagnostic);
                context.invalid.fetch_add(1); status("query_initialization_failed"); status("trace_incomplete"); context.finished.store(true); return;
            }
            for (unsigned slot = context.coarse_queries.backed.load() / mark_capacity; slot > 0; --slot) context.available_jobs.release(slot - 1);
            for (unsigned slot = context.fine_queries.backed.load() / fine_stride; slot > 0; --slot) context.available_fine.release(slot - 1);
            native_work::set_continuous(true);
            native_work::set_running(true);
            context.ready.store(true, std::memory_order_release);
            context.enabled.store(!context.stopping.load(std::memory_order_acquire), std::memory_order_release);
            if (context.ready_event) SetEvent(context.ready_event);
            status("session_started_nonblocking");
            worker_loop();
          }
          catch (...)
          { context.invalid.fetch_add(1); context.enabled.store(false); status("trace_incomplete"); context.finished.store(true); }
          });
          context.worker.detach();
        }
        catch (...)
        { context.invalid.fetch_add(1); context.enabled.store(false); status("trace_incomplete"); context.finished.store(true); }
    }
    if (engaged() && !context.stopping.load(std::memory_order_acquire) &&
        (!session || session != context.session || timing::cpu_ticks() - context.started > 165 * timing::cpu_frequency()))
    {
        context.enabled.store(false, std::memory_order_release);
        native_work::set_running(false);
        context.stopped_at.store(timing::cpu_ticks(), std::memory_order_release);
        context.stopping.store(true, std::memory_order_release);
    }
}
void observe(ID3D12GraphicsCommandList *command)
{
    if (!active() || !command || hooked(command)) return;
    if (!eligible(command)) return;
    const auto coverage = native_work::install(command, native_before, native_after, native_pipeline);
    if (coverage.failures) { data().invalid.fetch_add(1); return; }
    std::unique_lock<std::mutex> lock(hook_initialization, std::try_to_lock);
    if (!lock.owns_lock()) { data().queue_contention.fetch_add(1); return; }
    const auto table = *reinterpret_cast<void ***>(command);
    for (const auto &hook : hooks) if (hook.table.load(std::memory_order_acquire) == table) return;
    static const reset_fn resets[] = {native_reset<0>,native_reset<1>,native_reset<2>,native_reset<3>,native_reset<4>,native_reset<5>,native_reset<6>,native_reset<7>};
    static const close_fn closes[] = {native_close<0>,native_close<1>,native_close<2>,native_close<3>,native_close<4>,native_close<5>,native_close<6>,native_close<7>};
    for (unsigned index = 0; index < hooks.size(); ++index)
    {
        auto &hook = hooks[index];
        if (hook.table.load()) continue;
        DWORD protection = 0;
        if (!VirtualProtect(table + 9, sizeof(void *) * 2, PAGE_READWRITE, &protection)) { data().invalid.fetch_add(1); return; }
        hook.original_reset.store(reinterpret_cast<reset_fn>(table[10]), std::memory_order_release);
        hook.original_close.store(reinterpret_cast<close_fn>(table[9]), std::memory_order_release);
        InterlockedExchangePointer(table + 9, reinterpret_cast<void *>(closes[index]));
        InterlockedExchangePointer(table + 10, reinterpret_cast<void *>(resets[index]));
        DWORD ignored = 0;
        if (!VirtualProtect(table + 9, sizeof(void *) * 2, protection, &ignored)) data().invalid.fetch_add(1);
        hook.table.store(table, std::memory_order_release);
        return;
    }
    data().invalid.fetch_add(1);
}
void reset(ID3D12GraphicsCommandList *command) { if (!hooked(command)) reset_generation(command); }
void destroy(ID3D12GraphicsCommandList *command) { reset_generation(command); }
void close(ID3D12GraphicsCommandList *command)
{
    if (native_depth == 0 && hooked(command)) return;
    auto *entry = find(command);
    if (!entry || entry->status.load(std::memory_order_acquire) != stage::recording) return;
    native_work::select(command, false);
    if (!entry->marks) append(*entry, phase::unclassified, 0, 0, 0);
    if (entry->fine_open != UINT_MAX || entry->scope != phase::unclassified) ++entry->overflow;
    const auto tail = entry->storage * mark_capacity + entry->marks;
    command->EndQuery(data().coarse_queries.heap_at(tail), D3D12_QUERY_TYPE_TIMESTAMP, data().coarse_queries.local_index(tail));
    entry->points[entry->marks++] = {phase::tail};
    const auto coarse_start = entry->storage * mark_capacity;
    command->ResolveQueryData(data().coarse_queries.heap_at(coarse_start), D3D12_QUERY_TYPE_TIMESTAMP, data().coarse_queries.local_index(coarse_start),
        entry->marks, data().coarse_queries.buffer_at(coarse_start), UINT64(data().coarse_queries.local_index(coarse_start)) * sizeof(UINT64));
    if (entry->operations && entry->fine_open == UINT_MAX)
    {
        const auto fine_start = entry->fine_storage * fine_stride;
        command->ResolveQueryData(data().fine_queries.heap_at(fine_start), D3D12_QUERY_TYPE_TIMESTAMP, data().fine_queries.local_index(fine_start),
            entry->operations * 2, data().fine_queries.buffer_at(fine_start), UINT64(data().fine_queries.local_index(fine_start)) * sizeof(UINT64));
    }
    entry->native_end = native_depth != 0;
    entry->status.store(stage::closed, std::memory_order_release);
}
void marker(ID3D12GraphicsCommandList *command, phase category, UINT64 pipeline, unsigned width, unsigned height)
{
    if (!active())
    {
        if (category == phase::ngx_end || category == phase::sl_sr_end)
            if (auto *entry = find(command))
                if (entry->status.load(std::memory_order_acquire) == stage::recording && entry->scope != phase::unclassified)
                    append(*entry, category, pipeline, width, height);
        return;
    }
    if (auto *entry = record(command)) append(*entry, category, pipeline, width, height);
}
void tag(ID3D12GraphicsCommandList *command, unsigned frame, unsigned viewport, unsigned type, unsigned width, unsigned height)
{
    if (!active()) return;
    if (auto *entry = record(command))
    {
        entry->observed_work.store(true, std::memory_order_release);
        if (entry->frame_known && (entry->frame != frame || entry->viewport != viewport)) entry->frame_known = 2;
        else if (!entry->frame_known) { entry->frame = frame; entry->viewport = viewport; entry->frame_known = 1; }
        append(*entry, phase::capture_tag, type, width, height, true);
    }
}
void callback_marker(ID3D12GraphicsCommandList *command, unclassified_callback category)
{
    if (!active()) return;
    if (auto *entry = record(command))
    { if (unsigned(category) < entry->callbacks.size()) ++entry->callbacks[unsigned(category)]; }
}
void before_submit(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    if (submission_depth++ != 0) return;
    if (!engaged() || !lists) return;
    for (unsigned index = 0; index < count; ++index)
    {
        auto *command = static_cast<ID3D12GraphicsCommandList *>(lists[index]);
        auto *entry = find(command);
        if (!entry) continue;
        auto expected = stage::closed;
        if (!entry->status.compare_exchange_strong(expected, stage::submitting, std::memory_order_acq_rel))
        {
            if (expected == stage::submitted || expected == stage::emitted || expected == stage::submitting || expected == stage::queued)
            { data().replayed.fetch_add(1); entry->status.store(stage::invalid, std::memory_order_release); }
            continue;
        }
        queue_trace::job(queue, command, data().session, entry->identity.load(std::memory_order_acquire));
    }
}
void after_submit(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    if (submission_depth && --submission_depth != 0) return;
    if (!engaged() || !lists) return;
    unsigned slot = UINT_MAX;
    for (unsigned index = 0; index < count; ++index)
        if (auto *entry = find(static_cast<ID3D12GraphicsCommandList *>(lists[index])))
            if (entry->status.load(std::memory_order_acquire) == stage::submitting) { slot = queue_index(queue); break; }
    if (slot == UINT_MAX)
    {
        for (unsigned index = 0; index < count; ++index)
            if (auto *entry = find(static_cast<ID3D12GraphicsCommandList *>(lists[index])))
            {
                auto expected = stage::submitting;
                if (entry->status.compare_exchange_strong(expected, stage::invalid, std::memory_order_acq_rel)) data().invalid.fetch_add(1);
            }
        return;
    }
    for (unsigned index = 0; index < count; ++index)
    {
        auto *entry = find(static_cast<ID3D12GraphicsCommandList *>(lists[index]));
        if (!entry || entry->status.load(std::memory_order_acquire) != stage::submitting) continue;
        entry->queue = slot;
        entry->status.store(stage::queued, std::memory_order_release);
    }
}
}
