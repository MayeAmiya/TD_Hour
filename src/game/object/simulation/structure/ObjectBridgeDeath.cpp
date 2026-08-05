#include "game/object/simulation/structure/ObjectBridgeDetail.h"

#include "core/container/string_utils.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/data/base/ObjectSimulationRules.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/lifecycle/ObjectStructureDestruction.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace engine {
namespace {

[[nodiscard]] uint64_t mixBridgeDeathSeed(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] uint64_t bridgeDeathResourceKey(
    uint64_t sessionSeed, ObjectId object, uint32_t authoredOrder,
    uint32_t resourceOrdinal, ObjectBridgeDeathEffectKind kind) noexcept {
    uint64_t value = sessionSeed ^ 0x4252494447454449ull; // "BRIDGEDI"
    value ^= static_cast<uint64_t>(object.value) * 0x9e3779b97f4a7c15ull;
    value ^= (static_cast<uint64_t>(authoredOrder) + 1u) << 24u;
    value ^= (static_cast<uint64_t>(resourceOrdinal) + 1u) << 8u;
    value ^= static_cast<uint64_t>(kind) + 1u;
    return mixBridgeDeathSeed(value);
}

[[nodiscard]] math::q32_32 fixedUnit(uint64_t value) noexcept {
    return math::q32_32::from_raw(static_cast<int64_t>(
        static_cast<uint32_t>(mixBridgeDeathSeed(value))));
}

[[nodiscard]] const game::terrain::TerrainElevatedPathfindSurface*
findBridgeSurface(
    const ecs::registry& registry, ecs::entity entity,
    const game::terrain::TerrainLogic* terrain,
    uint32_t sourcePathfindLayer) noexcept {
    if (!terrain) return nullptr;
    const MapObjectProvenanceComponent* provenance =
        ecs::try_get<MapObjectProvenanceComponent>(registry, entity);
    if (!provenance || provenance->sourceRecordIndex == UINT64_MAX) {
        return nullptr;
    }
    const game::terrain::TerrainElevatedPathfindSurface* fallback = nullptr;
    for (const game::terrain::TerrainElevatedPathfindSurface& surface :
         terrain->elevatedPathfindSurfaces()) {
        if (surface.sourceRecordIndex != provenance->sourceRecordIndex ||
            surface.boundaryRaw.size() < 3) {
            continue;
        }
        if (!fallback) fallback = &surface;
        if (surface.layer == sourcePathfindLayer) return &surface;
    }
    return fallback;
}

[[nodiscard]] LogicFixedVec3 randomBridgeSurfacePosition(
    const game::terrain::TerrainElevatedPathfindSurface* surface,
    LogicFixedVec3 fallback, uint64_t key) noexcept {
    if (!surface || surface->boundaryRaw.size() < 3) return fallback;
    // Terrain bridge surfaces are authored as the same from-left,
    // from-right, to-right, to-left convex quad used by BridgeInfo. Sampling
    // the two adjacent edges reproduces getRandomSurfacePosition without a
    // floating-point ingress in the simulation layer. For a valid convex
    // polygon with more vertices, the first/second/last triangle remains a
    // conservative point on the authored surface.
    const auto& origin = surface->boundaryRaw.front();
    const auto& right = surface->boundaryRaw[1];
    const auto& forward = surface->boundaryRaw.back();
    const math::q32_32 along = fixedUnit(key ^ 0xa5a5a5a5a5a5a5a5ull);
    const math::q32_32 across = fixedUnit(key ^ 0x5a5a5a5a5a5a5a5aull);
    const math::q32_32 above = fixedUnit(key ^ 0x3c6ef372fe94f82bull);
    const math::q32_32 effectsHeight = math::q32_32::max(
        {}, math::q32_32::from_raw(surface->transitionEffectsHeightRaw));
    const LogicFixedVec3 from{
        math::q32_32::from_raw(origin[0]),
        math::q32_32::from_raw(origin[1]),
        math::q32_32::from_raw(origin[2]),
    };
    const LogicFixedVec3 rightPoint{
        math::q32_32::from_raw(right[0]),
        math::q32_32::from_raw(right[1]),
        math::q32_32::from_raw(right[2]),
    };
    const LogicFixedVec3 forwardPoint{
        math::q32_32::from_raw(forward[0]),
        math::q32_32::from_raw(forward[1]),
        math::q32_32::from_raw(forward[2]),
    };
    return {
        from.x + (forwardPoint.x - from.x) * along +
            (rightPoint.x - from.x) * across,
        from.y + (forwardPoint.y - from.y) * along +
            (rightPoint.y - from.y) * across,
        from.z + (forwardPoint.z - from.z) * along +
            (rightPoint.z - from.z) * across + effectsHeight * above,
    };
}

[[nodiscard]] std::optional<LogicFixedVec3> pristineBonePosition(
    const ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot* content, container::StringView bone,
    LogicFixedVec3 root, math::q32_32 roll, math::q32_32 pitch,
    math::q32_32 yaw) {
    if (!content || bone.empty()) return std::nullopt;
    const game::W3dPristineBoneCatalog* catalog =
        content->pristineBoneCatalog();
    const ThingTemplateComponent* source =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, entity);
    if (!catalog || !catalog->isLoaded() || !source ||
        !source->archetype || !visual) {
        return std::nullopt;
    }
    const game::ThingTemplate& templateData =
        source->archetype->templateData;
    const size_t visualRule = game::selectModelConditionVisualRuleIndex(
        templateData, visual->modelConditionFlags);
    if (visualRule >= templateData.modelConditionVisuals.size()) {
        return std::nullopt;
    }
    const std::optional<data::w3d::FixedRigidTransform> local =
        catalog->find(source->archetype->name, visualRule, bone);
    if (!local) return std::nullopt;
    LogicFixedVec3 position{
        local->translation.x, local->translation.y,
        local->translation.z};
    const math::q32_32_sincos x = math::fixed_sincos(-roll);
    position = {
        position.x,
        position.y * x.cosine - position.z * x.sine,
        position.y * x.sine + position.z * x.cosine,
    };
    const math::q32_32_sincos y = math::fixed_sincos(pitch);
    position = {
        position.x * y.cosine + position.z * y.sine,
        position.y,
        -position.x * y.sine + position.z * y.cosine,
    };
    const math::q32_32_sincos z = math::fixed_sincos(yaw);
    position = {
        position.x * z.cosine - position.y * z.sine,
        position.x * z.sine + position.y * z.cosine,
        position.z,
    };
    return LogicFixedVec3{
        root.x + position.x,
        root.y + position.y,
        root.z + position.z,
    };
}

