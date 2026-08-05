#pragma once

#include <cstdint>

namespace engine::ground_decals::performance_limits {

// Immutable validation ceilings. The smaller session-selected operational
// budgets live in RenderOperationalBudget (4096 instances / 128 textures by
// default) and are never allowed to raise these values.
inline constexpr uint32_t kHardMaximumInstancesPerFrame = 65536;
inline constexpr uint32_t kHardMaximumResidentTextures = 4096;
inline constexpr uint32_t kHardMaximumPersistentOwners = 65536;
// Covers the stock worst visible combination of one 30 x radius-100 dynamic
// shroud grid (about 12,000 heightfield tiles) plus ordinary shadows/scorches.
// This is an operational default, not an authored radius or count clamp.
inline constexpr uint32_t kDefaultMaximumInstancesPerFrame = 16384;
inline constexpr uint32_t kDefaultMaximumResidentTextures = 128;

struct OperationalBudget final {
    uint32_t maximumInstancesPerFrame = 0;
    uint32_t maximumResidentTextures = 0;
};

[[nodiscard]] constexpr OperationalBudget operationalBudget(
    uint32_t maximumInstancesPerFrame,
    uint32_t maximumResidentTextures) noexcept {
    return {
        .maximumInstancesPerFrame =
            maximumInstancesPerFrame < kHardMaximumInstancesPerFrame
            ? maximumInstancesPerFrame
            : kHardMaximumInstancesPerFrame,
        .maximumResidentTextures =
            maximumResidentTextures < kHardMaximumResidentTextures
            ? maximumResidentTextures
            : kHardMaximumResidentTextures,
    };
}

static_assert(operationalBudget(
    kDefaultMaximumInstancesPerFrame,
    kDefaultMaximumResidentTextures).maximumInstancesPerFrame ==
    kDefaultMaximumInstancesPerFrame);
static_assert(operationalBudget(UINT32_MAX, UINT32_MAX).maximumResidentTextures ==
              kHardMaximumResidentTextures);

} // namespace engine::ground_decals::performance_limits
