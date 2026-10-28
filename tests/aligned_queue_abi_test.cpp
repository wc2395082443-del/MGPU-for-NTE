#define CINTERFACE
#include <d3d12.h>
#include <cstddef>

static_assert(offsetof(ID3D12CommandQueueVtbl, ExecuteCommandLists) == 10 * sizeof(void *));
static_assert(offsetof(ID3D12CommandQueueVtbl, Signal) == 14 * sizeof(void *));
static_assert(offsetof(ID3D12CommandListVtbl, GetPrivateData) == 3 * sizeof(void *));
int main() { return 0; }
