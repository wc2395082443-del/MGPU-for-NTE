#include "output_trace.hpp"
#include <fstream>
#include <string>
#include <stdexcept>

int main()
{
    using namespace mgpu::official_fg;
    const auto path = std::filesystem::temp_directory_path() /
        (L"mgpu-output-trace-test-" + std::to_wstring(GetCurrentProcessId()) + L".csv");
    try {
        output_frame_trace frame{};
        if (frame.ordered()) throw std::runtime_error("empty trace accepted");
        frame.sequence = 1;
        frame.source = 44;
        frame.visible = 1;
        frame.requested = 6;
        frame.reflex.phases[0] = {1000, 2000, 3000, 4000, 5000};
        frame.helper_render_begin_ns = 6000;
        frame.helper_render_end_ns = 7000;
        frame.source_pings = 8;
        for (unsigned index = 0; index < frame.ticks.size(); ++index) frame.ticks[index] = 100 + index * 10;
        if (!frame.ordered()) throw std::runtime_error("ordered trace rejected");
        {
            output_trace trace(path);
            frame.ticks[4] = 1;
            if (trace.record(frame)) throw std::runtime_error("backwards stage accepted");
            frame.ticks[4] = 140;
            if (trace.record(frame)) throw std::runtime_error("unrequested trace consumed capacity");
            HANDLE trigger = OpenEventW(EVENT_MODIFY_STATE, FALSE, output_trace::trigger_name().c_str());
            if (!trigger) throw std::runtime_error("trace trigger unavailable");
            const bool signaled = SetEvent(trigger) != FALSE;
            CloseHandle(trigger);
            if (!signaled) throw std::runtime_error("cannot trigger trace");
            for (unsigned index = 0; index < output_trace::capacity; ++index) {
                frame.sequence = index + 1;
                if (!trace.record(frame)) throw std::runtime_error("bounded trace lost row");
                if (index == 63) {
                    HANDLE live = INVALID_HANDLE_VALUE;
                    const auto deadline = GetTickCount64() + 1000;
                    while (live == INVALID_HANDLE_VALUE && GetTickCount64() < deadline) {
                        live = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (live == INVALID_HANDLE_VALUE) Sleep(5);
                    }
                    if (live == INVALID_HANDLE_VALUE) throw std::runtime_error("trace cannot be read while recording");
                    CloseHandle(live);
                }
            }
            if (trace.record(frame)) throw std::runtime_error("trace limit ignored");
        }
        std::ifstream file(path);
        std::string line;
        unsigned lines = 0;
        while (std::getline(file, line)) {
            if (!lines && (line.find("sleep_native_begin_ns") == std::string::npos ||
                line.find("helper_render_begin_ns,helper_render_end_ns,source_pings") == std::string::npos))
                throw std::runtime_error("source Reflex trace columns missing");
            if (lines && (line.find(",1000,2000,3000,4000,5000,") == std::string::npos ||
                line.substr(line.size() - 12) != ",6000,7000,8"))
                throw std::runtime_error("source Reflex timestamps lost in async writer");
            if (lines && (line.find(",100,110,120,130,140,150,160,170,180,190,200") == std::string::npos ||
                line.rfind(std::to_string(lines) + ",44,", 0) != 0))
                throw std::runtime_error("async trace mixed frame fields");
            ++lines;
        }
        file.close();
        if (lines != output_trace::capacity + 1) throw std::runtime_error("trace not drained on shutdown");
        std::filesystem::remove(path);
        std::puts("output trace trigger, live sharing, ordering, capacity, concurrent writer and shutdown PASS");
        return 0;
    } catch (const std::exception &error) {
        std::printf("FAIL %s\n", error.what());
        return 1;
    }
}
