#include "../src/frame_measurement.hpp"
#include "../src/log_queue.hpp"
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace { bool logger_running = true; std::vector<std::string> lines; }
namespace mgpu::lq
{
bool running() { return logger_running; }
void post(dest, const char *line) { lines.emplace_back(line); }
}

int main()
{
    using namespace mgpu;
    if (measurement::session() != 0) return 1;
    measurement::poll();
    wchar_t name[100];
    std::swprintf(name, sizeof name / sizeof *name, L"Local\\MGPUFrameTiming.%lu", GetCurrentProcessId());
    HANDLE control = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (control == nullptr) return 2;
    SetEvent(control);
    measurement::poll();
    const auto session = measurement::session();
    if (session == 0) return 3;
    std::mutex mutex;
    { measurement::cpu_scope scope("unit", 5, 2); measurement::scoped_lock lock(mutex, "unit_lock", 5); Sleep(1); }
    ResetEvent(control);
    measurement::poll();
    if (measurement::session() != 0 || lines.size() != 5) return 4;
    if (lines[0].find("kind=start") == std::string::npos || lines[1].find("kind=lock") == std::string::npos ||
        lines[2].find("kind=cpu") == std::string::npos || lines[3].find("kind=stop") == std::string::npos ||
        lines[4].find("[M343] queue_trace_export=") == std::string::npos) return 5;
    CloseHandle(control);
    std::puts("PASS: opt-in M336 measurement session and CPU scopes");
    return 0;
}
