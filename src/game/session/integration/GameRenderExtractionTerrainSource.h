#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/session/integration/GameRenderTerrainExtractionCache.h"
#include "game/session/query/GameSessionRulesetQueryPort.h"
#include "game/render/LocalPlacementPresentationState.h"

#include <cstdint>

namespace game {
namespace terrain {
class TerrainLogic;
}
}

namespace engine {
class ClientTerrainObjectStore;
class GameContentSnapshot;
class ObjectOwnershipIndex;
class PlayerRegistry;
struct RenderGameDataSettings;
namespace script {
class ScriptMapPresentationState;
struct ScriptTerrainRoadPresentationSettings;
struct ScriptWaterPresentationSettings;
}

// Explicit read-only capabilities required by terrain admission and final
// world assembly. The only mutable member is the extractor-owned cache.
struct GameRenderTerrainExtractionSource final {
    const ecs::registry& registry;
    const GameContentSnapshot& content;
    const PlayerRegistry& players;
    const game::terrain::TerrainLogic& terrain;
    const ObjectOwnershipIndex& ownership;
    container::Span<const ObjectId> localSelection;
    bool showPlayerWaypoints = false;
    const ClientTerrainObjectStore& clientTerrainObjects;
    const selection::LocalPlacementPresentationState& localPlacement;
    container::Span<const selection::LocalPlacementPreviewSnapshot>
        queuedConstructionPlacements;
    container::Span<const selection::TimedLocalPlacementPreview>
        rejectedConstructionPlacements;
    const script::ScriptMapPresentationState& mapPresentation;
    const script::ScriptWaterPresentationSettings& waterPresentation;
    const script::ScriptTerrainRoadPresentationSettings& roadPresentation;
    const RenderGameDataSettings& renderSettings;
    GameSessionRulesetQueryPort ruleset;
    GameRenderTerrainExtractionCache& cache;
    uint64_t presentationEpoch = 0;
    uint64_t confirmedTick = 0;
};

} // namespace engine
