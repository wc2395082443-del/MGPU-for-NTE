#pragma once

#include <cstdint>
#include "aligned_frame_policy.hpp"
#include "motion_frame_packet.hpp"

namespace mgpu::calibration
{
inline bool authoritative_guides_available(bool aligned_mode, bool depth_valid,
    const motion_packet::packet &motion, std::uint64_t seal, std::uint64_t source_frame,
    const aligned::guide_constants &guides)
{
    return aligned_mode && depth_valid && seal != 0 && motion.seal == seal &&
        motion.source != 0 && motion.engine.frame_known && motion.engine.viewport_known &&
        guides.valid && guides.key.frame == source_frame &&
        motion.engine.frame == guides.key.frame && motion.engine.viewport == guides.key.viewport;
}

inline bool should_schedule(bool authoritative_guides, bool virtual_judge,
                            unsigned probe_count, bool probe_written,
                            bool learning, std::uint64_t frame)
{
    if (authoritative_guides) return false;
    if (virtual_judge)
        return probe_count < (learning ? 40u : 24u) || (frame % 1000ull) < 100ull;
    return probe_count < (learning ? 40u : 24u) && !probe_written;
}
}
