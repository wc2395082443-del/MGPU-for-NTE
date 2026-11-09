#pragma once

#include <cstdint>

namespace mgpu::present_backpressure
{
struct decision { bool wait_for_nr = false; unsigned timeout_ms = 0; };
inline decision choose(bool async_neural, bool neural_pending, unsigned inflight,
    std::uint64_t produced, std::uint64_t consumed, unsigned base_timeout_ms = 4,
    unsigned bounded_timeout_ms = 25)
{
    const bool backlog = async_neural && neural_pending && inflight > 0 && produced > consumed + 1;
    return {backlog, backlog ? bounded_timeout_ms : base_timeout_ms};
}
}
