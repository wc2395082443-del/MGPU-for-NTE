#include "native_work_observer.hpp"
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <mutex>
#include <wrl/client.h>

namespace mgpu::native_work
{
namespace
{
struct hook_record
{
    void **table=nullptr;
    std::atomic<void **> published{nullptr};
    std::array<std::atomic<void *>,80> originals{};
    coverage result{};
};
std::array<hook_record,8> hooks{};
std::mutex hook_mutex;
std::array<std::atomic<ID3D12GraphicsCommandList *>,16> selected_commands{};
std::atomic<UINT64> selection_epoch{1};
std::atomic<bool> running{false};
std::atomic<bool> continuous{false};
constexpr unsigned continuous_capacity = 65536;
std::array<std::atomic<std::uintptr_t>, continuous_capacity> continuous_commands{};
std::atomic<before_fn> before_callback{nullptr};
std::atomic<after_fn> after_callback{nullptr};
std::atomic<pipeline_fn> pipeline_callback{nullptr};
thread_local unsigned nesting=0;

bool selected(ID3D12GraphicsCommandList *command)
{
    if (!running.load(std::memory_order_acquire)) return false;
    if (continuous.load(std::memory_order_acquire))
    {
        const auto identity = reinterpret_cast<std::uintptr_t>(command);
        const auto start = unsigned((identity >> 4) * 11400714819323198485ull) & (continuous_capacity - 1);
        for (unsigned index = 0; index < 64; ++index)
        {
            const auto value = continuous_commands[(start + index) & (continuous_capacity - 1)].load(std::memory_order_acquire);
            if (value == identity) return true;
            if (!value) return false;
        }
        return false;
    }
    struct cache { ID3D12GraphicsCommandList *command=nullptr; UINT64 epoch=0; bool found=false; };
    static thread_local cache previous;
    const auto epoch=selection_epoch.load(std::memory_order_acquire);
    if (previous.command==command && previous.epoch==epoch) return previous.found;
    bool found=false;
    for (const auto &entry:selected_commands) found|=entry.load(std::memory_order_acquire)==command;
    previous={command,epoch,found};
    return found;
}

struct observation
{
    ID3D12GraphicsCommandList *command;
    unsigned token=UINT_MAX;
    observation(ID3D12GraphicsCommandList *value,operation kind,ID3D12Object *object,UINT64 arg0,UINT64 arg1,UINT64 arg2)
        :command(value)
    {
        const bool outer=nesting++==0;
        if (outer && selected(command))
            if (auto before=before_callback.load(std::memory_order_acquire)) token=before(command,kind,object,arg0,arg1,arg2);
    }
    ~observation()
    {
        if (token!=UINT_MAX)
            if (auto after=after_callback.load(std::memory_order_acquire)) after(command,token);
        --nesting;
    }
};

template<unsigned Index,typename Function>
Function original(unsigned slot) { return reinterpret_cast<Function>(hooks[Index].originals[slot].load(std::memory_order_acquire)); }

#define MGPU_NATIVE_VOID(Name,Slot,Parameters,Arguments,Kind,Object,Arg0,Arg1,Arg2) \
template<unsigned Index> void STDMETHODCALLTYPE Name Parameters \
{ \
    observation observed(command,operation::Kind,Object,Arg0,Arg1,Arg2); \
    using function=void(STDMETHODCALLTYPE *) Parameters; \
    original<Index,function>(Slot) Arguments; \
}

MGPU_NATIVE_VOID(draw,12,(ID3D12GraphicsCommandList *command,UINT vertex_count,UINT instance_count,UINT vertex_start,UINT instance_start),
    (command,vertex_count,instance_count,vertex_start,instance_start),draw,nullptr,vertex_count,instance_count,vertex_start)
MGPU_NATIVE_VOID(draw_indexed,13,(ID3D12GraphicsCommandList *command,UINT index_count,UINT instance_count,UINT index_start,INT vertex_base,UINT instance_start),
    (command,index_count,instance_count,index_start,vertex_base,instance_start),draw_indexed,nullptr,index_count,instance_count,index_start)
MGPU_NATIVE_VOID(dispatch,14,(ID3D12GraphicsCommandList *command,UINT groups_x,UINT groups_y,UINT groups_z),
    (command,groups_x,groups_y,groups_z),dispatch,nullptr,groups_x,groups_y,groups_z)
MGPU_NATIVE_VOID(copy_buffer,15,(ID3D12GraphicsCommandList *command,ID3D12Resource *destination,UINT64 destination_offset,ID3D12Resource *source,UINT64 source_offset,UINT64 bytes),
    (command,destination,destination_offset,source,source_offset,bytes),copy_buffer,destination,bytes,source_offset,destination_offset)
MGPU_NATIVE_VOID(copy_texture,16,(ID3D12GraphicsCommandList *command,const D3D12_TEXTURE_COPY_LOCATION *destination,UINT destination_x,UINT destination_y,UINT destination_z,const D3D12_TEXTURE_COPY_LOCATION *source,const D3D12_BOX *box),
    (command,destination,destination_x,destination_y,destination_z,source,box),copy_texture,destination ? destination->pResource : nullptr,box ? box->right-box->left : 0,box ? box->bottom-box->top : 0,box ? box->back-box->front : 0)
MGPU_NATIVE_VOID(copy_resource,17,(ID3D12GraphicsCommandList *command,ID3D12Resource *destination,ID3D12Resource *source),
    (command,destination,source),copy_resource,destination,reinterpret_cast<UINT64>(source),0,0)
MGPU_NATIVE_VOID(clear_uint,49,(ID3D12GraphicsCommandList *command,D3D12_GPU_DESCRIPTOR_HANDLE gpu,D3D12_CPU_DESCRIPTOR_HANDLE cpu,ID3D12Resource *resource,const UINT *values,UINT count,const D3D12_RECT *rectangles),
    (command,gpu,cpu,resource,values,count,rectangles),uav_clear_uint,resource,count,cpu.ptr,gpu.ptr)
MGPU_NATIVE_VOID(clear_float,50,(ID3D12GraphicsCommandList *command,D3D12_GPU_DESCRIPTOR_HANDLE gpu,D3D12_CPU_DESCRIPTOR_HANDLE cpu,ID3D12Resource *resource,const FLOAT *values,UINT count,const D3D12_RECT *rectangles),
    (command,gpu,cpu,resource,values,count,rectangles),uav_clear_float,resource,count,cpu.ptr,gpu.ptr)
MGPU_NATIVE_VOID(barrier,26,(ID3D12GraphicsCommandList *command,UINT count,const D3D12_RESOURCE_BARRIER *barriers),
    (command,count,barriers),barrier,nullptr,count,count && barriers ? UINT64(barriers[0].Type)|(UINT64(barriers[0].Flags)<<32) : 0,
    count && barriers && barriers[0].Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? UINT64(barriers[0].Transition.StateBefore)|(UINT64(barriers[0].Transition.StateAfter)<<32) : 0)
MGPU_NATIVE_VOID(bundle,27,(ID3D12GraphicsCommandList *command,ID3D12GraphicsCommandList *secondary),
    (command,secondary),bundle,secondary,0,0,0)
MGPU_NATIVE_VOID(indirect,59,(ID3D12GraphicsCommandList *command,ID3D12CommandSignature *signature,UINT maximum_count,ID3D12Resource *arguments,UINT64 argument_offset,ID3D12Resource *count_buffer,UINT64 count_offset),
    (command,signature,maximum_count,arguments,argument_offset,count_buffer,count_offset),indirect,signature,maximum_count,argument_offset,count_offset)
MGPU_NATIVE_VOID(meta_initialize,70,(ID3D12GraphicsCommandList4 *command,ID3D12MetaCommand *meta,const void *parameters,SIZE_T bytes),
    (command,meta,parameters,bytes),meta_initialize,meta,bytes,0,0)
MGPU_NATIVE_VOID(meta_execute,71,(ID3D12GraphicsCommandList4 *command,ID3D12MetaCommand *meta,const void *parameters,SIZE_T bytes),
    (command,meta,parameters,bytes),meta_execute,meta,bytes,0,0)
MGPU_NATIVE_VOID(acceleration_build,72,(ID3D12GraphicsCommandList4 *command,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *description,UINT count,const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild),
    (command,description,count,postbuild),acceleration_build,nullptr,description ? description->Inputs.NumDescs : 0,count,description ? description->Inputs.Flags : 0)
MGPU_NATIVE_VOID(rays,76,(ID3D12GraphicsCommandList4 *command,const D3D12_DISPATCH_RAYS_DESC *description),
    (command,description),rays,nullptr,description ? description->Width : 0,description ? description->Height : 0,description ? description->Depth : 0)
#undef MGPU_NATIVE_VOID

template<unsigned Index>
void STDMETHODCALLTYPE pipeline(ID3D12GraphicsCommandList *command,ID3D12PipelineState *state)
{
    const bool outer=nesting++==0;
    if (outer && selected(command))
        if (auto callback=pipeline_callback.load(std::memory_order_acquire)) callback(command,state);
    using function=void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *,ID3D12PipelineState *);
    original<Index,function>(25)(command,state);
    --nesting;
}

bool patch(hook_record &hook,unsigned slot,void *replacement)
{
    DWORD protection=0;
    if (!VirtualProtect(hook.table+slot,sizeof(void *),PAGE_READWRITE,&protection)) { ++hook.result.failures; return false; }
    hook.originals[slot].store(hook.table[slot],std::memory_order_release);
    InterlockedExchangePointer(hook.table+slot,replacement);
    DWORD ignored=0;
    if (!VirtualProtect(hook.table+slot,sizeof(void *),protection,&ignored)) ++hook.result.failures;
    return true;
}
}

