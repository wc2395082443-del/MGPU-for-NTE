#include "../src/frame_measurement.hpp"
#include "../src/game_work_trace.hpp"
#include "../src/log_queue.hpp"
#include "../src/continuous_trace.hpp"
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

namespace { std::vector<std::string> lines; bool draining = false, incomplete = false; }
namespace mgpu::lq
{
bool running() { return true; }
void post(dest, const char *line) { lines.emplace_back(line); }
}
namespace mgpu::game_work
{
void poll(UINT64 session, bool)
{
    if (!session && draining)
    {
        measurement::emit(incomplete ? "[MGPU][M337] kind=status session=1 reason=trace_incomplete" :
            "[MGPU][M337] kind=status session=1 reason=submitted_jobs_drained");
        draining = false;
    }
}
}

int main(int count, char **arguments)
{
    const bool long_test = count > 1 && std::string(arguments[1]) == "--120-seconds";
    incomplete = count > 1 && std::string(arguments[1]) == "--incomplete";
    mgpu::measurement::poll();
    if (count > 1 && std::string(arguments[1]) == "--legacy-first")
    {
        const auto name = L"Local\\MGPUFrameTiming." + std::to_wstring(GetCurrentProcessId());
        HANDLE bounded = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
        if (!bounded || !SetEvent(bounded)) return 6;
        mgpu::measurement::poll();
        ResetEvent(bounded);
        mgpu::measurement::poll();
        CloseHandle(bounded);
        const auto pipeline_name = L"Local\\MGPUFramePipeline." + std::to_wstring(GetCurrentProcessId());
        HANDLE pipeline = OpenEventW(EVENT_MODIFY_STATE, FALSE, pipeline_name.c_str());
        if (!pipeline || !SetEvent(pipeline)) return 7;
        mgpu::measurement::poll();
        CloseHandle(pipeline);
        if (mgpu::measurement::session() || mgpu::measurement::continuous()) return 8;
        std::puts("PASS legacy-to-continuous transition requires process restart");
        return 0;
    }
    wchar_t name[100];
    std::swprintf(name, 100, L"Local\\MGPUFramePipeline.%lu", GetCurrentProcessId());
    HANDLE control = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!control || !SetEvent(control)) return 1;
    mgpu::measurement::poll();
    const auto session = mgpu::measurement::session();
    if (!session || !mgpu::measurement::continuous()) return 2;
    const auto started = GetTickCount64();
    unsigned emitted = 0;
    do
    {
        for (unsigned index = 0; index < (long_test ? 32u : 256u); ++index)
        {
            char line[200];
            std::snprintf(line, sizeof line, "[MGPU][M360] kind=fixture session=%llu number=%u", session, emitted++);
            mgpu::measurement::emit(line);
        }
        mgpu::measurement::poll();
        if (mgpu::measurement::session() != session) return 3;
        Sleep(long_test ? 10 : 2);
    } while (long_test ? GetTickCount64() - started < 120050 : emitted < (incomplete ? 256u : 102400u));
    draining = true;
    ResetEvent(control);
    CloseHandle(control);
    const auto deadline = GetTickCount64() + 10000;
    bool sealed = false;
    while (GetTickCount64() < deadline)
    {
        mgpu::measurement::poll();
        for (const auto &line : lines) sealed |= line.find("pipeline_export=") != std::string::npos;
        if (sealed) break;
        Sleep(2);
    }
    if (!sealed || mgpu::measurement::session()) return 4;
    const auto path = std::filesystem::temp_directory_path() /
        (L"MGPUPipeline." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(session) + L".bin");
    std::ifstream file(path, std::ios::binary);
    mgpu::trace::stream_header header;
    file.read(reinterpret_cast<char *>(&header), sizeof header);
    if (!file || header.records < emitted || header.dropped != (incomplete ? 1u : 0u) || header.failures ||
        (long_test && double(header.end - header.begin) / header.frequency < 120)) return 5;
    file.close();
    std::filesystem::remove(path);
    std::printf("PASS continuous main session: %u records, %.3f seconds; explicit pipeline trigger and incomplete-trace rejection contract\n",
                emitted, double(header.end - header.begin) / header.frequency);
    return 0;
}