[[nodiscard]] LogicFixedVec3 resolveBridgeDeathPosition(
    const ecs::registry& registry, ecs::entity entity,
    const game::terrain::TerrainLogic* terrain,
    const GameContentSnapshot* content,
    const game::ObjectBridgeTimedResource& entry,
    LogicFixedVec3 root, math::q32_32 roll, math::q32_32 pitch,
    math::q32_32 yaw,
    uint32_t sourcePathfindLayer, uint64_t randomKey) {
    if (!entry.bone.empty()) {
        if (container::asciiEqualIgnoreCase(entry.bone, "ParentObject")) {
            return root;
        }
        if (const std::optional<LogicFixedVec3> bone = pristineBonePosition(
                registry, entity, content, entry.bone, root,
                roll, pitch, yaw)) {
            return *bone;
        }
        // getSingleLogicalBonePosition falls back to the object root when the
        // authored logical bone is unavailable.
        return root;
    }
    return randomBridgeSurfacePosition(
        findBridgeSurface(registry, entity, terrain, sourcePathfindLayer),
        root, randomKey);
}

void publishBridgeDeathEffect(
    ObjectBridgeDeathEffectRuntime effect, uint64_t confirmedTick,
    container::Vector<ObjectStructureEffectEvent>& effects,
    container::Vector<ObjectCreationListInvocation>& invocations) {
    if (effect.kind == ObjectBridgeDeathEffectKind::ObjectCreationList) {
        if (!effect.invocation.content) return;
        effect.invocation.confirmedTick = confirmedTick;
        invocations.push_back(std::move(effect.invocation));
        return;
    }
    if (effect.fxList.empty()) return;
    effects.push_back({
        .kind = ObjectStructureEffectKind::FxList,
        .anchor = ObjectStructureEffectAnchor::WorldPosition,
        .object = effect.invocation.source,
        .position = effect.position,
        .orientationRadians = effect.orientationRadians,
        .sourcePathfindLayer = effect.sourcePathfindLayer,
        .resource = std::move(effect.fxList),
        .authoredOrder = effect.authoredOrder,
        .emissionSequence = effect.emissionSequence,
        .confirmedTick = confirmedTick,
    });
}

} // namespace

