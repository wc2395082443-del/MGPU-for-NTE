#pragma once
#include <d3d12.h>

namespace mgpu::native_work
{
enum class operation : unsigned { draw=1,draw_indexed,dispatch,copy_buffer,copy_texture,
    copy_resource,barrier,indirect,bundle,meta_initialize,meta_execute,acceleration_build,rays,uav_clear_uint,uav_clear_float };
constexpr unsigned operation_count=15;
using before_fn=unsigned (*)(ID3D12GraphicsCommandList *,operation,ID3D12Object *,UINT64,UINT64,UINT64);
using after_fn=void (*)(ID3D12GraphicsCommandList *,unsigned);
using pipeline_fn=void (*)(ID3D12GraphicsCommandList *,ID3D12PipelineState *);
struct coverage { UINT64 mask=0; bool extended=false,new_table=false; unsigned failures=0; };
coverage install(ID3D12GraphicsCommandList *command,before_fn before,after_fn after,pipeline_fn pipeline);
bool select(ID3D12GraphicsCommandList *command,bool selected);
void set_running(bool running);
void set_continuous(bool enabled);
}
