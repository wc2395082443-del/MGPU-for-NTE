#include "official_channel.hpp"
#include "startup_source_policy.hpp"
#include <cstdlib>

using namespace mgpu::official_fg;

int main()
{
    source_history history;
    for (unsigned frame = 1130; frame <= 1469; ++frame)
        for (unsigned kind : {sleep_marker, 0u, 1u, 2u, 3u})
            if (!history.note(frame, kind)) return EXIT_FAILURE;
    history.retain({1157, 0, 0});
    if (history.simulation_ready(1157) || history.bits(1157) != 0 || history.cached_frame(1157) != 1413)
        return EXIT_FAILURE;
    if (!startup_source_expired(1157, history.cached_frame(1157), marker_history, 0, 0, 6000)) return EXIT_FAILURE;
    if (startup_source_expired(1157, 1157, marker_history, 0, 0, 6000)) return EXIT_FAILURE;
    if (startup_source_expired(1157, 1412, marker_history, 0, 0, 6000)) return EXIT_FAILURE;
    if (startup_source_expired(1157, 1093, marker_history, 0, 0, 6000)) return EXIT_FAILURE;
    if (startup_source_expired(1157, 1413, marker_history, 1, 0, 6000)) return EXIT_FAILURE;
    if (startup_source_expired(1157, 1413, marker_history, 0, 8, 6000)) return EXIT_FAILURE;
    if (startup_source_expired(1157, 1413, marker_history, 0, 0, 20000)) return EXIT_FAILURE;
    if (startup_source_expired(0, 64, marker_history, 0, 0, 6000)) return EXIT_FAILURE;
    history.retain({1469, 0, 0});
    if (!history.simulation_ready(1469)) return EXIT_FAILURE;
    for (unsigned frame = 1470; frame <= 1600; ++frame)
        for (unsigned kind : {sleep_marker, 0u, 1u, 2u, 3u}) history.note(frame, kind);
    if (!history.simulation_ready(1469) || history.simulation_ready(1157)) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
