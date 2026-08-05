#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/session/integration/GameRenderTerrainExtractionCache.h"
#include "presentation/render/RenderOverlaySnapshot.h"
#include "presentation/render/RenderSceneSnapshot.h"

#include <cstdint>

namespace engine {

// Presentation-only memoization owned beside the extractor. It contains no
// authoritative simulation state and may be discarded at any lifecycle edge.
struct GameRenderExtractionCache final {
    void resetWorld() noexcept {
        worldSnapshot.reset();
        worldSelection.clear();
        worldFrame = 0;
        worldEpoch = 0;
        worldPresentationSequence = 0;
        worldObserverPolicy = 0;
        worldClientTerrainRevision = 0;
        worldDirtyRevision = 0;
        worldHover = INVALID_OBJECT_ID;
        worldShowPlayerWaypoints = false;
        worldDependencies = false;
    }

    void resetAll() noexcept {
        terrain = {};
        resetWorld();
        gameplayRadarHistory.clear();
        gameplayRadarEpoch = 0;
    }

    GameRenderTerrainExtractionCache terrain;
    container::SharedPtr<const render::WorldRenderSnapshot> worldSnapshot;
    container::Vector<ObjectId> worldSelection;
    uint64_t worldFrame = 0;
    uint64_t worldEpoch = 0;
    uint64_t worldPresentationSequence = 0;
    uint64_t worldObserverPolicy = 0;
    uint64_t worldClientTerrainRevision = 0;
    uint64_t worldDirtyRevision = 0;
    ObjectId worldHover = INVALID_OBJECT_ID;
    bool worldShowPlayerWaypoints = false;
    bool worldDependencies = false;
    container::Vector<render::TacticalRadarEventRenderSnapshot>
        gameplayRadarHistory;
    uint64_t gameplayRadarEpoch = 0;
};

} // namespace engine
