#include "game_work_trace.hpp"
#include "nonblocking_work_trace.hpp"
#include "game_work_policy.hpp"
#include "frame_measurement.hpp"
#include "queue_dependency_trace.hpp"
#include "gpu_timestamp_span.hpp"
#include "native_work_observer.hpp"
#include "native_work_policy.hpp"
#include "trace_log_parts.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace mgpu::game_work
{
namespace
{
constexpr unsigned max_jobs = 8192;
constexpr unsigned max_marks = 64;
constexpr unsigned query_stride = native_work::timestamp_limit;
constexpr unsigned max_queues = 16;
const GUID trace_device_guid = {0x6d3388f9,0x2776,0x46a7,{0xac,0xcf,0x09,0xd4,0xb1,0xc8,0x05,0x33}};
const GUID trace_command_guid = {0x928eba5f,0x9558,0x479d,{0x92,0xa5,0x87,0xd3,0xfe,0x6b,0x67,0xa1}};
struct mark
{
    phase category = phase::unclassified;
    UINT64 pipeline = 0;
    unsigned width = 0, height = 0, operations = 0;
};
struct fine_operation
{
    unsigned begin=0,end=UINT_MAX,kind=0,origin=0;
    unsigned pipeline_label=0,object_label=0;
    UINT64 pipeline=0,object=0,arg0=0,arg1=0,arg2=0;
};
struct object_label
{
    UINT64 object=0;
    unsigned encoding=0;
    std::vector<unsigned char> bytes;
    bool emitted=false;
};
struct job
{
    record_state status{};
    UINT64 session = 0, id = 0, recorded_at = 0;
    unsigned storage = 0;
    bool reusable = false, reset_seen = false;
    ID3D12GraphicsCommandList *command = nullptr;
    unsigned type = 0, frame = 0, viewport = 0, frame_known = 0;
    UINT64 ngx_handle = 0;
    unsigned ngx_feature = UINT_MAX, ngx_known = 0;
    unsigned reset_origin = 0, submit_origin = 0;
    bool native_start = false, native_end = false;
    unsigned marks = 0, mark_overflow = 0, queue = UINT_MAX;
    UINT64 current_pipeline = 0;
    ID3D12PipelineState *native_pipeline_state = nullptr;
    unsigned target_width = 0, target_height = 0;
    UINT64 fence_value = 0;
    timing::calibration clock{};
    std::array<mark, max_marks> points{};
    std::array<UINT64, query_stride> values{};
    std::vector<ID3D12GraphicsCommandList *> aliases;
    phase scope = phase::unclassified;
    bool observed_scope = false;
    bool emitted = false, collected = false;
    bool encoding_prepared=false;
    trace_log_parts output;
    bool fine_candidate=false,fine_selected=false,fine_truncated=false,fine_mixed=false;
    unsigned fine_count=0,fine_open=UINT_MAX;
    std::array<unsigned,native_work::operation_count> fine_seen{};
    unsigned fine_seen_total=0,fine_budget_skipped=0,fine_scope_skipped=0;
    bool fine_seen_cutoff=false;
    std::array<unsigned,4> callback_counts{},callback_origins{};
    std::array<unsigned,4> callback_points{{UINT_MAX,UINT_MAX,UINT_MAX,UINT_MAX}};
    unsigned callback_total=0;
    std::array<fine_operation,native_work::operation_limit> fine{};
};
struct queue_record
{
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    UINT64 next = 0;
    timing::queue_clock_cache cache;
    timing::calibration clock{};
};
struct aggregate
{
    UINT64 queue = 0;
    unsigned type = 0;
    unsigned jobs = 0;
    unsigned frame_known = 0;
    unsigned frame_conflict = 0;
    unsigned overflow_jobs = 0;
    unsigned submitted_lists = 0, traced_lists = 0;
    std::array<unsigned, 13> segments{};
    std::array<double, 13> gpu_ms{};
};
struct state
{
    std::mutex mutex;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    UINT64 *mapped = nullptr;
    LUID luid{};
    UINT64 session = 0, started = 0;
    UINT64 refused_session = 0;
    unsigned cursor = 0, rejected = 0, replayed = 0;
    UINT64 generation = 0, cumulative_jobs = 0, recycled = 0;
    bool continuous_mode = false;
    std::vector<unsigned> free_slots;
    unsigned invalid_timestamps = 0, encoding_errors = 0;
    std::array<std::unique_ptr<job>, max_jobs> jobs{};
    std::unordered_map<ID3D12GraphicsCommandList *, unsigned> command_aliases;
    std::unordered_map<UINT64, unsigned> features;
    std::vector<std::array<UINT64, 8>> origins;
    std::array<queue_record, max_queues> queues{};
    std::array<aggregate, max_queues> aggregates{};
    bool attempted = false, failed = false;
    bool stopped_reported = false, drained_reported = false, aggregates_reported = false;
    unsigned native_starts = 0, native_closes = 0, native_reset_failures = 0;
    native_work::selection_policy detail_policy;
    unsigned fine_selection_refused=0;
    std::vector<object_label> labels;
};
bool safe_to_reuse(const job &entry)
{
    return entry.reset_seen && (entry.status.state == lifecycle::cancelled ||
        (entry.status.state == lifecycle::retired && entry.collected && entry.emitted));
}

void release_slot(state &context, job &entry)
{
    if (context.continuous_mode && !entry.reusable && safe_to_reuse(entry))
    {
        entry.reusable = true;
        context.free_slots.push_back(entry.storage);
    }
}
std::atomic<bool> enabled{false};
std::atomic<bool> tracked{false};
state &data() { static auto *value = new state; return *value; }

void report_status(state &context, const char *reason)
{
    unsigned submitted = 0, retired = 0, recording = 0, closed = 0, cancelled = 0, invalid = 0, overflow = 0, scope_pending = 0;
    for (unsigned index = 0; index < context.cursor; ++index)
    {
        const auto &entry = *context.jobs[index];
        if (entry.session != context.session) continue;
        submitted += entry.status.state == lifecycle::submitted;
        retired += entry.status.state == lifecycle::retired;
        recording += entry.status.state == lifecycle::recording;
        closed += entry.status.state == lifecycle::closed;
        cancelled += entry.status.state == lifecycle::cancelled;
        invalid += entry.status.state == lifecycle::invalid;
        overflow += entry.mark_overflow != 0;
        scope_pending += entry.observed_scope && (entry.status.state==lifecycle::recording || entry.status.state==lifecycle::closed);
    }
    char line[1200];
    std::snprintf(line, sizeof line,
        "[MGPU][M337] kind=status session=%llu reason=%s jobs=%u rejected=%u replayed=%u max_jobs=%u max_marks=%u "
        "recording=%u closed_unsubmitted=%u submitted_pending=%u retired=%u cancelled=%u invalid=%u overflow_jobs=%u invalid_timestamps=%u encoding_errors=%u native_starts=%u native_closes=%u native_reset_failures=%u native_boundary_mode=1 scope_pending=%u fine_selected=%u fine_general=%u fine_meta=%u fine_selection_refused=%u fine_general_limit=%u fine_meta_limit=%u fine_job_limit=%u fine_operation_limit=%u query_stride=%u fine_mixed_continues=1 cumulative_jobs=%llu recycled=%llu continuous=%u",
        context.session, reason, context.cursor, context.rejected, context.replayed, max_jobs, max_marks,
        recording, closed, submitted, retired, cancelled, invalid, overflow, context.invalid_timestamps, context.encoding_errors,
        context.native_starts,context.native_closes,context.native_reset_failures,scope_pending,context.detail_policy.selected,
        context.detail_policy.general_selected,context.detail_policy.meta_selected,context.fine_selection_refused,
        native_work::general_job_limit,native_work::meta_job_limit,native_work::job_limit,native_work::operation_limit,query_stride,
        context.cumulative_jobs, context.recycled, unsigned(context.continuous_mode));
    measurement::emit(line);
}

bool initialize(state &context)
{
    if (context.attempted) return context.mapped != nullptr;
    context.attempted = true;
    D3D12_QUERY_HEAP_DESC queries{};
    queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queries.Count = max_jobs * query_stride;
    HRESULT result = context.device->CreateQueryHeap(&queries, IID_PPV_ARGS(&context.queries));
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = UINT64(max_jobs) * query_stride * sizeof(UINT64);
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(result)) result = context.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&context.readback));
    if (SUCCEEDED(result))
    {
        const D3D12_RANGE range{0, SIZE_T(buffer.Width)};
        result = context.readback->Map(0, &range, reinterpret_cast<void **>(&context.mapped));
    }
    if (FAILED(result)) report_status(context, "query_initialization_failed");
    return SUCCEEDED(result) && context.mapped != nullptr;
}

