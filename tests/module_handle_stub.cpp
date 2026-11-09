#include <windows.h>

namespace mgpu
{
HMODULE module_handle() { return GetModuleHandleW(nullptr); }
}
