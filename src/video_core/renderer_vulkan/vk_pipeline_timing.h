// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cerrno>
#include <cstdlib>
#include <optional>

#include "common/common_types.h"

namespace Vulkan {

inline bool PipelineTimingEnabled(u64 hash) {
    static const bool enabled = [] {
        const char* value = std::getenv("SUYU_VK_PIPELINE_TIMING");
        return value && *value && *value != '0';
    }();
    if (!enabled) {
        return false;
    }
    static const std::optional<u64> target = []() -> std::optional<u64> {
        const char* value = std::getenv("SUYU_VK_PIPELINE_TIMING_HASH");
        if (!value || !*value) {
            return std::nullopt;
        }
        char* end{};
        errno = 0;
        const auto parsed = std::strtoull(value, &end, 16);
        return end != value && *end == '\0' && errno != ERANGE
                   ? std::optional<u64>{parsed}
                   : std::optional<u64>{0};
    }();
    return !target || *target == hash;
}

} // namespace Vulkan