bool eligible(state &context, ID3D12GraphicsCommandList *command)
{
    if (command == nullptr || context.failed || !context.device || !context.mapped) return false;
    const auto type = command->GetType();
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return false;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(command->GetDevice(IID_PPV_ARGS(&device)))) return false;
    const auto luid = device->GetAdapterLuid();
    if (luid.LowPart != context.luid.LowPart || luid.HighPart != context.luid.HighPart) return false;
    Microsoft::WRL::ComPtr<IUnknown> observed, bound;
    if (FAILED(device.As(&observed)) || FAILED(context.device.As(&bound))) return false;
    if (observed.Get() == bound.Get()) return true;
    UINT64 marker = 0;
    UINT bytes = sizeof marker;
    return SUCCEEDED(device->GetPrivateData(trace_device_guid,&bytes,&marker)) && bytes == sizeof marker &&
        marker == reinterpret_cast<UINT64>(&context);
}

job *find_job(state &context, ID3D12GraphicsCommandList *command)
{
    if (command == nullptr) return nullptr;
    const auto cached = context.command_aliases.find(command);
    if (cached != context.command_aliases.end()) return context.jobs[cached->second].get();
    UINT64 id = 0;
    UINT bytes = sizeof id;
    if (SUCCEEDED(command->GetPrivateData(trace_command_guid, &bytes, &id)) && bytes == sizeof id &&
        id != 0 && unsigned(id) != 0 && unsigned(id) <= context.cursor)
    {
        const auto slot = unsigned(id) - 1;
        auto *entry = context.jobs[slot].get();
        if (entry->id != id) return nullptr;
        context.command_aliases[command] = slot;
        entry->aliases.push_back(command);
        return entry;
    }
    return nullptr;
}

job *record(state &context, ID3D12GraphicsCommandList *command)
{
    if (auto *entry = find_job(context, command))
        return entry->session == context.session && entry->status.state == lifecycle::recording ? entry : nullptr;
    if (!eligible(context, command)) return nullptr;
    unsigned slot = context.cursor;
    if (context.cursor == max_jobs && context.continuous_mode && !context.free_slots.empty())
    {
        slot = context.free_slots.back();
        context.free_slots.pop_back();
        auto &previous = *context.jobs[slot];
        if (!safe_to_reuse(previous))
        {
            ++context.rejected;
            return nullptr;
        }
        for (auto *alias : previous.aliases) context.command_aliases.erase(alias);
        ++context.recycled;
    }
    else if (context.cursor == max_jobs)
    {
        if (context.rejected++ == 0) report_status(context, "record_budget_exhausted");
        return nullptr;
    }
    else ++context.cursor;
    context.jobs[slot] = std::make_unique<job>();
    auto &entry = *context.jobs[slot];
    entry.session = context.session;
    entry.storage = slot;
    entry.id = context.continuous_mode ? (++context.generation << 32) | (slot + 1) : slot + 1;
    ++context.cumulative_jobs;
    entry.command = command;
    entry.type = unsigned(command->GetType());
    entry.recorded_at = timing::cpu_ticks();
    if (FAILED(command->SetPrivateData(trace_command_guid,sizeof entry.id,&entry.id)))
    {
        entry.status.state = lifecycle::invalid;
        ++context.rejected;
        return nullptr;
    }
    entry.aliases.push_back(command);
    context.command_aliases[command] = slot;
    tracked.store(true, std::memory_order_release);
    return &entry;
}

