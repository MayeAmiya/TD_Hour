#pragma once

#include "core/ecs/registry.h"
#include "presentation/render/RenderSceneSnapshot.h"

#include <cstdint>

namespace engine {

class GameContentSnapshot;

// Appends JetAIUpdate's client-only LockonCursor drawable. Gameplay owns the
// targeter set and lock-on deadline; this extractor only converts those
// confirmed values into immutable render rows.
void appendJetLockonPresentation(
    const ecs::registry& registry,
    const GameContentSnapshot& content,
    ecs::entity jetEntity,
    render::LocalVisibilityRenderCellState visibilityState,
    bool hiddenByLocalVisibility,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond,
    render::WorldRenderSnapshot& snapshot);

[[nodiscard]] bool hasActiveJetLockonPresentation(
    const ecs::registry& registry,
    ecs::entity jetEntity,
    uint64_t cachedFrame,
    uint64_t simulationFrame) noexcept;

} // namespace engine
