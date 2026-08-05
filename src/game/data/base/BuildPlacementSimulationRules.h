#pragma once

#include "core/container/container_types.h"
#include "game/data/base/TerrainConstructionCatalog.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace engine {

// Session-frozen gameplay values consumed by BuildAssistant-compatible
// placement checks. They are authoring rules, never renderer budgets.
struct BuildPlacementSimulationRules final {
    // Authoring input is parsed through a temporary float, but the winning
    // session record is quantized here. Placement authority never converts
    // content values at the point of use.
    math::q32_32 minimumDistanceFromMapEdge{};
    math::q32_32 supplyBuildBorder{};
    math::q32_32 allowedHeightVariation{};
    // Original GlobalData::MaxLineBuildObjects. This is a gameplay/lockstep
    // limit rather than a presentation budget: both preview and authority
    // must plan the same bounded line from the session-frozen value.
    uint32_t maxLineBuildObjects = 0;
    // Terrain.ini is gameplay input for BuildAssistant terrain restrictions.
    // This catalog is copied by value through ObjectSimulationRules at match
    // startup, so an active session never queries renderer state or mutable
    // VFS content during a placement check.
    TerrainConstructionCatalog terrainTypes;

    void canonicalize() noexcept;

    [[nodiscard]] bool applyLegacyGameDataOverrides(
        container::StringView content, container::StringView sourceName,
        container::String* error = nullptr);

    [[nodiscard]] static bool loadFromLegacyGameData(
        container::StringView path, BuildPlacementSimulationRules& rules,
        container::String* error = nullptr);
};

} // namespace engine
