#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/definition/ModelConditionState.h"
#include "presentation/render/RenderGameDataSettings.h"
#include "presentation/render/RenderSceneSnapshot.h"

#include <cstddef>
#include <cstdint>

namespace game {
struct ThingTemplate;
}

namespace engine::script {
class ScriptObjectPresentationState;
}

namespace engine {
struct ObjectStatusComponent;
struct TrackMarksPresentationSettings;

namespace render_extraction_detail {

struct ChannelPostProcessSource final {
    const ecs::registry& registry;
    container::Span<const ObjectId> localSelection;
    const script::ScriptObjectPresentationState& objectPresentation;
    const TrackMarksPresentationSettings& trackMarks;
    const RenderBodyParticleGameData& bodyParticles;
    uint64_t simulationFrame = 0;
    int32_t logicFramesPerSecond = 1;
};

struct ChannelPostProcessInput final {
    ecs::entity entity = ecs::null;
    size_t channelIndex = 0;
    const ObjectIdentityComponent& identity;
    const TransformComponent& transform;
    const RenderModelComponent& visual;
    const ObjectProjectileComponent* projectile = nullptr;
    const ObjectKindOfComponent* kinds = nullptr;
    const ObjectHealthComponent* health = nullptr;
    const ObjectStatusComponent* status = nullptr;
    const ObjectGeometryComponent* geometry = nullptr;
    const game::ThingTemplate* templateData = nullptr;
    const game::ModelConditionMask& presentedConditions;
    const container::String& animationState;
    game::ModelAnimationMode animationMode =
        game::ModelAnimationMode::Loop;
    float animationTimeSeconds = 0.0f;
    float animationRate = 0.0f;
    float animationRuleRateFactor = 1.0f;
    float authoredAssetScale = 1.0f;
    uint64_t animationStateEnterTick = 0;
    bool animationPaused = false;
    bool hasSimulationObserver = false;
    bool alliedToObserver = false;
    // Authored ADJUST_HEIGHT_BY_CONSTRUCTION_PERCENT on the resolved
    // ConditionState. RefCode applies the resulting sink inside
    // W3DModelDraw::adjustTransformMtx, after the drawable transform is known.
    bool adjustHeightByConstructionPercent = false;
};

void appendFinalizedRenderChannel(
    const ChannelPostProcessSource& source,
    const ChannelPostProcessInput& input,
    render::RenderEntitySnapshot output,
    render::WorldRenderSnapshot& snapshot);

} // namespace render_extraction_detail
} // namespace engine
