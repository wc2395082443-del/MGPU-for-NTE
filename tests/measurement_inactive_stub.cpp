#include "../src/frame_measurement.hpp"
namespace mgpu::measurement
{
UINT64 session() { return 0; }
bool continuous() { return false; }
void emit(const char *) {}
}