unsigned capture_origin(state &context)
{
    void *addresses[8]{};
    const auto count = CaptureStackBackTrace(2,8,addresses,nullptr);
    std::array<UINT64,8> key{};
    for (unsigned index=0;index<count;++index) key[index]=reinterpret_cast<UINT64>(addresses[index]);
    const auto found=std::find(context.origins.begin(),context.origins.end(),key);
    if (found!=context.origins.end()) return unsigned(found-context.origins.begin())+1;
    if (context.origins.size()>=64) return 0;
    context.origins.push_back(key);
    char line[500];
    std::snprintf(line,sizeof line,
        "[MGPU][M337] kind=origin session=%llu origin=%u pcs=%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx",
        context.session,unsigned(context.origins.size()),key[0],key[1],key[2],key[3],key[4],key[5],key[6],key[7]);
    measurement::emit(line);
    return unsigned(context.origins.size());
}

void append(state &context, job &entry, phase category, UINT64 pipeline, unsigned width, unsigned height,bool force=false)
{
    if (category == phase::sl_sr_begin || category == phase::ngx_begin)
    {
        if (entry.scope != phase::unclassified) { ++entry.mark_overflow; return; }
        entry.scope = category;
        entry.observed_scope = true;
        if (category == phase::ngx_begin)
        {
            const auto feature = context.features.find(pipeline);
            if (entry.ngx_handle != 0 && (entry.ngx_handle != pipeline ||
                (feature != context.features.end() && entry.ngx_known == 1 && entry.ngx_feature != feature->second))) entry.ngx_known = 2;
            else if (entry.ngx_handle == 0)
            {
                entry.ngx_handle = pipeline;
                if (feature != context.features.end()) { entry.ngx_feature = feature->second; entry.ngx_known = 1; }
            }
        }
    }
    else if (entry.scope != phase::unclassified)
    {
        const bool ending = (entry.scope == phase::sl_sr_begin && category == phase::sl_sr_end) ||
            (entry.scope == phase::ngx_begin && category == phase::ngx_end);
        if (!ending) return;
        entry.scope = phase::unclassified;
    }
    const bool metadata = !force && (category == phase::target_change ||
        ((category == phase::graphics || category == phase::compute) && pipeline != 0));
    if (metadata && entry.marks == 0)
    {
        const auto index = entry.storage*query_stride;
        entry.command->EndQuery(context.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index);
        entry.points[entry.marks++] = {phase::unclassified};
    }
    if (category == phase::target_change) { entry.target_width = width; entry.target_height = height; return; }
    if (!force && (category == phase::graphics || category == phase::compute) && pipeline != 0)
    {
        entry.current_pipeline = pipeline;
        return;
    }
    if (category == phase::graphics || category == phase::compute || category == phase::indirect)
    {
        pipeline = entry.current_pipeline;
        width = entry.target_width;
        height = entry.target_height;
    }
    if (!force && entry.marks != 0)
    {
        auto &last = entry.points[entry.marks-1];
        if (last.category == category && last.width == width && last.height == height &&
            (category == phase::graphics || category == phase::compute || category == phase::indirect || last.pipeline == pipeline))
        {
            ++last.operations;
            if (last.pipeline != pipeline) last.pipeline = UINT64_MAX;
            return;
        }
    }
    if (entry.marks >= max_marks-1)
    {
        ++entry.mark_overflow;
        return;
    }
    const auto index = entry.storage*query_stride + entry.marks;
    entry.command->EndQuery(context.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    entry.points[entry.marks++] = {category, pipeline, width, height, 1};
}

unsigned label_for(state &context,ID3D12Object *object)
{
    if (!object) return 0;
    const auto identity=reinterpret_cast<UINT64>(object);
    for (unsigned index=0;index<context.labels.size();++index)
        if (context.labels[index].object==identity) return index+1;
    if (context.labels.size()>=64) return 0;
    object_label label;
    label.object=identity;
    unsigned char bytes[96]{};
    UINT length=sizeof bytes;
    if (SUCCEEDED(object->GetPrivateData(WKPDID_D3DDebugObjectName,&length,bytes)) && length>0 && length<=sizeof bytes)
        label.encoding=1;
    else
    {
        length=sizeof bytes;
        if (SUCCEEDED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW,&length,bytes)) && length>0 && length<=sizeof bytes && length%2==0)
            label.encoding=2;
    }
    if (label.encoding) label.bytes.assign(bytes,bytes+length);
    context.labels.push_back(std::move(label));
    return unsigned(context.labels.size());
}

unsigned native_before(ID3D12GraphicsCommandList *command,native_work::operation kind,
    ID3D12Object *object,UINT64 arg0,UINT64 arg1,UINT64 arg2)
{
    auto &context=data();
    std::lock_guard<std::mutex> lock(context.mutex);
    auto *entry=find_job(context,command);
    if (!active() || !entry || !entry->fine_candidate || entry->status.state!=lifecycle::recording) return UINT_MAX;
    const auto kind_index=unsigned(kind)-1;
    if (kind_index>=entry->fine_seen.size()) return UINT_MAX;
    ++entry->fine_seen[kind_index];
    ++entry->fine_seen_total;
    if (entry->scope!=phase::unclassified) { ++entry->fine_scope_skipped; return UINT_MAX; }
    if (entry->fine_count==0 && !entry->fine_selected)
    {
        const bool meta=kind==native_work::operation::meta_initialize || kind==native_work::operation::meta_execute;
        if (!context.continuous_mode && !context.detail_policy.claim(meta)) { ++entry->fine_budget_skipped; return UINT_MAX; }
        entry->fine_selected=true;
    }
    if (entry->fine_count==native_work::operation_limit || entry->fine_open!=UINT_MAX)
    {
        entry->fine_truncated=true;
        native_work::select(command,false);
        return UINT_MAX;
    }
    const auto token=entry->fine_count++;
    auto &operation=entry->fine[token];
    operation.kind=unsigned(kind);
    operation.origin=capture_origin(context);
    operation.pipeline=reinterpret_cast<UINT64>(entry->native_pipeline_state);
    operation.object=reinterpret_cast<UINT64>(object);
    operation.pipeline_label=label_for(context,entry->native_pipeline_state);
    operation.object_label=label_for(context,object);
    operation.arg0=arg0; operation.arg1=arg1; operation.arg2=arg2;
    operation.begin=max_marks+token*2;
    entry->fine_open=token;
    entry->observed_scope=true;
    command->EndQuery(context.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,entry->storage*query_stride+operation.begin);
    return token;
}