void set_running(bool value) { running.store(value,std::memory_order_release); }
void set_continuous(bool value) { continuous.store(value, std::memory_order_release); }

bool select(ID3D12GraphicsCommandList *command,bool value)
{
    if (!command) return false;
    if (continuous.load(std::memory_order_acquire))
    {
        const auto identity = reinterpret_cast<std::uintptr_t>(command);
        const auto start = unsigned((identity >> 4) * 11400714819323198485ull) & (continuous_capacity - 1);
        unsigned reusable = UINT_MAX;
        for (unsigned index = 0; index < 64; ++index)
        {
            auto &slot = continuous_commands[(start + index) & (continuous_capacity - 1)];
            auto current = slot.load(std::memory_order_acquire);
            if (current == identity)
            {
                if (!value) slot.compare_exchange_strong(current, UINTPTR_MAX, std::memory_order_acq_rel);
                return true;
            }
            if (!value && !current) return true;
            if (value && current == UINTPTR_MAX && reusable == UINT_MAX) reusable = (start + index) & (continuous_capacity - 1);
            if (value && !current)
            {
                auto &target = reusable == UINT_MAX ? slot : continuous_commands[reusable];
                auto expected = reusable == UINT_MAX ? std::uintptr_t(0) : UINTPTR_MAX;
                return target.compare_exchange_strong(expected, identity, std::memory_order_acq_rel);
            }
        }
        if (value && reusable != UINT_MAX)
        {
            auto expected = UINTPTR_MAX;
            return continuous_commands[reusable].compare_exchange_strong(expected, identity, std::memory_order_acq_rel);
        }
        return !value;
    }
    for (auto &entry:selected_commands)
    {
        if (entry.load(std::memory_order_acquire)!=command) continue;
        if (!value) { entry.store(nullptr,std::memory_order_release); selection_epoch.fetch_add(1,std::memory_order_release); }
        return true;
    }
    if (!value) return true;
    for (auto &entry:selected_commands)
    {
        ID3D12GraphicsCommandList *empty=nullptr;
        if (entry.compare_exchange_strong(empty,command,std::memory_order_acq_rel))
        {
            selection_epoch.fetch_add(1,std::memory_order_release);
            return true;
        }
    }
    return false;
}

