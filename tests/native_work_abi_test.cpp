#define CINTERFACE
#include <d3d12.h>
#include <cstddef>
#include <cstdio>
#include <type_traits>

#define CHECK_SLOT(Method,Slot) static_assert(offsetof(ID3D12GraphicsCommandList4Vtbl,Method)/sizeof(void *)==Slot,#Method)
CHECK_SLOT(DrawInstanced,12);
CHECK_SLOT(DrawIndexedInstanced,13);
CHECK_SLOT(Dispatch,14);
CHECK_SLOT(CopyBufferRegion,15);
CHECK_SLOT(CopyTextureRegion,16);
CHECK_SLOT(CopyResource,17);
CHECK_SLOT(SetPipelineState,25);
CHECK_SLOT(ResourceBarrier,26);
CHECK_SLOT(ExecuteBundle,27);
CHECK_SLOT(ExecuteIndirect,59);
CHECK_SLOT(ClearUnorderedAccessViewUint,49);
CHECK_SLOT(ClearUnorderedAccessViewFloat,50);
CHECK_SLOT(InitializeMetaCommand,70);
CHECK_SLOT(ExecuteMetaCommand,71);
CHECK_SLOT(BuildRaytracingAccelerationStructure,72);
CHECK_SLOT(DispatchRays,76);
using meta_signature=void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList4 *,ID3D12MetaCommand *,const void *,SIZE_T);
using rays_signature=void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList4 *,const D3D12_DISPATCH_RAYS_DESC *);
using build_signature=void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList4 *,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *,UINT,const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *);
static_assert(std::is_same_v<decltype(ID3D12GraphicsCommandList4Vtbl::InitializeMetaCommand),meta_signature>);
static_assert(std::is_same_v<decltype(ID3D12GraphicsCommandList4Vtbl::ExecuteMetaCommand),meta_signature>);
static_assert(std::is_same_v<decltype(ID3D12GraphicsCommandList4Vtbl::DispatchRays),rays_signature>);
static_assert(std::is_same_v<decltype(ID3D12GraphicsCommandList4Vtbl::BuildRaytracingAccelerationStructure),build_signature>);
using clear_uint_signature=void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList4 *,D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource *,const UINT *,UINT,const D3D12_RECT *);
using clear_float_signature=void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList4 *,D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource *,const FLOAT *,UINT,const D3D12_RECT *);
static_assert(std::is_same_v<decltype(ID3D12GraphicsCommandList4Vtbl::ClearUnorderedAccessViewUint),clear_uint_signature>);
static_assert(std::is_same_v<decltype(ID3D12GraphicsCommandList4Vtbl::ClearUnorderedAccessViewFloat),clear_float_signature>);
int main() { std::puts("PASS: native work slots match installed SDK C ABI; not hardware MetaCommand execution"); }