void native_after(ID3D12GraphicsCommandList *command,unsigned token)
{
    auto &context=data();
    std::lock_guard<std::mutex> lock(context.mutex);
    auto *entry=find_job(context,command);
    if (!entry || entry->status.state!=lifecycle::recording || token!=entry->fine_open || token>=entry->fine_count) return;
    entry->fine[token].end=entry->fine[token].begin+1;
    command->EndQuery(context.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,entry->storage*query_stride+entry->fine[token].end);
    entry->fine_open=UINT_MAX;
}

void native_pipeline(ID3D12GraphicsCommandList *command,ID3D12PipelineState *pipeline)
{
    auto &context=data();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (auto *entry=find_job(context,command))
        if (entry->status.state==lifecycle::recording) entry->native_pipeline_state=pipeline;
}

using reset_fn=HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *,ID3D12CommandAllocator *,ID3D12PipelineState *);
using close_fn=HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *);
struct native_hook
{
    std::atomic<void **> table{nullptr};
    std::atomic<reset_fn> original_reset{nullptr};
    std::atomic<close_fn> original_close{nullptr};
};
std::array<native_hook,8> native_hooks{};
std::mutex native_hook_mutex;
thread_local unsigned native_depth=0;

bool native_observed(ID3D12GraphicsCommandList *command)
{
    if (command==nullptr) return false;
    auto **table=*reinterpret_cast<void ***>(command);
    for (const auto &hook:native_hooks)
        if (hook.table.load(std::memory_order_acquire)==table) return true;
    return false;
}

template<unsigned Index>
HRESULT STDMETHODCALLTYPE native_reset(ID3D12GraphicsCommandList *command,
    ID3D12CommandAllocator *allocator,ID3D12PipelineState *pipeline)
{
    const bool outer=native_depth++==0;
    const auto original=native_hooks[Index].original_reset.load(std::memory_order_acquire);
    const auto result=original(command,allocator,pipeline);
    --native_depth;
    if (outer && (active() || tracked.load(std::memory_order_acquire)))
    {
        auto &context=data();
        std::lock_guard<std::mutex> lock(context.mutex);
        if (active() && !eligible(context,command)) return result;
        if (SUCCEEDED(result))
        {
            native_work::select(command,false);
            if (auto *old=find_job(context,command))
            {
                old->status.reset();
                old->reset_seen = true;
                release_slot(context, *old);
                for (auto *alias:old->aliases) context.command_aliases.erase(alias);
                old->aliases.clear();
                command->SetPrivateData(trace_command_guid,0,nullptr);
            }
            if (auto *entry=active() ? record(context,command) : nullptr)
            {
                append(context,*entry,phase::unclassified,0,0,0);
                entry->reset_origin=capture_origin(context);
                entry->native_start=true;
                entry->current_pipeline=reinterpret_cast<UINT64>(pipeline);
                entry->native_pipeline_state=pipeline;
                if (context.continuous_mode || context.detail_policy.candidate(entry->reset_origin))
                {
                    entry->fine_candidate=native_work::select(command,true);
                    if (!entry->fine_candidate) ++context.fine_selection_refused;
                }
                ++context.native_starts;
            }
        }
        else if (active()) ++context.native_reset_failures;
    }
    return result;
}

template<unsigned Index>
HRESULT STDMETHODCALLTYPE native_close(ID3D12GraphicsCommandList *command)
{
    const bool outer=native_depth++==0;
    if (outer)
    {
        close(command);
    }
    const auto original=native_hooks[Index].original_close.load(std::memory_order_acquire);
    const auto result=original(command);
    --native_depth;
    if (outer && FAILED(result))
    {
        auto &context=data();
        std::lock_guard<std::mutex> lock(context.mutex);
        if (auto *entry=find_job(context,command)) entry->status.state=lifecycle::invalid;
    }
    return result;
}

unsigned queue_index(state &context, ID3D12CommandQueue *queue)
{
    for (unsigned index = 0; index < max_queues; ++index)
        if (context.queues[index].queue.Get() == queue) return index;
    for (unsigned index = 0; index < max_queues; ++index)
    {
        auto &entry = context.queues[index];
        if (entry.queue) continue;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return UINT_MAX;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&entry.fence)))) return UINT_MAX;
        entry.queue = queue;
        measurement::queue(context.session, "m337_game_work", queue);
        return index;
    }
    return UINT_MAX;
}
}

bool active() { return nonblocking::engaged() ? nonblocking::active() : enabled.load(std::memory_order_acquire); }

void observe_native_list(ID3D12GraphicsCommandList *command)
{
    if (nonblocking::engaged()) { nonblocking::observe(command); return; }
    if (!active() || command==nullptr) return;
    {
        auto &context=data();
        std::lock_guard<std::mutex> lock(context.mutex);
        if (!eligible(context,command)) return;
    }
    const auto coverage=native_work::install(command,native_before,native_after,native_pipeline);
    static std::atomic<unsigned> coverage_errors{0};
    if (coverage.new_table || (coverage.failures && coverage_errors.fetch_add(1,std::memory_order_relaxed)<8))
    {
        char line[300];
        std::snprintf(line,sizeof line,"[MGPU][M339] kind=coverage session=%llu mask=%llu extended=%u failures=%u",
            measurement::session(),coverage.mask,unsigned(coverage.extended),coverage.failures);
        measurement::emit(line);
    }
    auto **table=*reinterpret_cast<void ***>(command);
    std::lock_guard<std::mutex> lock(native_hook_mutex);
    for (const auto &hook:native_hooks) if (hook.table.load(std::memory_order_acquire)==table) return;
    static const reset_fn resets[]={native_reset<0>,native_reset<1>,native_reset<2>,native_reset<3>,native_reset<4>,native_reset<5>,native_reset<6>,native_reset<7>};
    static const close_fn closes[]={native_close<0>,native_close<1>,native_close<2>,native_close<3>,native_close<4>,native_close<5>,native_close<6>,native_close<7>};
    for (unsigned index=0;index<native_hooks.size();++index)
    {
        auto &hook=native_hooks[index];
        if (hook.table.load(std::memory_order_acquire)!=nullptr) continue;
        HMODULE pinned=nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(resets[index]),&pinned)) return;
        DWORD protection=0;
        if (!VirtualProtect(table+9,2*sizeof(void *),PAGE_READWRITE,&protection)) return;
        hook.original_close.store(reinterpret_cast<close_fn>(table[9]),std::memory_order_release);
        hook.original_reset.store(reinterpret_cast<reset_fn>(table[10]),std::memory_order_release);
        hook.table.store(table,std::memory_order_release);
        InterlockedExchangePointer(table+9,reinterpret_cast<void *>(closes[index]));
        InterlockedExchangePointer(table+10,reinterpret_cast<void *>(resets[index]));
        DWORD ignored=0;
        VirtualProtect(table+9,2*sizeof(void *),protection,&ignored);
        char line[260];
        std::snprintf(line,sizeof line,"[MGPU][M337] kind=native_hook session=%llu table=%p close_slot=9 reset_slot=10",
            measurement::session(),table);
        measurement::emit(line);
        return;
    }
}