void ObjectBridgeSystem::beginDeathOccurrence(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic* terrain,
    const GameContentSnapshot* content,
    const ObjectSimulationRules& rules, ObjectId object,
    uint32_t authoredOrder, uint64_t sessionSeed,
    uint64_t confirmedTick, uint64_t& nextEmissionSequence,
    container::Vector<ObjectBridgeDeathEffectRuntime>& pending,
    container::Vector<ObjectStructureEffectEvent>& effects,
    container::Vector<ObjectCreationListInvocation>& invocations) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return;
    const ObjectBridgeComponent* bridge =
        ecs::try_get<ObjectBridgeComponent>(registry, *entity);
    if (!bridge || !bridge->plan) return;
    const auto ruleIterator = std::find_if(
        bridge->plan->bridges.begin(), bridge->plan->bridges.end(),
        [authoredOrder](const game::ObjectBridgeBehaviorRule& rule) {
            return rule.authoredOrder == authoredOrder;
        });
    if (ruleIterator == bridge->plan->bridges.end()) return;
    const size_t ruleIndex = static_cast<size_t>(
        ruleIterator - bridge->plan->bridges.begin());
    const game::ObjectBridgeBehaviorRule& rule = *ruleIterator;

    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, *entity);
    if (!transform) return;
    const LogicFixedVec3 root = readAuthoritativeObjectPosition(
        registry, *entity, *transform);
    const ObjectPhysicsComponent* physics =
        ecs::try_get<ObjectPhysicsComponent>(registry, *entity);
    const math::q32_32 yaw = physics && physics->ownsAttitude
        ? physics->yaw
        : readAuthoritativeObjectYaw(registry, *entity, *transform);
    const math::q32_32 pitch = physics && physics->ownsAttitude
        ? physics->pitch : math::q32_32{};
    const math::q32_32 roll = physics && physics->ownsAttitude
        ? physics->roll : math::q32_32{};
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, *entity);
    const uint32_t sourcePathfindLayer = terrainLayer
        ? terrainLayer->pathfindLayer
        : game::terrain::kGroundPathfindLayer;
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, *entity);
    const PrimaryTeamComponent* team =
        ecs::try_get<PrimaryTeamComponent>(registry, *entity);
    const ObjectVeterancyComponent* veterancy =
        ecs::try_get<ObjectVeterancyComponent>(registry, *entity);
    const ObjectAirborneComponent* airborne =
        ecs::try_get<ObjectAirborneComponent>(registry, *entity);

    const auto nextSequence = [&nextEmissionSequence]() noexcept {
        const uint64_t result = nextEmissionSequence++;
        if (nextEmissionSequence == 0) ++nextEmissionSequence;
        return result;
    };
    const auto submit = [&](ObjectBridgeDeathEffectRuntime effect,
                            uint32_t delayMilliseconds) {
        effect.dueTick = detail::saturatingAdd(
            confirmedTick,
            detail::millisecondsToTicks(
                delayMilliseconds, rules.logicFramesPerSecond));
        if (effect.dueTick <= confirmedTick) {
            publishBridgeDeathEffect(std::move(effect), confirmedTick,
                                     effects, invocations);
        } else {
            pending.push_back(std::move(effect));
        }
    };

    uint32_t resourceOrdinal = 0;
    for (const game::ObjectBridgeTimedResource& entry : rule.dieFx) {
        const uint64_t emissionSequence = nextSequence();
        const uint64_t randomKey = bridgeDeathResourceKey(
            sessionSeed, object, authoredOrder, resourceOrdinal++,
            ObjectBridgeDeathEffectKind::FxList);
        submit({
            .kind = ObjectBridgeDeathEffectKind::FxList,
            .invocation = {.source = object},
            .fxList = entry.resource,
            .position = resolveBridgeDeathPosition(
                registry, *entity, terrain, content, entry, root,
                roll, pitch, yaw,
                sourcePathfindLayer, randomKey),
            // FXList::doFXPos receives only a world position.
            .orientationRadians = {},
            .sourcePathfindLayer = sourcePathfindLayer,
            .authoredOrder = authoredOrder,
            .emissionSequence = emissionSequence,
        }, entry.delayMilliseconds);
    }

    const container::Vector<game::ObjectCreationListContentId>* resolved =
        ruleIndex < bridge->dieOclContentByRule.size()
            ? &bridge->dieOclContentByRule[ruleIndex] : nullptr;
    for (size_t index = 0; index < rule.dieOcl.size(); ++index) {
        const game::ObjectBridgeTimedResource& entry = rule.dieOcl[index];
        const game::ObjectCreationListContentId ocl =
            resolved && index < resolved->size()
                ? (*resolved)[index]
                : game::INVALID_OBJECT_CREATION_LIST_CONTENT_ID;
        const uint64_t emissionSequence = nextSequence();
        if (!ocl || !owner || !team || !owner->player || !team->team) {
            ++resourceOrdinal;
            continue;
        }
        const uint64_t randomKey = bridgeDeathResourceKey(
            sessionSeed, object, authoredOrder, resourceOrdinal++,
            ObjectBridgeDeathEffectKind::ObjectCreationList);
        const LogicFixedVec3 position = resolveBridgeDeathPosition(
            registry, *entity, terrain, content, entry, root,
            roll, pitch, yaw,
            sourcePathfindLayer, randomKey);
        const bool parentObjectAnchor =
            container::asciiEqualIgnoreCase(entry.bone, "ParentObject");
        ObjectCreationListInvocation invocation{
            .content = ocl,
            .source = object,
            .owner = owner->player,
            .primaryTeam = team->team,
            .primaryPosition = position,
            .sourceVelocity = physics
                ? physics->velocityUnitsPerSecond : LogicFixedVec3{},
            // RefCode passes nullptr only for Bone:ParentObject. Every bone
            // or random-surface OCL uses an explicit position together with
            // INVALID_ANGLE, which the legacy executor projects as zero.
            .orientationRadians = parentObjectAnchor ? yaw : math::q32_32{},
            .pitchRadians = parentObjectAnchor ? pitch : math::q32_32{},
            .rollRadians = parentObjectAnchor ? roll : math::q32_32{},
            .veterancy = veterancy
                ? veterancy->level : game::ObjectVeterancyLevel::Regular,
            .authoredOrder = authoredOrder,
            .emissionSequence = emissionSequence,
            .confirmedTick = confirmedTick,
            .sourcePathfindLayer = sourcePathfindLayer,
            .sourceAirborne = airborne && airborne->isAirborne,
            .sourceOwnsFullAttitude = parentObjectAnchor &&
                physics && physics->ownsAttitude,
        };
        submit({
            .kind = ObjectBridgeDeathEffectKind::ObjectCreationList,
            .invocation = std::move(invocation),
            .position = position,
            .orientationRadians = yaw,
            .sourcePathfindLayer = sourcePathfindLayer,
            .authoredOrder = authoredOrder,
            .emissionSequence = emissionSequence,
        }, entry.delayMilliseconds);
    }
}

void ObjectBridgeSystem::updateDeathEffects(
    uint64_t confirmedTick,
    container::Vector<ObjectBridgeDeathEffectRuntime>& pending,
    container::Vector<ObjectStructureEffectEvent>& effects,
    container::Vector<ObjectCreationListInvocation>& invocations) const {
    size_t retained = 0;
    for (size_t index = 0; index < pending.size(); ++index) {
        ObjectBridgeDeathEffectRuntime& effect = pending[index];
        if (effect.dueTick > confirmedTick) {
            if (retained != index) pending[retained] = std::move(effect);
            ++retained;
            continue;
        }
        publishBridgeDeathEffect(std::move(effect), confirmedTick,
                                 effects, invocations);
    }
    pending.resize(retained);
}

} // namespace engine
