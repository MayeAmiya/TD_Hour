#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "presentation/render/RenderSceneSnapshot.h"

#include <cstdint>

namespace game {
struct ThingTemplate;
}

namespace engine {
class GameContentSnapshot;
class MultiplayerRuleset;
class ObjectLifecycle;
class PlayerRegistry;
struct ObjectContainmentPlan;

namespace render_extraction_detail {

struct ObjectUiExtractionSource final {
    const ecs::registry& registry;
    const ObjectLifecycle& lifecycle;
    const PlayerRegistry& players;
    const MultiplayerRuleset* ruleset = nullptr;
    const GameContentSnapshot& content;
    container::Span<const ObjectId> localSelection;
    ObjectId localHover = INVALID_OBJECT_ID;
    uint64_t simulationFrame = 0;
    uint32_t logicFramesPerSecond = 1;
};

struct ObjectUiEntityInput final {
    ecs::entity entity = ecs::null;
    const ObjectIdentityComponent& identity;
    const TransformComponent& transform;
    const RenderModelComponent& visual;
    const OwnerComponent* owner = nullptr;
    const ObjectGeometryComponent* geometry = nullptr;
    const ObjectKindOfComponent* kinds = nullptr;
    const game::ThingTemplate* templateData = nullptr;
    const ObjectContainmentPlan* containmentPlan = nullptr;
    const ObjectWeaponComponent* weapons = nullptr;
    const ObjectHealthComponent* health = nullptr;
    render::RenderVector worldPosition{};
    float visibilityRadius = 0.0f;
    render::LocalVisibilityRenderCellState localVisibilityState =
        render::LocalVisibilityRenderCellState::Visible;
    uint64_t selectionFlashIdentity = 0;
    PlayerId observerId = INVALID_PLAYER_ID;
    bool beaconHiddenFromLocalObserver = false;
    bool hasSimulationObserver = false;
};

void appendObjectUiPresentation(
    const ObjectUiExtractionSource& source,
    const ObjectUiEntityInput& input,
    container::Vector<render::TacticalRadarEventRenderSnapshot>&
        radarCandidates,
    render::WorldRenderSnapshot& snapshot);

} // namespace render_extraction_detail
} // namespace engine
