#pragma once

#include "gpu_timestamp_span.hpp"
#include <mutex>

namespace mgpu::measurement
{
void poll();
UINT64 session();
bool continuous();
void emit(const char *record);
void queue(UINT64 session_id, const char *role, ID3D12CommandQueue *value);

class cpu_scope
{
    UINT64 session_ = 0, started_ = 0;
    const char *phase_;
    unsigned frame_ = 0, component_ = 0;
public:
    cpu_scope(const char *phase, unsigned frame = 0, unsigned component = 0);
    ~cpu_scope();
};

class scoped_lock
{
    std::unique_lock<std::mutex> lock_;
    UINT64 session_ = 0, started_ = 0, acquired_ = 0;
    const char *phase_;
    unsigned frame_ = 0;
public:
    scoped_lock(std::mutex &mutex, const char *phase, unsigned frame = 0);
    ~scoped_lock();
};
}
