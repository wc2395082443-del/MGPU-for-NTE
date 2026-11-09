#include "frame_measurement.hpp"

namespace mgpu::measurement
{
void poll() {}
UINT64 session() { return 0; }
bool continuous() { return false; }
void emit(const char *) {}
void queue(UINT64, const char *, ID3D12CommandQueue *) {}
cpu_scope::cpu_scope(const char *phase, unsigned frame, unsigned component)
    : phase_(phase), frame_(frame), component_(component) {}
cpu_scope::~cpu_scope() = default;
scoped_lock::scoped_lock(std::mutex &mutex, const char *phase, unsigned frame)
    : lock_(mutex), phase_(phase), frame_(frame) {}
scoped_lock::~scoped_lock() = default;
}