void feature_created(UINT64 handle, unsigned feature)
{
    if (handle == 0) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (context.features.size() < 128 || context.features.find(handle) != context.features.end())
        context.features[handle] = feature;
}

void feature_released(UINT64 handle)
{
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    context.features.erase(handle);
}

void bind_device(ID3D12Device *device)
{
    nonblocking::bind_device(device);
    if (device == nullptr) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    const auto luid = device->GetAdapterLuid();
    if (context.device && (luid.LowPart != context.luid.LowPart || luid.HighPart != context.luid.HighPart))
    {
        context.failed = true;
        enabled.store(false, std::memory_order_release);
        return;
    }
    if (!context.device)
    {
        context.device = device;
        context.luid = luid;
        const auto marker = reinterpret_cast<UINT64>(&context);
        device->SetPrivateData(trace_device_guid,sizeof marker,&marker);
    }
}

void reset(ID3D12GraphicsCommandList *command)
{
    if (nonblocking::engaged()) { nonblocking::reset(command); return; }
    if (!tracked.load(std::memory_order_acquire)) return;
    if (native_observed(command)) return;
    destroy(command);
}

void destroy(ID3D12GraphicsCommandList *command)
{
    if (nonblocking::engaged()) { nonblocking::destroy(command); return; }
    if (!tracked.load(std::memory_order_acquire)) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    native_work::select(command,false);
    auto *entry = find_job(context, command);
    if (entry == nullptr) return;
    entry->status.reset();
    entry->reset_seen = true;
    release_slot(context, *entry);
    for (auto *alias : entry->aliases) context.command_aliases.erase(alias);
    entry->aliases.clear();
    command->SetPrivateData(trace_command_guid,0,nullptr);
}

void marker(ID3D12GraphicsCommandList *command, phase category, UINT64 pipeline, unsigned width, unsigned height)
{
    if (nonblocking::engaged()) { nonblocking::marker(command, category, pipeline, width, height); return; }
    const bool finishing=category==phase::ngx_end || category==phase::sl_sr_end;
    if (!active() && (!finishing || !tracked.load(std::memory_order_acquire))) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (!active())
    {
        auto *entry=find_job(context,command);
        if (entry && entry->status.state==lifecycle::recording &&
            ((category==phase::ngx_end && entry->scope==phase::ngx_begin) ||
             (category==phase::sl_sr_end && entry->scope==phase::sl_sr_begin)))
            append(context,*entry,category,pipeline,width,height);
        return;
    }
    if (auto *entry = record(context, command))
    {
        const bool metadata=category==phase::target_change ||
            ((category==phase::graphics || category==phase::compute) && pipeline!=0);
        if (entry->fine_candidate && !metadata)
        {
            entry->fine_mixed=true;
        }
        append(context, *entry, category, pipeline, width, height);
    }
}

void tag(ID3D12GraphicsCommandList *command, unsigned frame, unsigned viewport, unsigned type, unsigned width, unsigned height)
{
    if (nonblocking::engaged()) { nonblocking::tag(command, frame, viewport, type, width, height); return; }
    if (!active()) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (auto *entry = record(context, command))
    {
        if (entry->frame_known && (entry->frame != frame || entry->viewport != viewport))
            entry->frame_known = 2;
        else if (!entry->frame_known) { entry->frame = frame; entry->viewport = viewport; entry->frame_known = 1; }
        append(context, *entry, phase::capture_tag, type, width, height);
    }
}

void callback_marker(ID3D12GraphicsCommandList *command,unclassified_callback kind)
{
    if (nonblocking::engaged()) { nonblocking::callback_marker(command, kind); return; }
    if (!active()) return;
    auto &context=data();
    std::lock_guard<std::mutex> lock(context.mutex);
    auto *entry=record(context,command);
    const auto index=unsigned(kind);
    if (!entry || index>=entry->callback_counts.size()) return;
    const bool first=entry->callback_counts[index]++==0;
    ++entry->callback_total;
    if (entry->fine_candidate) entry->fine_mixed=true;
    if (first) entry->callback_origins[index]=capture_origin(context);
    const auto before=entry->marks;
    append(context,*entry,phase::unclassified,0,0,0,first);
    if (first && entry->marks>before) entry->callback_points[index]=before;
}

