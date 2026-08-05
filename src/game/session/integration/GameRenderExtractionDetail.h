#pragma once

#include "core/container/hash_containers.h"
#include "core/ecs/registry.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "presentation/render/RenderSceneSnapshot.h"
#include "presentation/render/RenderGameDataSettings.h"

#include <cstdint>
#include <optional>

namespace engine {
class GameContentSnapshot;
class ObjectLifecycle;
}

namespace engine::render_extraction_detail {

struct WeaponPresentationSource final {
    const GameContentSnapshot& content;
    const void* cacheOwner = nullptr;
    uint64_t presentationEpoch = 0;
};

struct ProjectilePresentationSource final {
    const ecs::registry& registry;
    const ObjectLifecycle& objects;
    const GameContentSnapshot& content;
    const RenderFeatureQualitySettings& featureQuality;
    const render::LocalVisibilityRenderSnapshot& localVisibility;
    int32_t logicFramesPerSecond = 1;
};

[[nodiscard]] bool hasObjectKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf sought) noexcept;
[[nodiscard]] bool equalsInsensitive(
    container::StringView left,
    container::StringView right) noexcept;

void appendThingVisualAssetDependencies(
    render::WorldRenderSnapshot& snapshot,
    container::HashSet<container::String>& dependencyKeys,
    const game::ThingTemplate& templateData,
    game::ModelConditionMask currentConditions);
void appendWeaponProjectileVisualAssetDependencies(
    render::WorldRenderSnapshot& snapshot,
    container::HashSet<container::String>& dependencyKeys,
    const GameContentSnapshot& content,
    const ObjectWeaponComponent* weapons,
    game::ModelConditionMask sourceConditions);
void appendSpecialPowerVisualAssetDependencies(
    render::WorldRenderSnapshot& snapshot,
    container::HashSet<container::String>& dependencyKeys,
    const GameContentSnapshot& content,
    const game::ObjectArchetype& archetype,
    game::ModelConditionMask sourceConditions);

[[nodiscard]] uint64_t radarEventIdentity(
    uint64_t producer,
    uint64_t createTick,
    int32_t eventType) noexcept;
void appendAcceptedGameplayRadarEvents(
    container::Vector<render::TacticalRadarEventRenderSnapshot>& candidates,
    container::Vector<render::TacticalRadarEventRenderSnapshot>& destination,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond);
void updateGameplayRadarHistoryAndAppend(
    container::Vector<render::TacticalRadarEventRenderSnapshot>& history,
    uint64_t& historyEpoch,
    uint64_t presentationEpoch,
    container::Vector<render::TacticalRadarEventRenderSnapshot> candidates,
    container::Vector<render::TacticalRadarEventRenderSnapshot>& destination,
    uint64_t simulationFrame,
    uint32_t logicFramesPerSecond);

[[nodiscard]] render::RenderEntityId renderInstanceId(
    uint32_t objectId,
    size_t channelIndex) noexcept;
[[nodiscard]] render::RenderEntityId clientTerrainObjectId(
    uint32_t clientObjectId) noexcept;
[[nodiscard]] render::RenderEntityId clientTerrainInstanceId(
    uint32_t clientObjectId,
    uint32_t channelIndex) noexcept;
[[nodiscard]] uint64_t modelParticleEmitterIdentity(
    render::RenderEntityId instanceId,
    uint32_t phaseIdentity,
    size_t declarationOrdinal) noexcept;

[[nodiscard]] render::ProjectileTrailBlendMode projectileTrailBlend(
    game::ProjectileStreamBlendMode source) noexcept;
[[nodiscard]] render::ProjectileTrailDepthMode projectileTrailDepth(
    game::ProjectileStreamDepthMode source) noexcept;
[[nodiscard]] std::optional<math::quat> projectileFlightOrientation(
    const ObjectProjectileComponent& projectile,
    uint64_t simulationFrame,
    bool preserveLaunchOrientation = false) noexcept;
void appendProjectilePresentation(
    const ProjectilePresentationSource& source,
    ecs::entity entity,
    const ObjectIdentityComponent& identity,
    const TransformComponent& transform,
    const ObjectGeometryComponent* geometry,
    const ObjectProjectileComponent* projectile,
    bool hiddenByLocalVisibility,
    bool alliedToObserver,
    render::WorldRenderSnapshot& snapshot);

[[nodiscard]] render::RenderAnimationMode toRenderAnimationMode(
    game::ModelAnimationMode mode);
[[nodiscard]] render::RenderAnimationStartKind toRenderAnimationStartKind(
    VisualAnimationStartKind kind) noexcept;

[[nodiscard]] container::Array<
    container::String,
    render::kRenderWeaponSlotCount>
extractWeaponLaunchBones(
    const game::ThingTemplate& tmpl,
    game::ModelConditionMask conditions);
[[nodiscard]] container::Vector<render::RenderBoneControl>
extractTurretControls(
    const ObjectWeaponComponent* weapons,
    const container::Array<game::ModelTurretBoneDefinition, 2>& definitions);
void appendVehicleDrawControls(
    const game::VehicleDrawVisualRecipe& recipe,
    const VehicleDrawChannelPresentationState& state,
    container::Vector<render::RenderBoneControl>& controls,
    render::RenderVehicleTreadState& treads);
[[nodiscard]] container::Array<
    container::String,
    render::kRenderWeaponSlotCount>
typedWeaponLaunchBones(
    const container::Array<game::ModelWeaponBoneDefinition, 3>& definitions);
[[nodiscard]] const container::Array<game::W3dWeaponBarrelTable, 3>*
resolveWeaponBarrelTables(
    const WeaponPresentationSource& source,
    container::StringView archetypeName,
    size_t poseRuleIndex,
    const container::Array<game::ModelWeaponBoneDefinition, 3>& definitions);
void appendWeaponPresentationControls(
    const WeaponPresentationSource& source,
    const ObjectWeaponComponent* weapons,
    const RenderModelComponent& visual,
    const container::Array<game::ModelWeaponBoneDefinition, 3>& definitions,
    const container::Array<game::W3dWeaponBarrelTable, 3>* barrelTables,
    uint8_t projectileBoneFeedbackEnabledSlots,
    const game::ModelWeaponRecoilProfile& recoil,
    uint64_t simulationFrame,
    container::Vector<render::RenderSubObjectVisibility>& visibility,
    container::Vector<render::RenderWeaponImpulse>& weaponImpulses);

} // namespace engine::render_extraction_detail
