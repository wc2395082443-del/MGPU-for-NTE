#include <cstdint>
namespace mgpu::official_bridge
{
unsigned test_sleep_count = 0, test_marker_count = 0;
std::uint32_t test_source_frame = 0;
std::uint64_t test_native_begin_ns = 0, test_native_end_ns = 0;
bool selected() { return true; }
std::uint64_t source_clock_ns() { static std::uint64_t ticks = 0; return ++ticks; }
void source_sleep(std::uint32_t frame, std::uint64_t begin, std::uint64_t end)
{
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
