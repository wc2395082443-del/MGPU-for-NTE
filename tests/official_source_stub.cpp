#include <cstdint>
namespace mgpu::official_bridge
{
unsigned test_sleep_count = 0, test_marker_count = 0;
unsigned test_admit_count = 0;
unsigned test_input_count = 0, test_phase = 0;
std::uint32_t test_admit_frame = 0;
std::uint32_t test_source_frame = 0;
std::uint64_t test_native_begin_ns = 0, test_native_end_ns = 0;
bool selected() { return true; }
void source_submitted(std::uint32_t frame)
{
    ++test_admit_count;
    test_admit_frame = frame;
}
void source_cancel() {}
void source_wait_input(std::uint32_t frame)
{
    ++test_input_count;
    test_admit_frame = frame;
    test_phase = 1;
}
void source_completed(std::uint32_t) {}
std::uint64_t source_clock_ns() { static std::uint64_t ticks = 0; return ++ticks; }
void source_sleep(std::uint32_t frame, std::uint64_t begin, std::uint64_t end)
{
    test_phase = test_phase == 2 ? 3 : 99;
    ++test_sleep_count;
    test_source_frame = frame;
    test_native_begin_ns = begin;
    test_native_end_ns = end;
}
void source_marker(std::uint32_t frame, unsigned, std::uint64_t begin, std::uint64_t end)
{
    ++test_marker_count;
    test_source_frame = frame;
    test_native_begin_ns = begin;
    test_native_end_ns = end;
}
}
