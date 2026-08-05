#pragma once

#include "core/container/container_types.h"

#include "math/fixed/q32_32.h"

#include <cstddef>
#include <cstdint>

namespace engine {

// Session-frozen projection of the six single-player health multipliers in
// legacy GameData. Player kind is intentionally represented as a compact
// index here: 0=human, 1=computer; difficulty is 0=easy, 1=normal, 2=hard.
// The live GameSession resolves modern PlayerControllerKind/AiDifficulty at
// the boundary, so this content value never depends on mutable player state.
struct DifficultySimulationRules final {
    using Scalar = math::q32_32;

    static constexpr size_t kHumanIndex = 0;
    static constexpr size_t kComputerIndex = 1;
    static constexpr size_t kPlayerKindCount = 2;
    static constexpr size_t kDifficultyCount = 3;

    container::Array<container::Array<Scalar, kDifficultyCount>,
                     kPlayerKindCount> healthMultipliers{{
        {Scalar{int32_t{1}}, Scalar{int32_t{1}}, Scalar{int32_t{1}}},
        {Scalar{int32_t{1}}, Scalar{int32_t{1}}, Scalar{int32_t{1}}},
    }};

    [[nodiscard]] Scalar healthMultiplier(
        size_t playerKind, size_t difficulty) const noexcept;
    void canonicalize() noexcept;

    [[nodiscard]] bool applyLegacyGameDataOverrides(
        container::StringView content, container::StringView sourceName,
        container::String* error = nullptr);

    // Uses the source INI::parsePercentToReal convention: `110` and `110%`
    // both mean 1.10. Every VFS layer is applied in authoring order.
    [[nodiscard]] static bool loadFromLegacyGameData(
        container::StringView path, DifficultySimulationRules& rules,
        container::String* error = nullptr);
};

} // namespace engine
