#pragma once

#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"
#include "presentation/render/RenderSceneSnapshot.h"

#include <cstdint>

namespace engine {

class GameContentSnapshot;

// Appends RefCode's client-only GarrisonGun drawables.  These rows are not
// ECS objects and carry no gameplay identity; they are immutable render
// descriptions owned by the enclosing host and keyed by FIREPOINT index.
void appendGarrisonGunPresentation(
    const ecs::registry& registry,
    const GameContentSnapshot& content,
    ecs::entity hostEntity,
    ObjectId host,
    render::LocalVisibilityRenderCellState visibilityState,
    bool hiddenByLocalVisibility,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond,
    render::WorldRenderSnapshot& snapshot);

[[nodiscard]] bool hasDueGarrisonPresentationBoundary(
    const ecs::registry& registry,
    ecs::entity hostEntity,
    uint64_t cachedFrame,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond) noexcept;

} // namespace engine
