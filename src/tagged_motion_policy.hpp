#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mgpu::tagged_motion
{
struct source_priority
{
    bool tagged_only = false;

    bool accepts(bool tagged) const { return tagged || !tagged_only; }
    bool recorded(bool tagged)
    {
        const bool changed = tagged && !tagged_only;
        tagged_only = tagged_only || tagged;
        return changed;
    }
};

inline bool whole_extent(unsigned top, unsigned left, unsigned width,
                         unsigned height, unsigned resource_width, unsigned resource_height)
{
    return top == 0 && left == 0 &&
        ((width == 0 && height == 0) ||
         (width == resource_width && height == resource_height));
}

struct pointer_matches
{
    void **slot = nullptr;
    unsigned count = 0;

    void scan(void *memory, size_t bytes, void *target)
    {
        if (memory == nullptr || target == nullptr) return;
        auto *data = static_cast<unsigned char *>(memory);
        const size_t alignment = alignof(void *);
        const size_t first = (alignment - reinterpret_cast<uintptr_t>(data) % alignment) % alignment;
        for (size_t offset = first; offset + sizeof(void *) <= bytes; offset += alignment)
        {
            void *value = nullptr;
            std::memcpy(&value, data + offset, sizeof value);
            if (value == target)
            {
                slot = reinterpret_cast<void **>(data + offset);
                ++count;
            }
        }
    }

    void **unique_slot() const { return count == 1 ? slot : nullptr; }
};
}
