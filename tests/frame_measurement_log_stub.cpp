#include "../src/log_queue.hpp"

namespace mgpu::lq
{
bool running() { return false; }
void post(dest, const char *) {}
}