void close(ID3D12GraphicsCommandList *command)
{
    if (nonblocking::engaged()) { nonblocking::close(command); return; }
    if (!tracked.load(std::memory_order_acquire)) return;
    if (native_depth==0 && native_observed(command)) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    auto *found = find_job(context, command);
    if (found == nullptr) return;
    auto &entry = *found;
    if (entry.status.state != lifecycle::recording) return;
    native_work::select(command,false);
    if (entry.fine_candidate && !active()) entry.fine_seen_cutoff=true;
    if (entry.fine_open!=UINT_MAX) ++entry.mark_overflow;
    if (entry.marks == 0)
    {
        entry.status.state = lifecycle::cancelled;
        return;
    }
    if (native_depth!=0)
    {
        entry.native_end=true;
        ++context.native_closes;
    }
    if (entry.scope != phase::unclassified) ++entry.mark_overflow;
    const auto index = entry.storage*query_stride + entry.marks;
    command->EndQuery(context.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    entry.points[entry.marks++] = {phase::tail};
    command->ResolveQueryData(context.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        entry.storage*query_stride, entry.marks, context.readback.Get(), UINT64(entry.storage)*query_stride*sizeof(UINT64));
    if (entry.fine_count && entry.fine_open==UINT_MAX)
        command->ResolveQueryData(context.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,
            entry.storage*query_stride+max_marks,entry.fine_count*2,context.readback.Get(),
            (UINT64(entry.storage)*query_stride+max_marks)*sizeof(UINT64));
    entry.status.close();
}

void before_submit(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    if (nonblocking::engaged()) { nonblocking::before_submit(queue, count, lists); return; }
    if (!tracked.load(std::memory_order_acquire)) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    unsigned traced = 0;
    unsigned origin = 0;
    for (UINT index = 0; lists != nullptr && index < count; ++index)
    {
        auto *found = find_job(context, static_cast<ID3D12GraphicsCommandList *>(lists[index]));
        if (found == nullptr) continue;
        auto &entry = *found;
        ++traced;
        if (active() && entry.status.state==lifecycle::closed)
        {
            mgpu::queue_trace::job(queue, static_cast<ID3D12GraphicsCommandList *>(lists[index]), entry.session, entry.id);
            if (origin==0) origin=capture_origin(context);
            entry.submit_origin=origin;
        }
        bool duplicate = false;
        for (UINT earlier = 0; earlier < index; ++earlier) duplicate |= lists[earlier] == lists[index];
        if (duplicate || entry.status.state == lifecycle::retired || entry.status.state == lifecycle::submitted)
        {
            char line[350];
            std::snprintf(line,sizeof line,
                "[MGPU][M337] kind=replay session=%llu job=%llu command=%p queue=%p lifecycle=%u duplicate=%u collected=%u emitted=%u",
                entry.session,entry.id,lists[index],queue,unsigned(entry.status.state),unsigned(duplicate),
                unsigned(entry.collected),unsigned(entry.emitted));
            measurement::emit(line);
            entry.status.replayed = true;
            entry.status.state = lifecycle::invalid;
            ++context.replayed;
            report_status(context, "command_replayed_queries_invalid");
        }
    }
    if (active())
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))))
        {
            const auto luid = device->GetAdapterLuid();
            if (luid.LowPart == context.luid.LowPart && luid.HighPart == context.luid.HighPart)
            {
                const auto slot = queue_index(context,queue);
                if (slot < max_queues)
                {
                    context.aggregates[slot].queue = reinterpret_cast<UINT64>(queue);
                    context.aggregates[slot].type = unsigned(queue->GetDesc().Type);
                    context.aggregates[slot].submitted_lists += count;
                    context.aggregates[slot].traced_lists += traced;
                }
            }
        }
    }
}

