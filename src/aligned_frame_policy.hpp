#pragma once

#include <array>
#include <cstdint>

namespace mgpu::aligned
{
enum class part : unsigned { color, depth, motion, count };
struct frame_key
{
    std::uint32_t frame = 0;
    std::uint32_t viewport = 0;
    bool operator==(const frame_key &other) const
    {
        return frame == other.frame && viewport == other.viewport;
    }
};
inline bool newer_frame(std::uint32_t current, std::uint32_t previous)
{
    const auto distance = current - previous;
    return distance != 0 && distance < 0x80000000u;
}
inline bool consecutive_frame(std::uint32_t current, std::uint32_t previous)
{
    return current - previous == 1u;
}
struct completion
{
    unsigned queue = 0;
    std::uint64_t value = 0;
};
struct camera_constants
{
    std::array<float, 16> view_to_clip{}, clip_to_lens{};
    std::array<float, 2> pinhole{};
    std::array<float, 3> position{}, up{}, right{}, forward{};
    float aspect = 0;
    float invalid_motion = 0;
    float min_depth_separation = 40.0f;
    std::uint64_t capture_ns = 0;
    bool valid = false;
};
struct guide_constants
{
    frame_key key{};
    float motion_scale_x = 0, motion_scale_y = 0;
    float jitter_x = 0, jitter_y = 0;
    std::array<float, 16> clip_to_previous{}, previous_to_clip{}, clip_to_view{};
    float camera_near = 0, camera_far = 0, camera_fov = 0;
    bool motion_dilated = false;
    bool spatial_valid = false;
    bool depth_inverted = false;
    bool reset = false;
    bool valid = false;
    camera_constants camera{};

    std::array<float, 2> motion_subrect_scale(unsigned motion_width, unsigned motion_height) const
    {
        // Streamline normalizes motion; NR consumes pixels of the bound motion subrect.
        return {motion_scale_x * float(motion_width), motion_scale_y * float(motion_height)};
    }
};
struct frame_state
{
    frame_key key{};
    std::uint64_t serial = 0;
    std::array<std::uint64_t, 3> commands{};
    std::array<completion, 3> copied{};
    std::uint64_t release_command = 0;
    completion released{};
    bool leased = false;

    bool recorded() const
    {
        return serial != 0 && !leased && release_command == 0 &&
            commands[0] != 0 && commands[1] != 0 && commands[2] != 0;
    }

    bool cancel(std::uint64_t command)
    {
        if (command == 0) return true;
        if (release_command == command) return false;
        for (unsigned index = 0; index != 3; ++index)
            if (commands[index] == command && copied[index].value == 0)
                commands[index] = 0;
        return true;
    }

    template <typename Completed>
    bool ready(Completed completed) const
    {
        if (serial == 0 || leased || release_command != 0) return false;
        for (unsigned index = 0; index != 3; ++index)
        {
            const auto ticket = copied[index];
            const auto value = completed(ticket.queue);
            if (commands[index] == 0 || ticket.value == 0 || value == UINT64_MAX || value < ticket.value)
                return false;
        }
        return true;
    }

    bool schedulable() const
    {
        if (!recorded()) return false;
        for (const auto &ticket : copied)
            if (ticket.value == 0 || ticket.value == UINT64_MAX) return false;
        return true;
    }

    template <typename Completed>
    bool reclaimable(Completed completed) const
    {
        if (serial == 0) return true;
        if (leased || release_command == 0 || released.value == 0) return false;
        const auto value = completed(released.queue);
        return value != UINT64_MAX && value >= released.value;
    }

    template <typename Completed>
    bool abandoned_complete(Completed completed) const
    {
        if (leased || release_command != 0) return false;
        for (unsigned index = 0; index != 3; ++index)
        {
            if (commands[index] == 0) continue;
            const auto ticket = copied[index];
            const auto value = completed(ticket.queue);
            if (ticket.value == 0 || value == UINT64_MAX || value < ticket.value) return false;
        }
        return true;
    }

    void submitted(std::uint64_t command, completion ticket)
    {
        if (command == 0 || ticket.value == 0) return;
        for (unsigned index = 0; index != 3; ++index)
            if (commands[index] == command) copied[index] = ticket;
        if (release_command == command) released = ticket;
    }
};
}
