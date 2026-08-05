#pragma once

#include "core/container/container_types.h"

#include "math/fixed/q32_32.h"

#include <cstddef>
#include <cstdint>
namespace engine {

// Immutable-at-session-start projection of GlobalData's veterancy health
// multipliers. Regular is an implicit identity value: the source parse table
// deliberately exposes only Veteran, Elite and Heroic, so malformed content
// can never redefine the base level for a live match.
struct VeterancySimulationRules final {
    using Scalar = math::q32_32;

    static constexpr size_t kRegularLevelIndex = 0;
    static constexpr size_t kVeteranLevelIndex = 1;
    static constexpr size_t kEliteLevelIndex = 2;
    static constexpr size_t kHeroicLevelIndex = 3;
    static constexpr size_t kLevelCount = 4;

    Scalar veteranHealthBonus{int32_t{1}};
    Scalar eliteHealthBonus{int32_t{1}};
    Scalar heroicHealthBonus{int32_t{1}};

    // Returns identity for Regular and for an invalid index. This keeps a
    // future ExperienceTracker consumer total at the enum/data boundary.
    [[nodiscard]] Scalar healthBonusForLevelIndex(size_t levelIndex) const noexcept;

    // Authored multipliers, including zero/negative legacy values, remain
    // observable; only representation-equivalent normalization belongs here.
    void canonicalize() noexcept;

    [[nodiscard]] bool applyLegacyGameDataOverrides(
        container::StringView content, container::StringView sourceName,
        container::String* error = nullptr);

    // Reads the VFS winner of a legacy `GameData ... End` source. Explicitly
    // ordered logical GameData sources remain separate. HealthBonus_* values
    // use INI::parsePercentToReal semantics: both
    // `110` and `110%` represent a multiplier of 1.10. Malformed or
    // unrepresentable fields retain the prior/default value with a diagnostic.
    [[nodiscard]] static bool loadFromLegacyGameData(
        container::StringView path, VeterancySimulationRules& rules,
        container::String* error = nullptr);
};

} // namespace engine
