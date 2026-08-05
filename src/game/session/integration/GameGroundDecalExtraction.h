#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/simulation/world/ObjectDynamicShroud.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/player/PlayerList.h"
#include "game/session/query/GameSessionRulesetQueryPort.h"
#include "presentation/render/GroundDecalPresentationContracts.h"

#include <cstdint>

namespace engine::render_extraction_detail {

// Private extraction input assembled at the Session-owned drain boundary.
// Event vectors are detached values; ECS, players and rules remain read-only.
struct GroundDecalExtractionSource final {
    const ecs::registry& registry;
    const PlayerList& players;
    GameSessionRulesetQueryPort ruleset;
    uint64_t presentationEpoch = 0;
    uint64_t confirmedFrame = 0;
    uint32_t logicFramesPerSecond = 1;
    bool drawIconUiEnabled = true;
    container::Vector<ObjectRadiusDecalEvent> radiusEvents;
    container::Vector<ObjectDynamicShroudDecalEvent> dynamicShroudEvents;
};

[[nodiscard]] render::GroundDecalPresentationBatch
extractGroundDecalPresentation(GroundDecalExtractionSource source);

} // namespace engine::render_extraction_detail