void after_submit(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
{
    if (nonblocking::engaged()) { nonblocking::after_submit(queue, count, lists); return; }
    if (!tracked.load(std::memory_order_acquire)) return;
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    unsigned destination = UINT_MAX;
    UINT64 ticket = 0;
    for (UINT index = 0; lists != nullptr && index < count; ++index)
    {
        auto *found = find_job(context, static_cast<ID3D12GraphicsCommandList *>(lists[index]));
        if (found == nullptr) continue;
        auto &entry = *found;
        if (entry.status.state != lifecycle::closed) continue;
        if (destination == UINT_MAX)
        {
            destination = queue_index(context, queue);
            if (destination == UINT_MAX) { report_status(context, "queue_fence_unavailable"); return; }
            auto &timeline = context.queues[destination];
            ticket = ++timeline.next;
            timeline.clock = timeline.cache.sample(queue, timing::cpu_ticks());
            if (context.continuous_mode)
            {
                char line[500];
                std::snprintf(line, sizeof line,
                    "[MGPU][M360] kind=clock session=%llu queue=%p gpu=%llu cpu=%llu gpu_frequency=%llu cpu_frequency=%llu query_before=%llu query_after=%llu submitted_qpc=%llu",
                    context.session, queue, timeline.clock.gpu, timeline.clock.cpu, timeline.clock.gpu_frequency,
                    timeline.clock.cpu_frequency, timeline.clock.query_before, timeline.clock.query_after, timing::cpu_ticks());
                measurement::emit(line);
            }
            if (FAILED(queue->Signal(timeline.fence.Get(), ticket)))
            {
                context.failed = true;
                enabled.store(false, std::memory_order_release);
                report_status(context, "queue_signal_failed");
                return;
            }
        }
        entry.queue = destination;
        entry.fence_value = ticket;
        entry.clock = context.queues[destination].clock;
        entry.status.submit();
    }
}

void poll(UINT64 session, bool continuous)
{
    if (continuous || nonblocking::engaged()) { nonblocking::poll(session); return; }
    auto &context = data();
    std::lock_guard<std::mutex> lock(context.mutex);
    if (session != 0 && context.session != 0 && context.session != session)
    {
        if (context.refused_session != session)
        {
            context.refused_session = session;
            char line[250];
            std::snprintf(line,sizeof line,
                "[MGPU][M337] kind=status session=%llu reason=process_restart_required previous_session=%llu",
                session,context.session);
            measurement::emit(line);
        }
        session = 0;
    }
    if (session != 0 && context.session != session)
    {
        context.session = session;
        context.continuous_mode = continuous;
        context.started = timing::cpu_ticks();
        context.stopped_reported = context.drained_reported = false;
        context.aggregates_reported = false;
        report_status(context, "session_started_native_boundary_probe");
    }
    const auto elapsed = timing::cpu_ticks()-context.started;
    if (session != 0 && context.device && !context.failed) initialize(context);
    enabled.store(session != 0 && !context.failed && context.device &&
        (context.continuous_mode || (elapsed >= 2*timing::cpu_frequency() && elapsed < timing::cpu_frequency()*11/4)) &&
        context.mapped != nullptr, std::memory_order_release);
    native_work::set_running(active());
    std::array<UINT64,max_queues> completed{};
    for (unsigned index = 0; index < max_queues; ++index)
        if (context.queues[index].fence) completed[index] = context.queues[index].fence->GetCompletedValue();
    for (unsigned index = 0; index < context.cursor; ++index)
    {
        auto &entry = *context.jobs[index];
        if (entry.collected || entry.queue >= max_queues || completed[entry.queue] == UINT64_MAX ||
            !entry.status.retire(completed[entry.queue] >= entry.fence_value)) continue;
        const auto *values = context.mapped + UINT64(index)*query_stride;
        for (unsigned point = 0; point < entry.marks; ++point) entry.values[point] = values[point];
        for (unsigned point=0;point<entry.fine_count*2;++point) entry.values[max_marks+point]=values[max_marks+point];
        const auto &clock = entry.clock;
        auto &summary = context.aggregates[entry.queue];
        summary.queue = reinterpret_cast<UINT64>(context.queues[entry.queue].queue.Get());
        summary.type = unsigned(context.queues[entry.queue].queue->GetDesc().Type);
        ++summary.jobs;
        summary.frame_known += entry.frame_known == 1;
        summary.frame_conflict += entry.frame_known == 2;
        summary.overflow_jobs += entry.mark_overflow != 0;
        for (unsigned point = 0; point+1 < entry.marks; ++point)
        {
            const auto span = timing::decode(values[point],values[point+1],clock);
            const auto &mark = entry.points[point];
            if (mark.category <= phase::tail && span.gpu_ms >= 0)
            {
                ++summary.segments[unsigned(mark.category)];
                summary.gpu_ms[unsigned(mark.category)] += span.gpu_ms;
            }
            else ++context.invalid_timestamps;
        }
        for (unsigned operation=0;operation<entry.fine_count;++operation)
        {
            const auto &item=entry.fine[operation];
            if (item.end!=item.begin+1 || item.begin<max_marks || item.end>=query_stride ||
                values[item.begin]<values[0] || values[item.end]<values[item.begin] || values[item.end]>values[entry.marks-1])
                ++context.invalid_timestamps;
        }
        entry.collected = true;
        const auto whole=timing::decode(values[0],values[entry.marks-1],clock);
        bool opaque=entry.marks>=2 && !entry.mark_overflow;
        for (unsigned point=0;point+1<entry.marks;++point) opaque&=entry.points[point].category==phase::unclassified;
#ifdef MGPU_FINE_TEST_LEARN
        const double learn_ms=opaque && whole.gpu_ms>=0 ? 2.0 : whole.gpu_ms;
#else
        const double learn_ms=whole.gpu_ms;
#endif
        if (active() && opaque && context.detail_policy.learn(entry.reset_origin,entry.type,entry.marks,
            entry.native_start && entry.native_end,entry.observed_scope,learn_ms,opaque))
        {
            char line[300];
            std::snprintf(line,sizeof line,"[MGPU][M339] kind=learn session=%llu job=%llu reset_origin=%u bracket_ms=%.6f",
                entry.session,entry.id,entry.reset_origin,whole.gpu_ms);
            measurement::emit(line);
        }
    }
    unsigned drained = 0;
    for (unsigned index=0;index<context.labels.size() && drained<8;++index)
    {
        auto &label=context.labels[index];
        if (label.emitted) continue;
        char encoded[193]{};
        for (unsigned byte=0;byte<label.bytes.size();++byte)
            std::snprintf(encoded+byte*2,sizeof encoded-byte*2,"%02x",unsigned(label.bytes[byte]));
        char line[420];
        std::snprintf(line,sizeof line,"[MGPU][M339] kind=label session=%llu label=%u object=%llx encoding=%u bytes=%u data=%s",
            context.session,index+1,label.object,label.encoding,unsigned(label.bytes.size()),encoded);
        measurement::emit(line);
        label.emitted=true;
        ++drained;
    }
    const unsigned drain_limit = context.continuous_mode ? 256u : 8u;
    for (unsigned index = 0; index < context.cursor && drained < drain_limit; ++index)
    {
        auto &entry = *context.jobs[index];
        if (!entry.collected || entry.emitted) continue;
        if (!entry.encoding_prepared)
        {
        char line[trace_log_parts::record_bytes];
        int written = std::snprintf(line,sizeof line,
            "[MGPU][M337] kind=job session=%llu job=%llu queue=%p type=%u engine_frame=%u frame_known=%u viewport=%u "
            "overflow=%u start_ms=%.6f frequency=%llu marks=%u ngx_handle=%llu ngx_feature=%u ngx_known=%u reset_origin=%u submit_origin=%u native_start=%u native_end=%u command=%p fine_selected=%u fine_candidate=%u callback_record=%u data=",
            entry.session,entry.id,context.queues[entry.queue].queue.Get(),entry.type,entry.frame,entry.frame_known,
            entry.viewport,entry.mark_overflow,entry.clock.cpu_ms(entry.values[0]),entry.clock.gpu_frequency,entry.marks,
            entry.ngx_handle,entry.ngx_feature,entry.ngx_known,entry.reset_origin,entry.submit_origin,
            unsigned(entry.native_start),unsigned(entry.native_end),entry.command,unsigned(entry.fine_selected),unsigned(entry.fine_candidate),unsigned(entry.callback_total!=0));
        bool encoded = written > 0 && unsigned(written) < sizeof line;
        for (unsigned point = 0; encoded && point < entry.marks; ++point)
        {
            if (entry.values[point] < entry.values[0]) { encoded = false; break; }
            const auto &mark = entry.points[point];
            const int size = std::snprintf(line+written,sizeof line-unsigned(written),"%s%u:%u:%u:%llx",
                point ? "," : "",unsigned(mark.category),mark.width,mark.height,entry.values[point]-entry.values[0]);
            if (size < 0 || unsigned(size) >= sizeof line-unsigned(written)) encoded = false;
            else written += size;
        }
        char fine_line[trace_log_parts::record_bytes]{};
        int fine_written=0;
        if (entry.fine_selected)
        {
            fine_written=std::snprintf(fine_line,sizeof fine_line,
                "[MGPU][M339] kind=fine session=%llu job=%llu count=%u truncated=%u mixed=%u timing=queries mixed_continues=1 data=",
                entry.session,entry.id,entry.fine_count,unsigned(entry.fine_truncated),unsigned(entry.fine_mixed));
            for (unsigned operation=0;encoded && operation<entry.fine_count;++operation)
            {
                const auto &item=entry.fine[operation];
                if (item.begin>=query_stride || item.end>=query_stride) { encoded=false; break; }
                const int size=std::snprintf(fine_line+fine_written,sizeof fine_line-unsigned(fine_written),
                    "%s%llx:%llx:%u:%u:%llx:%llx:%llx:%llx:%llx:%u:%u",operation ? "," : "",
                    entry.values[item.begin]-entry.values[0],entry.values[item.end]-entry.values[0],item.kind,item.origin,item.pipeline,item.object,item.arg0,item.arg1,item.arg2,item.pipeline_label,item.object_label);
                if (size<0 || unsigned(size)>=sizeof fine_line-unsigned(fine_written)) encoded=false;
                else fine_written+=size;
            }
        }
        char seen_line[700]{};
        int seen_written=0;
        if (entry.fine_candidate)
        {
            seen_written=std::snprintf(seen_line,sizeof seen_line,
                "[MGPU][M339] kind=seen session=%llu job=%llu total=%u budget_skipped=%u scope_skipped=%u mixed=%u truncated=%u cutoff=%u mixed_continues=1 counts=",
                entry.session,entry.id,entry.fine_seen_total,entry.fine_budget_skipped,entry.fine_scope_skipped,
                unsigned(entry.fine_mixed),unsigned(entry.fine_truncated),unsigned(entry.fine_seen_cutoff));
            for (unsigned kind=0;encoded && kind<entry.fine_seen.size();++kind)
            {
                const int size=std::snprintf(seen_line+seen_written,sizeof seen_line-unsigned(seen_written),
                    "%s%u",kind ? "," : "",entry.fine_seen[kind]);
                if (size<0 || unsigned(size)>=sizeof seen_line-unsigned(seen_written)) encoded=false;
                else seen_written+=size;
            }
        }
        if (encoded) encoded=entry.output.append(line,unsigned(written),"[MGPU][M337] kind=job_part",entry.session,entry.id);
        if (encoded && fine_written) encoded=entry.output.append(fine_line,unsigned(fine_written),"[MGPU][M339] kind=fine_part",entry.session,entry.id);
        if (encoded && seen_written) encoded=entry.output.append(seen_line,unsigned(seen_written),"",entry.session,entry.id);
        if (encoded && entry.callback_total)
        {
            char callback_line[700];
            const int callback_written=std::snprintf(callback_line,sizeof callback_line,
                "[MGPU][M339] kind=callbacks session=%llu job=%llu total=%u counts=%u,%u,%u,%u origins=%u,%u,%u,%u points=%u,%u,%u,%u",
                entry.session,entry.id,entry.callback_total,entry.callback_counts[0],entry.callback_counts[1],entry.callback_counts[2],entry.callback_counts[3],
                entry.callback_origins[0],entry.callback_origins[1],entry.callback_origins[2],entry.callback_origins[3],
                entry.callback_points[0],entry.callback_points[1],entry.callback_points[2],entry.callback_points[3]);
            encoded=callback_written>0 && unsigned(callback_written)<sizeof callback_line &&
                entry.output.append(callback_line,unsigned(callback_written),"",entry.session,entry.id);
        }
        if (!encoded) { entry.output.clear(); ++context.encoding_errors; }
        entry.encoding_prepared=true;
        }
        drained+=entry.output.drain(drain_limit-drained,[](const char *line) { measurement::emit(line); });
        entry.emitted=entry.output.empty();
        release_slot(context, entry);
    }
    if (context.session != 0 && !active() && (context.continuous_mode || elapsed >= timing::cpu_frequency()*11/4))
    {
        if (!context.stopped_reported)
        {
            report_status(context, "recording_stopped");
            context.stopped_reported = true;
        }
        bool pending = false;
        for (const auto &label:context.labels) pending|=!label.emitted;
        for (unsigned index = 0; index < context.cursor; ++index)
            pending |= context.jobs[index]->session == context.session &&
                (context.jobs[index]->status.state == lifecycle::submitted ||
                 (context.jobs[index]->observed_scope && (context.jobs[index]->status.state==lifecycle::recording || context.jobs[index]->status.state==lifecycle::closed)) ||
                 (context.jobs[index]->collected && !context.jobs[index]->emitted));
        if (!pending && !context.drained_reported)
        {
            report_status(context, "submitted_jobs_drained");
            context.drained_reported = true;
        }
        if (!pending && !context.aggregates_reported)
        {
            for (const auto &summary : context.aggregates)
            {
                if (summary.queue == 0) continue;
                char line[1200];
                std::snprintf(line, sizeof line,
                    "[MGPU][M337] kind=aggregate session=%llu queue=%p type=%u jobs=%u frame_known=%u frame_conflict=%u overflow_jobs=%u lists=%u traced=%u "
                    "segments=(%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u) "
                    "gpu_ms=(%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f)",
                    context.session, reinterpret_cast<void *>(summary.queue), summary.type, summary.jobs,
                    summary.frame_known, summary.frame_conflict, summary.overflow_jobs,summary.submitted_lists,summary.traced_lists,
                    summary.segments[0],summary.segments[1],summary.segments[2],summary.segments[3],summary.segments[4],
                    summary.segments[5],summary.segments[6],summary.segments[7],summary.segments[8],summary.segments[9],
                    summary.segments[10],summary.segments[11],summary.segments[12],
                    summary.gpu_ms[0],summary.gpu_ms[1],summary.gpu_ms[2],summary.gpu_ms[3],summary.gpu_ms[4],
                    summary.gpu_ms[5],summary.gpu_ms[6],summary.gpu_ms[7],summary.gpu_ms[8],summary.gpu_ms[9],
                    summary.gpu_ms[10],summary.gpu_ms[11],summary.gpu_ms[12]);
                measurement::emit(line);
            }
            context.aggregates_reported = true;
        }
    }
}
}
