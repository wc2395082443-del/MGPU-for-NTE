#include "output_trace.hpp"
#include <fstream>
#include <stdexcept>

int main()
{
    using namespace mgpu::official_fg;
    const auto path = std::filesystem::temp_directory_path() /
        (L"mgpu-pipeline-trace-test-" + std::to_wstring(GetCurrentProcessId()) + L".csv");
    const auto binary = path.wstring() + L".pipeline.bin";
    try
    {
        constexpr unsigned total = 24000;
        {
            output_trace trace(path);
            HANDLE trigger = OpenEventW(EVENT_MODIFY_STATE, FALSE, output_trace::pipeline_trigger_name().c_str());
            if (!trigger || !SetEvent(trigger)) throw std::runtime_error("pipeline trigger");
            output_frame_trace frame{};
            frame.source = 44;
            frame.camera.forward = {1, 0, 0};
            frame.sdk_reported = 6;
            for (unsigned index = 0; index < frame.ticks.size(); ++index) frame.ticks[index] = 100 + index;
            for (unsigned index = 0; index < total; ++index)
            {
                frame.sequence = index + 1;
                frame.present_count = index + 1;
                if (!trace.record(frame)) throw std::runtime_error("continuous helper trace lost a frame");
                if (index % 128 == 0) Sleep(1);
            }
            if (!ResetEvent(trigger)) throw std::runtime_error("pipeline stop");
            CloseHandle(trigger);
            trace.poll();
        }
        std::ifstream input(binary, std::ios::binary);
        mgpu::trace::stream_header header;
        input.read(reinterpret_cast<char *>(&header), sizeof header);
        if (!input || header.records != total || header.dropped || header.failures ||
            header.record_bytes != sizeof(output_frame_trace)) throw std::runtime_error("pipeline seal/count");
        for (unsigned index = 0; index < total; ++index)
        {
            output_frame_trace frame;
            input.read(reinterpret_cast<char *>(&frame), sizeof frame);
            if (!input || frame.sequence != index + 1 || frame.present_count != index + 1 ||
                frame.source != 44 || frame.sdk_reported != 6 || frame.camera.forward[0] != 1)
                throw std::runtime_error("camera/FG/Present identity lost");
        }
        input.close();
        std::filesystem::remove(binary);
        std::filesystem::remove(path);
        std::puts("PASS: 24000 ordered helper frames, camera/SDK/Present IDs, bounded async storage and explicit stop");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::printf("FAIL %s\n", error.what());
        return 1;
    }
}
