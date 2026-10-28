#include "../experimental/official_fg/output_trace.hpp"
#include <cstddef>
#include <cstdio>

int main()
{
    using mgpu::official_fg::output_frame_trace;
    using mgpu::official_fg::camera_packet;
    std::printf("{\"helper_record_bytes\":%zu,\"fields\":{", sizeof(output_frame_trace));
    const auto field = [](const char *name, std::size_t offset) { std::printf("\"%s\":%zu,", name, offset); };
    field("sequence", offsetof(output_frame_trace, sequence));
    field("source", offsetof(output_frame_trace, source));
    field("capture_ns", offsetof(output_frame_trace, capture_ns));
    field("backlog", offsetof(output_frame_trace, backlog));
    field("visible", offsetof(output_frame_trace, visible));
    field("requested", offsetof(output_frame_trace, requested));
    field("ticks", offsetof(output_frame_trace, ticks));
    field("reflex", offsetof(output_frame_trace, reflex));
    field("sdk_reported", offsetof(output_frame_trace, sdk_reported));
    field("sdk_status", offsetof(output_frame_trace, sdk_status));
    field("present_count", offsetof(output_frame_trace, present_count));
    field("present_result", offsetof(output_frame_trace, present_result));
    field("camera_forward", offsetof(output_frame_trace, camera) + offsetof(camera_packet, forward));
    field("camera_position", offsetof(output_frame_trace, camera) + offsetof(camera_packet, position));
    std::printf("\"camera_reset\":%zu}}\n", offsetof(output_frame_trace, camera) + offsetof(camera_packet, reset));
    return 0;
}
