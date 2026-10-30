#pragma once

#include "../ext/streamline/sl_dlss_g.h"
#include <cstddef>
#include <cstring>

namespace mgpu::source_fg_options
{
static_assert(sizeof(sl::BaseStructure) == 32);
static_assert(offsetof(sl::DLSSGOptions, mode) == 32);
static_assert(offsetof(sl::DLSSGOptions, dynamicTargetFrameRate) == 116);
static_assert(sizeof(sl::DLSSGOptions) == 120);

enum class action { forwarded, already_off, rewritten };

inline std::size_t supported_size(std::size_t version)
{
    switch (version) {
    case 1: return offsetof(sl::DLSSGOptions, bReserved15);
    case 2: return offsetof(sl::DLSSGOptions, bReserved15) + sizeof(sl::Boolean);
    case 3: return offsetof(sl::DLSSGOptions, queueParallelismMode) + sizeof(sl::DLSSGQueueParallelismMode);
    case 4: return offsetof(sl::DLSSGOptions, enableUserInterfaceRecomposition) + sizeof(sl::Boolean);
    case 5: return offsetof(sl::DLSSGOptions, dynamicTargetFrameRate) + sizeof(float);
    default: return 0;
    }
}

inline action prepare(const sl::ViewportHandle &viewport, const sl::DLSSGOptions &input,
                      sl::DLSSGOptions &output, bool guard_ready)
{
    const auto bytes = supported_size(input.structVersion);
    if (!guard_ready || static_cast<uint32_t>(viewport) != 1 || !bytes ||
        input.structType != sl::DLSSGOptions::s_structType || input.next != nullptr ||
        input.mode >= sl::DLSSGMode::eCount)
        return action::forwarded;
    if (input.mode == sl::DLSSGMode::eOff) return action::already_off;
    std::memcpy(&output, &input, bytes);
    output.mode = sl::DLSSGMode::eOff;
    return action::rewritten;
}

template <typename Original>
sl::Result forward(Original original, const sl::ViewportHandle &viewport, const sl::DLSSGOptions &input,
                   bool guard_ready, action &result)
{
    sl::DLSSGOptions output{};
    result = prepare(viewport, input, output, guard_ready);
    return original(viewport, result == action::rewritten ? output : input);
}
}
