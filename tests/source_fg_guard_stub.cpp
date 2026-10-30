#include "../src/source_fg_guard.hpp"

namespace mgpu::source_fg_guard
{
bool test_ready = false;
void prepare() {}
bool ready() { return test_ready; }
}