coverage install(ID3D12GraphicsCommandList *command,before_fn before,after_fn after,pipeline_fn on_pipeline)
{
    if (!command) return {};
    auto **table=*reinterpret_cast<void ***>(command);
    for (const auto &hook : hooks)
        if (hook.published.load(std::memory_order_acquire) == table) return hook.result;
    std::lock_guard<std::mutex> lock(hook_mutex);
    for (const auto &hook:hooks) if (hook.table==table) return hook.result;
    before_callback.store(before,std::memory_order_release);
    after_callback.store(after,std::memory_order_release);
    pipeline_callback.store(on_pipeline,std::memory_order_release);
    for (unsigned index=0;index<hooks.size();++index)
    {
        auto &hook=hooks[index];
        if (hook.table) continue;
        HMODULE pinned=nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&install),&pinned)) return {0,false,false,1};
        hook.table=table;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> extended;
        hook.result.extended=SUCCEEDED(command->QueryInterface(IID_PPV_ARGS(&extended))) &&
            static_cast<ID3D12GraphicsCommandList *>(extended.Get())==command &&
            *reinterpret_cast<void ***>(extended.Get())==table;
#define MGPU_PATCH_NATIVE(Name,Slot,Kind) \
        { static const void *functions[]={reinterpret_cast<void *>(Name<0>),reinterpret_cast<void *>(Name<1>),reinterpret_cast<void *>(Name<2>),reinterpret_cast<void *>(Name<3>),reinterpret_cast<void *>(Name<4>),reinterpret_cast<void *>(Name<5>),reinterpret_cast<void *>(Name<6>),reinterpret_cast<void *>(Name<7>)}; \
          if (patch(hook,Slot,const_cast<void *>(functions[index]))) hook.result.mask|=UINT64(1)<<(unsigned(operation::Kind)-1); }
        MGPU_PATCH_NATIVE(draw,12,draw)
        MGPU_PATCH_NATIVE(draw_indexed,13,draw_indexed)
        MGPU_PATCH_NATIVE(dispatch,14,dispatch)
        MGPU_PATCH_NATIVE(copy_buffer,15,copy_buffer)
        MGPU_PATCH_NATIVE(copy_texture,16,copy_texture)
        MGPU_PATCH_NATIVE(copy_resource,17,copy_resource)
        MGPU_PATCH_NATIVE(clear_uint,49,uav_clear_uint)
        MGPU_PATCH_NATIVE(clear_float,50,uav_clear_float)
        MGPU_PATCH_NATIVE(barrier,26,barrier)
        MGPU_PATCH_NATIVE(bundle,27,bundle)
        MGPU_PATCH_NATIVE(indirect,59,indirect)
        if (hook.result.extended)
        {
            MGPU_PATCH_NATIVE(meta_initialize,70,meta_initialize)
            MGPU_PATCH_NATIVE(meta_execute,71,meta_execute)
            MGPU_PATCH_NATIVE(acceleration_build,72,acceleration_build)
            MGPU_PATCH_NATIVE(rays,76,rays)
        }
#undef MGPU_PATCH_NATIVE
        static const void *pipelines[]={reinterpret_cast<void *>(pipeline<0>),reinterpret_cast<void *>(pipeline<1>),reinterpret_cast<void *>(pipeline<2>),reinterpret_cast<void *>(pipeline<3>),reinterpret_cast<void *>(pipeline<4>),reinterpret_cast<void *>(pipeline<5>),reinterpret_cast<void *>(pipeline<6>),reinterpret_cast<void *>(pipeline<7>)};
        patch(hook,25,const_cast<void *>(pipelines[index]));
        auto result=hook.result;
        hook.published.store(table, std::memory_order_release);
        result.new_table=true;
        return result;
    }
    return {0,false,false,1};
}
}
