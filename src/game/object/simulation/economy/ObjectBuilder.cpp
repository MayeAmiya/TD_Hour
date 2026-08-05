#include "game/object/simulation/economy/ObjectBuilder.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/navigation/runtime/NavigationSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace {

[[nodiscard]] engine::navigation::NavigationMovementMask
builderNavigationMovementMask(
    const engine::ObjectLocomotionComponent& locomotion) noexcept {
    static_assert(
        game::locomotorSurfaceBit(game::LocomotorSurface::Ground) ==
            engine::navigation::NavigationMovement::Ground &&
        game::locomotorSurfaceBit(game::LocomotorSurface::Water) ==
            engine::navigation::NavigationMovement::Water &&
        game::locomotorSurfaceBit(game::LocomotorSurface::Cliff) ==
            engine::navigation::NavigationMovement::Cliff &&
        game::locomotorSurfaceBit(game::LocomotorSurface::Air) ==
            engine::navigation::NavigationMovement::Air &&
        game::locomotorSurfaceBit(game::LocomotorSurface::Rubble) ==
            engine::navigation::NavigationMovement::Rubble);
    return static_cast<engine::navigation::NavigationMovementMask>(
        locomotion.surfaces);
}

[[nodiscard]] bool builderTargetSharesReachableZone(
    const ecs::registry& registry,
    const engine::navigation::NavigationSystem* navigation,
    ecs::entity builder, ecs::entity target) noexcept {
    if (!navigation || !navigation->isInitialized() ||
        !navigation->topologyQueriesAvailable()) {
        return true;
    }
    const engine::ObjectLocomotionComponent* locomotion =
        ecs::try_get<engine::ObjectLocomotionComponent>(registry, builder);
    const engine::ObjectGeometryComponent* geometry =
        ecs::try_get<engine::ObjectGeometryComponent>(registry, builder);
    const engine::TransformComponent* builderTransform =
        ecs::try_get<engine::TransformComponent>(registry, builder);
    const engine::TransformComponent* targetTransform =
        ecs::try_get<engine::TransformComponent>(registry, target);
    if (!locomotion || !geometry || !builderTransform || !targetTransform)
        return true;

    const engine::ObjectTerrainLayerComponent* builderTerrainLayer =
        ecs::try_get<engine::ObjectTerrainLayerComponent>(registry, builder);
    const engine::ObjectTerrainLayerComponent* targetTerrainLayer =
        ecs::try_get<engine::ObjectTerrainLayerComponent>(registry, target);
    engine::navigation::NavigationLayerId builderLayer;
    engine::navigation::NavigationLayerId targetLayer;
    if (!engine::navigation::tryNavigationLayerFromTerrainPathfindLayer(
            builderTerrainLayer
                ? builderTerrainLayer->pathfindLayer
                : game::terrain::kGroundPathfindLayer,
            builderLayer) ||
        !engine::navigation::tryNavigationLayerFromTerrainPathfindLayer(
            targetTerrainLayer
                ? targetTerrainLayer->pathfindLayer
                : game::terrain::kGroundPathfindLayer,
            targetLayer) ||
        builderLayer != targetLayer) {
        return false;
    }
    const engine::navigation::NavigationMovementMask movement =
        builderNavigationMovementMask(*locomotion);
    if (movement == 0) return false;
    const engine::navigation::NavigationGrid* grid =
        navigation->layers().find(builderLayer);
    if (!grid) return true;
    const engine::navigation::NavigationClearanceClass clearance =
        engine::navigation::clearanceClassForRadiusRaw(
            math::q32_32::max(
                math::q32_32{}, geometry->boundingCircleRadiusFixed).raw(),
            grid->transform().cellSizeRaw);
    const engine::LogicFixedVec3 builderPosition =
        engine::readAuthoritativeObjectPosition(
            registry, builder, *builderTransform);
    const engine::LogicFixedVec3 targetPosition =
        engine::readAuthoritativeObjectPosition(
            registry, target, *targetTransform);
    const auto adjustedCell = [&](const engine::LogicFixedVec3& position) {
        return engine::navigation::adjustNavigationDestination(
            navigation->layers(), {
                .desired = {
                    position.x.raw(), position.y.raw(), position.z.raw()},
                .layer = builderLayer,
                .movementMask = movement,
                .clearance = clearance,
                .allowAdjustment = true,
            });
    };
    const engine::navigation::NavigationDestinationAdjustmentResult
        builderDestination = adjustedCell(builderPosition);
    const engine::navigation::NavigationDestinationAdjustmentResult
        targetDestination = adjustedCell(targetPosition);
    if (!builderDestination.accepted() || !targetDestination.accepted())
        return false;
    const engine::navigation::NavigationCellId builderCell =
        builderDestination.location.cell;
    const engine::navigation::NavigationCellId targetCell =
        targetDestination.location.cell;
    for (const engine::navigation::NavigationZoneField& zones :
         navigation->layerZones()) {
        if (zones.isBuilt() && zones.layer() == builderLayer &&
            zones.clearanceClass() == clearance &&
            zones.movementMask() == movement) {
            return zones.sameZone(builderCell, targetCell);
        }
    }
    return true;
}

void enterWorkerDozerMode(ecs::registry& registry,
                          ecs::entity builderEntity) noexcept {
    engine::ObjectEconomyComponent* economy =
        ecs::try_get<engine::ObjectEconomyComponent>(registry,
                                                      builderEntity);
    if (!economy || !economy->plan) return;
    const size_t count = std::min(economy->plan->supplyTrucks.size(),
                                  economy->supplyTrucks.size());
    for (size_t index = 0; index < count; ++index) {
        if (!economy->plan->supplyTrucks[index].workerMode) continue;
        engine::ObjectSupplyTruckRuntime& supply =
            economy->supplyTrucks[index];
        supply.workerSupplyActive = false;
        supply.preferredDock = engine::INVALID_OBJECT_ID;
        supply.externalIdleSuppressed = false;
        supply.regroupMoveIssued = false;
    }
}

[[nodiscard]] uint64_t millisecondsToTicks(uint32_t milliseconds,
                                           uint32_t fps) noexcept {
    if (milliseconds == 0) return 0;
    return (static_cast<uint64_t>(milliseconds) * std::max(1u, fps) + 999u) /
        1000u;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return left > std::numeric_limits<uint64_t>::max() - right
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

[[nodiscard]] math::q32_32 distanceSquared(
    const engine::LogicFixedVec3& left,
    const engine::LogicFixedVec3& right) noexcept {
    const math::q32_32 dx = left.x - right.x;
    const math::q32_32 dy = left.y - right.y;
    return dx * dx + dy * dy;
}

[[nodiscard]] bool hasKind(const engine::ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] bool isBuilderMove(const engine::ObjectOrderIntent& order,
                                 size_t instance) noexcept {
    return order.source == engine::ObjectOrderSource::System &&
        order.systemPurpose == engine::ObjectOrderSystemPurpose::Builder &&
        order.systemPurposeInstance == instance;
}

[[nodiscard]] constexpr std::optional<size_t> builderTaskSlotIndex(
    engine::ObjectBuilderTaskKind kind) noexcept {
    switch (kind) {
    case engine::ObjectBuilderTaskKind::Build: return 0;
    case engine::ObjectBuilderTaskKind::Repair: return 1;
    case engine::ObjectBuilderTaskKind::Fortify: return 2;
    case engine::ObjectBuilderTaskKind::None: break;
    }
    return std::nullopt;
}

[[nodiscard]] bool validBuilderTask(
    const engine::ObjectBuilderTask& task) noexcept {
    return task.kind != engine::ObjectBuilderTaskKind::None && task.target;
}

[[nodiscard]] bool hasAuthoritativeTaskSlots(
    const engine::ObjectBuilderRuntime& runtime) noexcept {
    return std::any_of(runtime.taskSlots.begin(), runtime.taskSlots.end(),
                       validBuilderTask);
}

void markConstructionPresentationDirty(ecs::registry& registry,
                                      ecs::entity entity) {
    engine::markObjectDirty(
        registry, entity,
        engine::objectDirtyBit(engine::ObjectDirtyDomain::ModelCondition) |
            engine::objectDirtyBit(
                engine::ObjectDirtyDomain::RenderExtraction));
}

// Existing focused callers historically populated `current` directly. Seed
// the new RefCode-compatible slot authority once so that ABI-compatible setup
// keeps working while all production ingress writes slots immediately.
void seedTaskSlotsFromCompatibilityState(
    engine::ObjectBuilderRuntime& runtime) noexcept {
    if (hasAuthoritativeTaskSlots(runtime)) return;
    const engine::ObjectBuilderTask* seed = nullptr;
    if (validBuilderTask(runtime.current)) seed = &runtime.current;
    else if (runtime.suspendedByDisable && validBuilderTask(runtime.previous))
        seed = &runtime.previous;
    if (!seed) return;
    if (const std::optional<size_t> slot = builderTaskSlotIndex(seed->kind))
        runtime.taskSlots[*slot] = *seed;
}

[[nodiscard]] engine::ObjectBuilderTask taskFromSlot(
    const engine::ObjectBuilderRuntime& runtime,
    engine::ObjectBuilderTaskKind kind) noexcept {
    if (const std::optional<size_t> slot = builderTaskSlotIndex(kind)) {
        if (validBuilderTask(runtime.taskSlots[*slot]))
            return runtime.taskSlots[*slot];
    }
    // Read compatibility for value-injected runtimes before their first
    // confirmed Builder update.
    if (runtime.current.kind == kind && validBuilderTask(runtime.current))
        return runtime.current;
    if (runtime.suspendedByDisable && runtime.previous.kind == kind &&
        validBuilderTask(runtime.previous))
        return runtime.previous;
    return {};
}

[[nodiscard]] engine::ObjectBuilderTask mostRecentPendingTask(
    const engine::ObjectBuilderRuntime& runtime) noexcept {
    engine::ObjectBuilderTask selected;
    bool found = false;
    // Strict `>` and authored enum order intentionally reproduce RefCode:
    // equal-frame commands choose Build, then Repair, then Fortify.
    for (const engine::ObjectBuilderTaskKind kind : {
             engine::ObjectBuilderTaskKind::Build,
             engine::ObjectBuilderTaskKind::Repair,
             engine::ObjectBuilderTaskKind::Fortify}) {
        const engine::ObjectBuilderTask candidate = taskFromSlot(runtime, kind);
        if (!validBuilderTask(candidate)) continue;
        if (!found || candidate.issuedTick > selected.issuedTick) {
            selected = candidate;
            found = true;
        }
    }
    return selected;
}

void storePendingTask(engine::ObjectBuilderRuntime& runtime,
                      const engine::ObjectBuilderTask& task) noexcept {
    if (const std::optional<size_t> slot = builderTaskSlotIndex(task.kind))
        runtime.taskSlots[*slot] = task;
}

void clearPendingTask(engine::ObjectBuilderRuntime& runtime,
                      engine::ObjectBuilderTaskKind kind) noexcept {
    if (const std::optional<size_t> slot = builderTaskSlotIndex(kind))
        runtime.taskSlots[*slot] = {};
}

void projectMostRecentTask(engine::ObjectBuilderRuntime& runtime,
                           uint64_t confirmedTick) noexcept {
    runtime.current = mostRecentPendingTask(runtime);
    runtime.requireClearRepairTarget =
        runtime.current.requireClearRepairTarget;
    if (validBuilderTask(runtime.current)) {
        runtime.phase = engine::ObjectBuilderPhase::Approaching;
    } else {
        runtime.phase = engine::ObjectBuilderPhase::Idle;
        runtime.idleSinceTick = confirmedTick;
        runtime.nextBoredScanTick = 0;
    }
}

[[nodiscard]] const engine::ObjectBuilderRuntime* findBuilderRuntime(
    const ecs::registry& registry, const engine::ObjectLifecycle& lifecycle,
    engine::ObjectId builder, size_t moduleIndex) {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(builder);
    if (!entity) return nullptr;
    const engine::ObjectBuilderComponent* component =
        ecs::try_get<engine::ObjectBuilderComponent>(registry, *entity);
    if (!component || moduleIndex >= component->runtimes.size()) return nullptr;
    return &component->runtimes[moduleIndex];
}

void clearBuilderMove(engine::ObjectOrderQueueComponent* queue,
                      size_t instance) {
    if (!queue || queue->orders.empty() ||
        !isBuilderMove(queue->orders.front(), instance)) return;
    queue->orders.erase(queue->orders.begin());
    ++queue->revision;
}

void queueBuilderMove(engine::ObjectOrderQueueComponent& queue,
                      engine::ObjectBuilderRuntime& runtime, size_t instance,
                      engine::PlayerId owner, engine::ObjectId target,
                      const engine::LogicFixedVec3& position,
                      uint64_t confirmedTick) {
    if (!queue.orders.empty()) {
        if (isBuilderMove(queue.orders.front(), instance) &&
            queue.orders.front().targetObject == target) return;
        if (isBuilderMove(queue.orders.front(), instance)) {
            queue.orders.erase(queue.orders.begin());
        } else if (queue.orders.front().kind !=
                   engine::ObjectOrderKind::Build) {
            return;
        }
    }
    queue.orders.insert(queue.orders.begin(), {
        .kind = engine::ObjectOrderKind::Move,
        .source = engine::ObjectOrderSource::System,
        .contextPlayer = owner,
        .issuedTick = confirmedTick,
        .sourceSequence = runtime.nextCommandSequence,
        .targetObject = target,
        .targetX = position.x,
        .targetY = position.y,
        .targetZ = position.z,
        .hasTargetPosition = true,
        .systemPurpose = engine::ObjectOrderSystemPurpose::Builder,
        .systemPurposeInstance = static_cast<uint32_t>(instance),
    });
    ++queue.revision;
    ++runtime.nextCommandSequence;
    if (runtime.nextCommandSequence == 0) ++runtime.nextCommandSequence;
}

[[nodiscard]] bool appendScaffoldRemovalIntent(
    container::Vector<engine::ObjectBridgeRepairScaffoldIntent>& intents,
    engine::ObjectId bridge, engine::ObjectId tower,
    engine::ObjectId builder, uint32_t sourceSequence,
    uint64_t confirmedTick) {
    if (!bridge) return false;
    const bool alreadyQueued = std::any_of(
        intents.begin(), intents.end(),
        [bridge, builder, confirmedTick](
            const engine::ObjectBridgeRepairScaffoldIntent& intent) {
            return intent.kind ==
                    engine::ObjectBridgeRepairScaffoldIntentKind::Remove &&
                intent.bridge == bridge && intent.builder == builder &&
                intent.confirmedTick == confirmedTick;
        });
    if (alreadyQueued) return false;
    intents.push_back({
        .kind = engine::ObjectBridgeRepairScaffoldIntentKind::Remove,
        .bridge = bridge,
        .tower = tower,
        .builder = builder,
        .sourceSequence = sourceSequence,
        .confirmedTick = confirmedTick,
    });
    return true;
}

[[nodiscard]] bool releaseTaskOwnership(
    ecs::registry& registry, const engine::ObjectLifecycle& lifecycle,
    engine::ObjectId builder, const engine::ObjectBuilderTask& task,
    uint64_t confirmedTick,
    container::Vector<engine::ObjectBridgeRepairScaffoldIntent>*
        bridgeScaffoldIntents) {
    if (!task.target) return false;
    const std::optional<ecs::entity> target =
        lifecycle.entityFromIdIncludingPending(task.target);
    if (!target) return false;

    bool changed = false;
    if (task.kind == engine::ObjectBuilderTaskKind::Build) {
        engine::ObjectConstructionSiteComponent* site =
            ecs::try_get<engine::ObjectConstructionSiteComponent>(
                registry, *target);
        if (site && site->builder == builder) {
            site->builder = engine::INVALID_OBJECT_ID;
            ++site->revision;
            markConstructionPresentationDirty(registry, *target);
            changed = true;
        }
    } else if (task.kind == engine::ObjectBuilderTaskKind::Repair) {
        if (bridgeScaffoldIntents) {
            if (const engine::ObjectBridgeTowerComponent* tower =
                    ecs::try_get<engine::ObjectBridgeTowerComponent>(
                        registry, *target);
                tower && tower->bridge) {
                changed = appendScaffoldRemovalIntent(
                    *bridgeScaffoldIntents, tower->bridge, task.target,
                    builder, task.sourceSequence, confirmedTick) || changed;
            }
        }
        engine::ObjectRepairBenefactorLeaseComponent* lease =
            ecs::try_get<engine::ObjectRepairBenefactorLeaseComponent>(
                registry, *target);
        if (lease && lease->builder == builder) {
            lease->builder = engine::INVALID_OBJECT_ID;
            lease->expiresTick = confirmedTick;
            ++lease->revision;
            changed = true;
        }
    }
    return changed;
}

void finishTask(ecs::registry& registry,
                const engine::ObjectLifecycle& lifecycle,
                engine::ObjectId builder,
                engine::ObjectBuilderRuntime& runtime,
                engine::ObjectOrderQueueComponent* queue, size_t instance,
                uint64_t confirmedTick,
                container::Vector<
                    engine::ObjectBridgeRepairScaffoldIntent>*
                    bridgeScaffoldIntents = nullptr) {
    static_cast<void>(releaseTaskOwnership(
        registry, lifecycle, builder, runtime.current, confirmedTick,
        bridgeScaffoldIntents));
    clearBuilderMove(queue, instance);
    clearPendingTask(runtime, runtime.current.kind);
    runtime.previous = {};
    runtime.current = {};
    runtime.suspendedByDisable = false;
    projectMostRecentTask(runtime, confirmedTick);
    ++runtime.revision;
}

} // namespace

namespace engine {

ObjectBuilderApproachResult objectBuilderApproach(
    const LogicFixedVec3& current,
    const LogicFixedVec3& constructionCenter,
    math::q32_32 builderRadius,
    math::q32_32 productRadius,
    math::q32_32 closeEnough) noexcept {
    using Fixed = math::q32_32;
    const Fixed minimumDistance = Fixed::max(
        Fixed{int32_t{1}},
        Fixed::max(Fixed{}, builderRadius) +
            Fixed::max(Fixed{}, productRadius));
    const Fixed approachPadding = Fixed::max(
        Fixed{int32_t{1}}, Fixed::max(Fixed{}, closeEnough));
    const Fixed maximumDistance =
        minimumDistance + approachPadding * Fixed{int32_t{2}} +
        Fixed{int32_t{3}};

    const Fixed deltaX = current.x - constructionCenter.x;
    const Fixed deltaY = current.y - constructionCenter.y;
    const Fixed distanceSquared = deltaX * deltaX + deltaY * deltaY;
    const bool arrived =
        distanceSquared >= minimumDistance * minimumDistance &&
        distanceSquared <= maximumDistance * maximumDistance;

    Fixed directionX{int32_t{1}};
    Fixed directionY{};
    const Fixed distance = Fixed::sqrt(distanceSquared);
    if (distance > Fixed{}) {
        directionX = deltaX / distance;
        directionY = deltaY / distance;
    }
    const Fixed approachDistance = minimumDistance + approachPadding;
    return {
        .target = {
            constructionCenter.x + directionX * approachDistance,
            constructionCenter.y + directionY * approachDistance,
            constructionCenter.z,
        },
        .minimumDistance = minimumDistance,
        .maximumDistance = maximumDistance,
        .arrived = arrived,
    };
}

void ObjectBuilderSystem::initializeObject(ecs::registry& registry,
                                           ecs::entity entity,
                                           uint64_t confirmedTick) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype || !type->archetype->builderPlan) return;
    ObjectBuilderComponent component;
    component.plan = type->archetype->builderPlan;
    component.runtimes.resize(component.plan->rules.size());
    for (ObjectBuilderRuntime& runtime : component.runtimes) {
        runtime.idleSinceTick = confirmedTick;
        runtime.nextBoredScanTick = 0;
        if (const ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(registry, entity))
            runtime.observedExternalOrderRevision = queue->externalRevision;
    }
    if (ObjectBuilderComponent* existing =
            ecs::try_get<ObjectBuilderComponent>(registry, entity))
        *existing = std::move(component);
    else
        ecs::emplace<ObjectBuilderComponent>(registry, entity,
                                             std::move(component));
}

bool ObjectBuilderSystem::beginConstruction(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, ObjectId site,
    ObjectId builder, uint32_t requiredFrames, bool rebuild,
    uint64_t confirmedTick) const {
    const std::optional<ecs::entity> siteEntity = lifecycle.entityFromId(site);
    const std::optional<ecs::entity> builderEntity =
        lifecycle.entityFromId(builder);
    if (!siteEntity || !builderEntity ||
        !ecs::try_get<ObjectBuilderComponent>(registry, *builderEntity))
        return false;
    ObjectConstructionSiteComponent component{
        .builder = builder,
        .requiredFrames = std::max(1u, requiredFrames),
        .completedFrames = 0,
        .lastProgressTick = confirmedTick,
        .revision = 1,
        .rebuild = rebuild,
    };
    if (ObjectConstructionSiteComponent* existing =
            ecs::try_get<ObjectConstructionSiteComponent>(registry, *siteEntity))
        *existing = component;
    else
        ecs::emplace<ObjectConstructionSiteComponent>(registry, *siteEntity,
                                                       component);
    markConstructionPresentationDirty(registry, *siteEntity);
    return true;
}

bool ObjectBuilderSystem::requestRepair(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, ObjectId builder,
    ObjectId target, uint64_t confirmedTick, uint32_t sourceSequence) const {
    const std::optional<ecs::entity> builderEntity = lifecycle.entityFromId(builder);
    const std::optional<ecs::entity> targetEntity = lifecycle.entityFromId(target);
    if (!builderEntity || !targetEntity) return false;
    ObjectBuilderComponent* component =
        ecs::try_get<ObjectBuilderComponent>(registry, *builderEntity);
    const OwnerComponent* builderOwner =
        ecs::try_get<OwnerComponent>(registry, *builderEntity);
    const OwnerComponent* targetOwner =
        ecs::try_get<OwnerComponent>(registry, *targetEntity);
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, *targetEntity);
    const ObjectStatusComponent* targetStatus =
        ecs::try_get<ObjectStatusComponent>(registry, *targetEntity);
    if (!component || component->runtimes.empty() || !builderOwner ||
        !targetOwner || !health ||
        health->effectivelyDead ||
        (targetStatus && targetStatus->hasAny(game::objectStatusBit(
             game::ObjectStatusFlag::UnderConstruction))) ||
        health->currentFixed >= health->maximumFixed) return false;
    ObjectBuilderRuntime& runtime = component->runtimes.front();
    seedTaskSlotsFromCompatibilityState(runtime);
    const ObjectBuilderTask previousRepair =
        taskFromSlot(runtime, ObjectBuilderTaskKind::Repair);
    if (validBuilderTask(previousRepair)) {
        if (previousRepair.target == target) return false;
        const std::optional<ecs::entity> previousTarget =
            lifecycle.entityFromIdIncludingPending(previousRepair.target);
        const ObjectBridgeTowerComponent* previousTower = previousTarget
            ? ecs::try_get<ObjectBridgeTowerComponent>(registry,
                                                        *previousTarget)
            : nullptr;
        const ObjectBridgeTowerComponent* nextTower =
            ecs::try_get<ObjectBridgeTowerComponent>(registry, *targetEntity);
        // RefCode treats another tower on the same bridge as the same repair
        // task instead of reversing and rebuilding one scaffold generation.
        if (previousTower && nextTower && previousTower->bridge &&
            previousTower->bridge == nextTower->bridge) {
            return false;
        }
        if (previousTarget) {
            ObjectRepairBenefactorLeaseComponent* lease =
                ecs::try_get<ObjectRepairBenefactorLeaseComponent>(
                    registry, *previousTarget);
            if (lease && lease->builder == builder) {
                lease->builder = INVALID_OBJECT_ID;
                lease->expiresTick = confirmedTick;
                ++lease->revision;
            }
        }
        if (previousTower && previousTower->bridge) {
            runtime.pendingScaffoldRemovalBridge = previousTower->bridge;
            runtime.pendingScaffoldRemovalTower = previousRepair.target;
            runtime.pendingScaffoldRemovalSequence =
                previousRepair.sourceSequence;
        }
    }
    if (ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, *builderEntity)) {
        while (!queue->orders.empty() &&
               queue->orders.front().source == ObjectOrderSource::System) {
            queue->orders.erase(queue->orders.begin());
            ++queue->revision;
        }
        runtime.observedExternalOrderRevision = queue->externalRevision;
    }
    runtime.previous = validBuilderTask(previousRepair)
        ? previousRepair : runtime.current;
    ObjectBuilderTask repairTask{
        .kind = ObjectBuilderTaskKind::Repair,
        .target = target,
        .issuedTick = confirmedTick,
        .sourceSequence = sourceSequence,
    };
    storePendingTask(runtime, repairTask);
    projectMostRecentTask(runtime, confirmedTick);
    enterWorkerDozerMode(registry, *builderEntity);
    runtime.suspendedByDisable = false;
    ++runtime.revision;
    return true;
}

bool ObjectBuilderSystem::canRepair(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ObjectId builder, ObjectId target) const {
    const std::optional<ecs::entity> builderEntity =
        lifecycle.entityFromId(builder);
    const std::optional<ecs::entity> targetEntity =
        lifecycle.entityFromId(target);
    if (!builderEntity || !targetEntity || builder == target) return false;
    const ObjectBuilderComponent* component =
        ecs::try_get<ObjectBuilderComponent>(registry, *builderEntity);
    const OwnerComponent* builderOwner =
        ecs::try_get<OwnerComponent>(registry, *builderEntity);
    const OwnerComponent* targetOwner =
        ecs::try_get<OwnerComponent>(registry, *targetEntity);
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, *targetEntity);
    const ObjectHealthComponent* builderHealth =
        ecs::try_get<ObjectHealthComponent>(registry, *builderEntity);
    const ObjectStatusComponent* builderStatus =
        ecs::try_get<ObjectStatusComponent>(registry, *builderEntity);
    const ObjectStatusComponent* targetStatus =
        ecs::try_get<ObjectStatusComponent>(registry, *targetEntity);
    const ObjectKindOfComponent* builderKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, *builderEntity);
    const ObjectKindOfComponent* targetKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, *targetEntity);
    const auto underConstruction = [](const ObjectStatusComponent* status) {
        return status && status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::UnderConstruction));
    };
    return component && !component->runtimes.empty() && builderOwner &&
        targetOwner &&
        players.relationship(builderOwner->player, targetOwner->player) !=
            PlayerRelationship::Enemies &&
        hasKind(builderKinds, game::ObjectKindOf::Dozer) &&
        hasKind(targetKinds, game::ObjectKindOf::Structure) &&
        !hasKind(targetKinds, game::ObjectKindOf::Bridge) &&
        !hasKind(targetKinds, game::ObjectKindOf::BridgeTower) &&
        !hasKind(targetKinds, game::ObjectKindOf::RebuildHole) &&
        !underConstruction(builderStatus) && !underConstruction(targetStatus) &&
        !ecs::try_get<ObjectContainedByComponent>(registry, *builderEntity) &&
        builderHealth && !builderHealth->effectivelyDead &&
        health && !health->effectivelyDead &&
        health->currentFixed < health->maximumFixed;
}

bool ObjectBuilderSystem::requestRepair(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ObjectId builder, ObjectId target,
    uint64_t confirmedTick, uint32_t sourceSequence,
    bool replaceExternalOrders, bool requireClearTarget) const {
    if (!canRepair(registry, lifecycle, players, builder, target)) return false;
    const std::optional<ecs::entity> builderEntity =
        lifecycle.entityFromId(builder);
    const std::optional<ecs::entity> targetEntity =
        lifecycle.entityFromId(target);
    if (!builderEntity || !targetEntity) return false;

    const ObjectRepairBenefactorLeaseComponent* lease =
        ecs::try_get<ObjectRepairBenefactorLeaseComponent>(registry,
                                                            *targetEntity);
    if (lease && lease->builder && lease->builder != builder &&
        lease->expiresTick >= confirmedTick) {
        return false;
    }
    ObjectBuilderComponent* component =
        ecs::try_get<ObjectBuilderComponent>(registry, *builderEntity);
    if (!component || component->runtimes.empty()) return false;
    ObjectBuilderRuntime& runtime = component->runtimes.front();
    if (runtime.current.kind == ObjectBuilderTaskKind::Repair &&
        runtime.current.target == target) {
        return false;
    }

    if (replaceExternalOrders) {
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, *builderEntity);
        if (!queue) {
            queue = &ecs::emplace<ObjectOrderQueueComponent>(registry,
                                                              *builderEntity);
        }
        if (!queue->orders.empty()) {
            queue->orders.clear();
            ++queue->revision;
        }
        ++queue->externalRevision;
        if (queue->externalRevision == 0) ++queue->externalRevision;
    }
    // Reuse the one task/lifecycle owner after the external interruption has
    // been recorded; requestRepair synchronizes observedExternalOrderRevision.
    const bool accepted = requestRepair(registry, lifecycle, builder, target,
                                        confirmedTick, sourceSequence);
    if (accepted) {
        if (const std::optional<size_t> slot = builderTaskSlotIndex(
                ObjectBuilderTaskKind::Repair)) {
            runtime.taskSlots[*slot].requireClearRepairTarget =
                requireClearTarget;
        }
        if (runtime.current.kind == ObjectBuilderTaskKind::Repair &&
            runtime.current.target == target) {
            runtime.current.requireClearRepairTarget = requireClearTarget;
            runtime.requireClearRepairTarget = requireClearTarget;
        }
    }
    return accepted;
}

bool ObjectBuilderSystem::canResumeConstruction(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ObjectId builder, ObjectId site) const {
    const std::optional<ecs::entity> builderEntity =
        lifecycle.entityFromId(builder);
    const std::optional<ecs::entity> siteEntity =
        lifecycle.entityFromId(site);
    if (!builderEntity || !siteEntity || builder == site) return false;

    const ObjectBuilderComponent* component =
        ecs::try_get<ObjectBuilderComponent>(registry, *builderEntity);
    const OwnerComponent* builderOwner =
        ecs::try_get<OwnerComponent>(registry, *builderEntity);
    const OwnerComponent* siteOwner =
        ecs::try_get<OwnerComponent>(registry, *siteEntity);
    const ObjectHealthComponent* builderHealth =
        ecs::try_get<ObjectHealthComponent>(registry, *builderEntity);
    const ObjectHealthComponent* siteHealth =
        ecs::try_get<ObjectHealthComponent>(registry, *siteEntity);
    const ObjectStatusComponent* builderStatus =
        ecs::try_get<ObjectStatusComponent>(registry, *builderEntity);
    const ObjectStatusComponent* siteStatus =
        ecs::try_get<ObjectStatusComponent>(registry, *siteEntity);
    const ObjectKindOfComponent* builderKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, *builderEntity);
    const ObjectKindOfComponent* siteKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, *siteEntity);
    const ObjectConstructionSiteComponent* construction =
        ecs::try_get<ObjectConstructionSiteComponent>(registry, *siteEntity);
    const auto underConstruction = [](const ObjectStatusComponent* status) {
        return status && status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::UnderConstruction));
    };
    if (!component || component->runtimes.empty() || !builderOwner ||
        !siteOwner || players.relationship(
            builderOwner->player, siteOwner->player) !=
                PlayerRelationship::Allies ||
        !hasKind(builderKinds, game::ObjectKindOf::Dozer) ||
        !hasKind(siteKinds, game::ObjectKindOf::Structure) ||
        underConstruction(builderStatus) || !underConstruction(siteStatus) ||
        !construction ||
        ecs::try_get<ObjectContainedByComponent>(registry, *builderEntity) ||
        !builderHealth || builderHealth->effectivelyDead ||
        !siteHealth || siteHealth->effectivelyDead) {
        return false;
    }

    if (!construction->builder) return true;
    const std::optional<ecs::entity> activeBuilderEntity =
        lifecycle.entityFromId(construction->builder);
    const ObjectBuilderComponent* activeBuilder = activeBuilderEntity
        ? ecs::try_get<ObjectBuilderComponent>(registry,
                                               *activeBuilderEntity)
        : nullptr;
    if (!activeBuilder) return true;
    for (const ObjectBuilderRuntime& runtime : activeBuilder->runtimes) {
        if (runtime.current.kind == ObjectBuilderTaskKind::Build &&
            runtime.current.target == site) {
            // A second worker cannot add construction effectiveness while
            // the first worker still owns the live Build task.
            return false;
        }
    }
    return true;
}

bool ObjectBuilderSystem::resumeConstruction(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ObjectId builder, ObjectId site,
    uint64_t confirmedTick, uint32_t sourceSequence,
    bool replaceExternalOrders) const {
    if (!canResumeConstruction(
            registry, lifecycle, players, builder, site)) {
        return false;
    }
    const std::optional<ecs::entity> builderEntity =
        lifecycle.entityFromId(builder);
    const std::optional<ecs::entity> siteEntity =
        lifecycle.entityFromId(site);
    if (!builderEntity || !siteEntity) return false;

    ObjectConstructionSiteComponent* construction =
        ecs::try_get<ObjectConstructionSiteComponent>(registry, *siteEntity);
    if (!construction) return false;

    // The site may still name a previous builder whose current state already
    // moved on. Remove that stale Build slot before transferring the claim so
    // it cannot reappear and steal the site back on a later projection pass.
    if (construction->builder && construction->builder != builder) {
        const std::optional<ecs::entity> previousEntity =
            lifecycle.entityFromId(construction->builder);
        ObjectBuilderComponent* previous = previousEntity
            ? ecs::try_get<ObjectBuilderComponent>(registry,
                                                   *previousEntity)
            : nullptr;
        if (previous) {
            for (ObjectBuilderRuntime& runtime : previous->runtimes) {
                bool changed = false;
                if (const std::optional<size_t> slot = builderTaskSlotIndex(
                        ObjectBuilderTaskKind::Build);
                    slot && runtime.taskSlots[*slot].target == site) {
                    runtime.taskSlots[*slot] = {};
                    changed = true;
                }
                if (runtime.previous.kind == ObjectBuilderTaskKind::Build &&
                    runtime.previous.target == site) {
                    runtime.previous = {};
                    changed = true;
                }
                if (changed) {
                    projectMostRecentTask(runtime, confirmedTick);
                    ++runtime.revision;
                }
            }
        }
        construction->builder = INVALID_OBJECT_ID;
        ++construction->revision;
        markConstructionPresentationDirty(registry, *siteEntity);
    }

    if (replaceExternalOrders) {
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                     *builderEntity);
        if (!queue) {
            queue = &ecs::emplace<ObjectOrderQueueComponent>(registry,
                                                              *builderEntity);
        }
        if (!queue->orders.empty()) {
            queue->orders.clear();
            ++queue->revision;
        }
        ++queue->externalRevision;
        if (queue->externalRevision == 0) ++queue->externalRevision;
    }
    return assignConstruction(
        registry, lifecycle, builder, site, confirmedTick, sourceSequence);
}

bool ObjectBuilderSystem::assignConstruction(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, ObjectId site, uint64_t confirmedTick,
    uint32_t sourceSequence) const {
    const std::optional<ecs::entity> builderEntity = lifecycle.entityFromId(builder);
    const std::optional<ecs::entity> siteEntity = lifecycle.entityFromId(site);
    if (!builderEntity || !siteEntity) return false;
    ObjectBuilderComponent* component =
        ecs::try_get<ObjectBuilderComponent>(registry, *builderEntity);
    ObjectConstructionSiteComponent* construction =
        ecs::try_get<ObjectConstructionSiteComponent>(registry, *siteEntity);
    if (!component || component->runtimes.empty() || !construction ||
        (construction->builder && construction->builder != builder)) return false;
    ObjectBuilderRuntime& runtime = component->runtimes.front();
    seedTaskSlotsFromCompatibilityState(runtime);
    const ObjectBuilderTask previousBuild =
        taskFromSlot(runtime, ObjectBuilderTaskKind::Build);
    if (validBuilderTask(previousBuild) && previousBuild.target != site) {
        static_cast<void>(releaseTaskOwnership(
            registry, lifecycle, builder, previousBuild, confirmedTick,
            nullptr));
    }
    if (ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, *builderEntity)) {
        if (!queue->orders.empty() &&
            isBuilderMove(queue->orders.front(), 0)) {
            queue->orders.erase(queue->orders.begin());
            ++queue->revision;
        }
        if (!queue->orders.empty() &&
            queue->orders.front().kind == ObjectOrderKind::Build &&
            queue->orders.front().sourceSequence == sourceSequence) {
            queue->orders.erase(queue->orders.begin());
            ++queue->revision;
        }
        runtime.observedExternalOrderRevision = queue->externalRevision;
    }
    construction->builder = builder;
    ++construction->revision;
    markConstructionPresentationDirty(registry, *siteEntity);
    runtime.previous = validBuilderTask(previousBuild)
        ? previousBuild : runtime.current;
    const ObjectBuilderTask buildTask{
        .kind = ObjectBuilderTaskKind::Build,
        .target = site,
        .issuedTick = confirmedTick,
        .sourceSequence = sourceSequence,
    };
    storePendingTask(runtime, buildTask);
    projectMostRecentTask(runtime, confirmedTick);
    enterWorkerDozerMode(registry, *builderEntity);
    runtime.suspendedByDisable = false;
    ++runtime.revision;
    return true;
}

ObjectBuilderTask ObjectBuilderSystem::task(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, ObjectBuilderTaskKind kind, size_t moduleIndex) const {
    const ObjectBuilderRuntime* runtime = findBuilderRuntime(
        registry, lifecycle, builder, moduleIndex);
    return runtime ? taskFromSlot(*runtime, kind) : ObjectBuilderTask{};
}

ObjectBuilderTask ObjectBuilderSystem::mostRecentTask(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, size_t moduleIndex) const {
    const ObjectBuilderRuntime* runtime = findBuilderRuntime(
        registry, lifecycle, builder, moduleIndex);
    return runtime ? mostRecentPendingTask(*runtime) : ObjectBuilderTask{};
}

ObjectBuilderTask ObjectBuilderSystem::currentTask(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, size_t moduleIndex) const {
    const ObjectBuilderRuntime* runtime = findBuilderRuntime(
        registry, lifecycle, builder, moduleIndex);
    return runtime ? runtime->current : ObjectBuilderTask{};
}

bool ObjectBuilderSystem::isTaskPending(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, ObjectBuilderTaskKind kind, size_t moduleIndex) const {
    return validBuilderTask(task(registry, lifecycle, builder, kind,
                                 moduleIndex));
}

bool ObjectBuilderSystem::isAnyTaskPending(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, size_t moduleIndex) const {
    return validBuilderTask(mostRecentTask(registry, lifecycle, builder,
                                           moduleIndex));
}

bool ObjectBuilderSystem::cancelAllTasks(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, uint64_t confirmedTick,
    container::Vector<ObjectBridgeRepairScaffoldIntent>&
        outBridgeScaffoldIntents,
    std::optional<uint32_t> authoredOrder) const {
    const std::optional<ecs::entity> builderEntity =
        lifecycle.entityFromIdIncludingPending(builder);
    if (!builderEntity) return false;
    ObjectBuilderComponent* component =
        ecs::try_get<ObjectBuilderComponent>(registry, *builderEntity);
    if (!component || !component->plan) return false;

    std::optional<size_t> selectedRuntime;
    if (authoredOrder) {
        const size_t count = std::min(component->plan->rules.size(),
                                      component->runtimes.size());
        for (size_t index = 0; index < count; ++index) {
            if (component->plan->rules[index].authoredOrder ==
                *authoredOrder) {
                selectedRuntime = index;
                break;
            }
        }
        if (!selectedRuntime) return false;
    }

    bool changed = false;
    bool canceledQueuedTask = false;
    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(registry, *builderEntity);
    if (queue) {
        bool removedExternalBuild = false;
        const auto retained = std::remove_if(
            queue->orders.begin(), queue->orders.end(),
            [&removedExternalBuild, selectedRuntime](
                const ObjectOrderIntent& order) {
                const bool builderMove =
                    order.source == ObjectOrderSource::System &&
                    order.systemPurpose ==
                        ObjectOrderSystemPurpose::Builder &&
                    (!selectedRuntime ||
                     order.systemPurposeInstance == *selectedRuntime);
                const bool buildTask =
                    order.kind == ObjectOrderKind::Build &&
                    (!selectedRuntime || *selectedRuntime == 0);
                removedExternalBuild = removedExternalBuild ||
                    (buildTask && order.source != ObjectOrderSource::System);
                return builderMove || buildTask;
            });
        if (retained != queue->orders.end()) {
            queue->orders.erase(retained, queue->orders.end());
            ++queue->revision;
            if (removedExternalBuild) {
                ++queue->externalRevision;
                if (queue->externalRevision == 0) ++queue->externalRevision;
            }
            changed = true;
            canceledQueuedTask = true;
        }
    }

    // Runtime order is authored module order.  Preserve it when emitting
    // value intents so cancellation is deterministic even for templates that
    // carry more than one builder module.
    for (size_t runtimeIndex = 0;
         runtimeIndex < component->runtimes.size(); ++runtimeIndex) {
        if (selectedRuntime && runtimeIndex != *selectedRuntime) continue;
        ObjectBuilderRuntime& runtime = component->runtimes[runtimeIndex];
        seedTaskSlotsFromCompatibilityState(runtime);
        bool runtimeChanged = false;
        if (runtime.pendingScaffoldRemovalBridge) {
            runtimeChanged = appendScaffoldRemovalIntent(
                outBridgeScaffoldIntents,
                runtime.pendingScaffoldRemovalBridge,
                runtime.pendingScaffoldRemovalTower, builder,
                runtime.pendingScaffoldRemovalSequence,
                confirmedTick) || runtimeChanged;
        }
        runtimeChanged = releaseTaskOwnership(
            registry, lifecycle, builder, runtime.current, confirmedTick,
            &outBridgeScaffoldIntents) || runtimeChanged;
        runtimeChanged = releaseTaskOwnership(
            registry, lifecycle, builder, runtime.previous, confirmedTick,
            &outBridgeScaffoldIntents) || runtimeChanged;
        for (const ObjectBuilderTask& pending : runtime.taskSlots) {
            runtimeChanged = releaseTaskOwnership(
                registry, lifecycle, builder, pending, confirmedTick,
                &outBridgeScaffoldIntents) || runtimeChanged;
        }

        const bool heldRuntimeState =
            hasAuthoritativeTaskSlots(runtime) ||
            runtime.current.kind != ObjectBuilderTaskKind::None ||
            runtime.current.target ||
            runtime.previous.kind != ObjectBuilderTaskKind::None ||
            runtime.previous.target ||
            runtime.pendingScaffoldRemovalBridge ||
            runtime.pendingScaffoldRemovalTower ||
            runtime.pendingScaffoldRemovalSequence != 0 ||
            runtime.phase != ObjectBuilderPhase::Idle ||
            runtime.requireClearRepairTarget ||
            runtime.suspendedByDisable ||
            runtime.nextBoredScanTick != 0;
        const bool resetRuntime =
            heldRuntimeState || runtimeChanged || canceledQueuedTask;
        if (resetRuntime) {
            runtime.taskSlots = {};
            runtime.current = {};
            runtime.previous = {};
            runtime.pendingScaffoldRemovalBridge = INVALID_OBJECT_ID;
            runtime.pendingScaffoldRemovalTower = INVALID_OBJECT_ID;
            runtime.pendingScaffoldRemovalSequence = 0;
            runtime.phase = ObjectBuilderPhase::Idle;
            runtime.requireClearRepairTarget = false;
            runtime.suspendedByDisable = false;
            runtime.idleSinceTick = confirmedTick;
            runtime.nextBoredScanTick = 0;
        }
        if (queue)
            runtime.observedExternalOrderRevision = queue->externalRevision;
        if (resetRuntime) {
            ++runtime.revision;
            changed = true;
        }
    }

    // beginConstruction() claims the site before assignConstruction() stores
    // the active task.  Sweep these sparse ownership components as a safety
    // net so cancellation between those two calls cannot leave an old-owner
    // claim behind.  Mutations commute and each component revision advances
    // at most once, independent of registry iteration order.
    const bool sweepUnattributedClaims =
        !selectedRuntime || *selectedRuntime == 0;
    const auto constructionView =
        ecs::view<ObjectConstructionSiteComponent>(registry);
    for (const ecs::entity entity : constructionView) {
        if (!sweepUnattributedClaims) break;
        ObjectConstructionSiteComponent& site =
            constructionView.template get<ObjectConstructionSiteComponent>(
                entity);
        if (site.builder != builder) continue;
        site.builder = INVALID_OBJECT_ID;
        ++site.revision;
        markConstructionPresentationDirty(registry, entity);
        changed = true;
    }
    const auto leaseView =
        ecs::view<ObjectRepairBenefactorLeaseComponent>(registry);
    for (const ecs::entity entity : leaseView) {
        if (!sweepUnattributedClaims) break;
        ObjectRepairBenefactorLeaseComponent& lease =
            leaseView.template get<ObjectRepairBenefactorLeaseComponent>(
                entity);
        if (lease.builder != builder) continue;
        lease.builder = INVALID_OBJECT_ID;
        lease.expiresTick = confirmedTick;
        ++lease.revision;
        changed = true;
    }
    return changed;
}

void ObjectBuilderSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry* players,
    const game::terrain::MapVisibilitySnapshot* visibility,
    const GameContentSnapshot* content,
    const navigation::NavigationSystem* navigation,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectConstructionCompletionIntent>&
        outCompletedConstruction,
    container::Vector<ObjectBridgeRepairScaffoldIntent>&
        outBridgeScaffoldIntents) const {
    const auto sites = ecs::view<ObjectConstructionSiteComponent>(registry);
    for (const ecs::entity siteEntity : sites) {
        ObjectConstructionSiteComponent& site =
            sites.template get<ObjectConstructionSiteComponent>(siteEntity);
        if (site.builder && !lifecycle.entityFromId(site.builder)) {
            site.builder = INVALID_OBJECT_ID;
            ++site.revision;
            markConstructionPresentationDirty(registry, siteEntity);
        }

    }
    struct Candidate { ObjectId id; ecs::entity entity; };
    container::Vector<Candidate> builders;
    const auto view =
        ecs::view<const ObjectIdentityComponent, ObjectBuilderComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectId id = view.template get<const ObjectIdentityComponent>(entity).id;
        if (id && lifecycle.entityFromId(id)) builders.push_back({id, entity});
    }
    std::sort(builders.begin(), builders.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.id < right.id;
              });

    for (const Candidate& builder : builders) {
        ObjectBuilderComponent& component =
            ecs::get<ObjectBuilderComponent>(registry, builder.entity);
        if (!component.plan) continue;
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, builder.entity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, builder.entity);
        const TransformComponent* builderTransform =
            ecs::try_get<TransformComponent>(registry, builder.entity);
        const ObjectLocomotionComponent* builderLocomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry, builder.entity);
        if (!owner || !builderTransform || !builderLocomotion) continue;

        const ObjectMapStatusComponent* mapStatus =
            ecs::try_get<ObjectMapStatusComponent>(registry, builder.entity);
        const bool unavailableInContainer =
            ecs::try_get<ObjectContainedByComponent>(registry,
                                                      builder.entity) != nullptr ||
            (mapStatus && mapStatus->offMap) ||
            isObjectDisabledBy(registry, builder.entity,
                               ObjectDisabledReason::Held, confirmedTick);
        if (unavailableInContainer) {
            // Entering an enclosing transport is a hard Worker state change,
            // not a temporary Dozer disable. Release construction claims,
            // repair leases and system movement before any later phase can
            // retain or resume the old task after detach.
            static_cast<void>(cancelAllTasks(
                registry, lifecycle, builder.id, confirmedTick,
                outBridgeScaffoldIntents));
            continue;
        }

        const size_t count = std::min(component.plan->rules.size(),
                                      component.runtimes.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectBuilderRule& rule = component.plan->rules[index];
            ObjectBuilderRuntime& runtime = component.runtimes[index];
            seedTaskSlotsFromCompatibilityState(runtime);

            if (runtime.pendingScaffoldRemovalBridge) {
                outBridgeScaffoldIntents.push_back({
                    .kind = ObjectBridgeRepairScaffoldIntentKind::Remove,
                    .bridge = runtime.pendingScaffoldRemovalBridge,
                    .tower = runtime.pendingScaffoldRemovalTower,
                    .builder = builder.id,
                    .sourceSequence =
                        runtime.pendingScaffoldRemovalSequence,
                    .confirmedTick = confirmedTick,
                });
                runtime.pendingScaffoldRemovalBridge = INVALID_OBJECT_ID;
                runtime.pendingScaffoldRemovalTower = INVALID_OBJECT_ID;
                runtime.pendingScaffoldRemovalSequence = 0;
                ++runtime.revision;
            }

            const ObjectHealthComponent* builderHealth =
                ecs::try_get<ObjectHealthComponent>(registry,
                                                     builder.entity);
            if (builderHealth && builderHealth->effectivelyDead) {
                if (!validBuilderTask(runtime.current))
                    projectMostRecentTask(runtime, confirmedTick);
                while (validBuilderTask(runtime.current)) {
                    finishTask(registry, lifecycle, builder.id, runtime, queue,
                               index, confirmedTick,
                               &outBridgeScaffoldIntents);
                }
                runtime.previous = {};
                runtime.suspendedByDisable = false;
                continue;
            }

            const bool disabled = isObjectDisabled(
                registry, builder.entity, confirmedTick);
            if (disabled) {
                if (runtime.current.kind != ObjectBuilderTaskKind::None &&
                    !runtime.suspendedByDisable) {
                    // Match DozerAIUpdate::cancelTask on the disabled edge:
                    // release ownership immediately so another worker may
                    // act, but preserve one value task for re-enable.
                    const ObjectBuilderTask suspended = runtime.current;
                    static_cast<void>(releaseTaskOwnership(
                        registry, lifecycle, builder.id, suspended,
                        confirmedTick, &outBridgeScaffoldIntents));
                    clearBuilderMove(queue, index);
                    runtime.previous = suspended;
                    runtime.current = {};
                    runtime.phase = ObjectBuilderPhase::Idle;
                    runtime.requireClearRepairTarget = false;
                    runtime.suspendedByDisable = true;
                    ++runtime.revision;
                }
                continue;
            }

            if (runtime.suspendedByDisable) {
                const ObjectBuilderTask resume = mostRecentPendingTask(runtime);
                runtime.previous = {};
                runtime.suspendedByDisable = false;
                const std::optional<ecs::entity> target =
                    lifecycle.entityFromId(resume.target);
                bool admitted = target.has_value();
                if (admitted && resume.kind == ObjectBuilderTaskKind::Build) {
                    ObjectConstructionSiteComponent* site =
                        ecs::try_get<ObjectConstructionSiteComponent>(
                            registry, *target);
                    admitted = site &&
                        (!site->builder || site->builder == builder.id);
                    if (admitted) {
                        site->builder = builder.id;
                        ++site->revision;
                        markConstructionPresentationDirty(registry, *target);
                    }
                } else if (admitted &&
                           resume.kind == ObjectBuilderTaskKind::Repair) {
                    ObjectRepairBenefactorLeaseComponent* lease =
                        ecs::try_get<ObjectRepairBenefactorLeaseComponent>(
                            registry, *target);
                    admitted = !lease || !lease->builder ||
                        lease->builder == builder.id ||
                        lease->expiresTick < confirmedTick;
                }
                if (admitted) {
                    runtime.current = resume;
                    runtime.phase = ObjectBuilderPhase::Approaching;
                    runtime.requireClearRepairTarget =
                        resume.requireClearRepairTarget;
                } else {
                    clearPendingTask(runtime, resume.kind);
                    projectMostRecentTask(runtime, confirmedTick);
                }
                ++runtime.revision;
            }

            if (queue && runtime.observedExternalOrderRevision !=
                    queue->externalRevision) {
                runtime.observedExternalOrderRevision = queue->externalRevision;
                const bool replacement =
                    queue->replacementExternalRevision ==
                    queue->externalRevision;
                const bool retainsCurrentTask = !replacement &&
                    !queue->orders.empty() &&
                    (queue->orders.front().kind ==
                         ObjectOrderKind::Build ||
                     isBuilderMove(queue->orders.front(), index));
                if (runtime.current.kind != ObjectBuilderTaskKind::None &&
                    !retainsCurrentTask) {
                    finishTask(registry, lifecycle, builder.id, runtime, queue,
                               index, confirmedTick,
                               &outBridgeScaffoldIntents);
                }
            }

            if (runtime.current.kind == ObjectBuilderTaskKind::None && queue &&
                !queue->orders.empty() &&
                queue->orders.front().kind == ObjectOrderKind::Build &&
                queue->orders.front().targetObject) {
                const ObjectOrderIntent order = queue->orders.front();
                queue->orders.erase(queue->orders.begin());
                ++queue->revision;
                const ObjectBuilderTask buildTask{
                    .kind = ObjectBuilderTaskKind::Build,
                    .target = order.targetObject,
                    .issuedTick = order.issuedTick,
                    .sourceSequence = order.sourceSequence,
                };
                storePendingTask(runtime, buildTask);
                projectMostRecentTask(runtime, confirmedTick);
                ++runtime.revision;
            }

            if (runtime.current.kind == ObjectBuilderTaskKind::None &&
                runtime.nextBoredScanTick == 0) {
                runtime.nextBoredScanTick = saturatingAdd(
                    runtime.idleSinceTick,
                    millisecondsToTicks(rule.boredTimeMilliseconds,
                                        rules.logicFramesPerSecond));
            }
            if (runtime.current.kind == ObjectBuilderTaskKind::None &&
                confirmedTick >= runtime.nextBoredScanTick) {
                runtime.nextBoredScanTick = saturatingAdd(
                    confirmedTick,
                    std::max<uint64_t>(1u, millisecondsToTicks(
                        rule.boredTimeMilliseconds,
                        rules.logicFramesPerSecond)));
                math::q32_32 boredRange = rule.boredRange;
                if (players && owner) {
                    const PlayerState* controllingPlayer =
                        players->get(owner->player);
                    if (controllingPlayer && controllingPlayer->controller ==
                            PlayerControllerKind::Ai) {
                        // RefCode DozerAIUpdate::getBoredRange expands only
                        // computer-controlled workers. Human builders retain
                        // the authored module range.
                        boredRange *= rules.ai.aiDozerBoredRadiusModifier;
                    }
                }
                const math::q32_32 rangeSquared = boredRange * boredRange;
                ObjectId best = INVALID_OBJECT_ID;
                math::q32_32 bestDistance = math::q32_32::from_raw(
                    std::numeric_limits<int64_t>::max());
                const auto targetView = ecs::view<
                    const ObjectIdentityComponent, const OwnerComponent,
                    const TransformComponent, const ObjectHealthComponent>(registry);
                for (const ecs::entity targetEntity : targetView) {
                    const OwnerComponent& targetOwner =
                        targetView.template get<const OwnerComponent>(targetEntity);
                    const ObjectHealthComponent& health =
                        targetView.template get<const ObjectHealthComponent>(targetEntity);
                    if (targetOwner.player != owner->player ||
                        health.effectivelyDead ||
                        health.currentFixed >= health.maximumFixed) continue;
                    const ObjectKindOfComponent* targetKinds =
                        ecs::try_get<ObjectKindOfComponent>(
                            registry, targetEntity);
                    if (!hasKind(targetKinds, game::ObjectKindOf::Structure) &&
                        !hasKind(targetKinds,
                                 game::ObjectKindOf::BridgeTower))
                        continue;
                    const ObjectId target = targetView.template get<
                        const ObjectIdentityComponent>(targetEntity).id;
                    if (!target) continue;
                    const ObjectBridgeComponent* bridge =
                        ecs::try_get<ObjectBridgeComponent>(
                            registry, targetEntity);
                    const ObjectBridgeTowerComponent* bridgeTower =
                        ecs::try_get<ObjectBridgeTowerComponent>(
                            registry, targetEntity);
                    if (bridge) {
                        bool hasRepairTower = false;
                        const auto towers = ecs::view<
                            const ObjectBridgeTowerComponent,
                            const ObjectHealthComponent>(registry);
                        for (const ecs::entity towerEntity : towers) {
                            const ObjectBridgeTowerComponent& tower =
                                towers.template get<
                                    const ObjectBridgeTowerComponent>(
                                    towerEntity);
                            const ObjectHealthComponent& towerHealth =
                                towers.template get<
                                    const ObjectHealthComponent>(towerEntity);
                            if (tower.bridge == target &&
                                !towerHealth.effectivelyDead &&
                                towerHealth.currentFixed <
                                    towerHealth.maximumFixed) {
                                hasRepairTower = true;
                                break;
                            }
                        }
                        if (hasRepairTower) continue;
                    }
                    const ObjectStatusComponent* targetStatus =
                        ecs::try_get<ObjectStatusComponent>(registry,
                                                            targetEntity);
                    if (targetStatus && targetStatus->hasAny(
                            game::objectStatusBit(
                                game::ObjectStatusFlag::UnderConstruction)))
                        continue;
                    const TransformComponent& candidateTransform =
                        targetView.template get<const TransformComponent>(
                            targetEntity);
                    const math::q32_32 distance = distanceSquared(
                        readAuthoritativeObjectPosition(
                            registry, builder.entity, *builderTransform),
                        readAuthoritativeObjectPosition(
                            registry, targetEntity, candidateTransform));
                    if (boredRange > math::q32_32{} &&
                        distance > rangeSquared) continue;
                    if ((bridge || bridgeTower) &&
                        !builderTargetSharesReachableZone(
                            registry, navigation, builder.entity,
                            targetEntity)) {
                        continue;
                    }
                    if (distance < bestDistance ||
                        (distance == bestDistance && target < best)) {
                        best = target;
                        bestDistance = distance;
                    }
                }
                if (best) static_cast<void>(requestRepair(
                    registry, lifecycle, builder.id, best, confirmedTick));
            }

            if (runtime.current.kind == ObjectBuilderTaskKind::None) continue;
            if (runtime.current.kind == ObjectBuilderTaskKind::Repair &&
                players) {
                const std::optional<ecs::entity> repairTarget =
                    lifecycle.entityFromId(runtime.current.target);
                const ObjectKindOfComponent* repairKinds = repairTarget
                    ? ecs::try_get<ObjectKindOfComponent>(registry,
                                                          *repairTarget)
                    : nullptr;
                // Manual/player ingress rejects bridges, but the dormant
                // scaffold transaction still has direct system/AI callers.
                // Preserve that legacy owner instead of cancelling it through
                // the stricter player ActionManager predicate.
                const bool legacyBridgeRepair =
                    hasKind(repairKinds, game::ObjectKindOf::Bridge) ||
                    hasKind(repairKinds, game::ObjectKindOf::BridgeTower) ||
                    (repairTarget &&
                     (ecs::try_get<ObjectBridgeComponent>(registry,
                                                          *repairTarget) ||
                      ecs::try_get<ObjectBridgeTowerComponent>(registry,
                                                               *repairTarget)));
                if (!legacyBridgeRepair &&
                    !canRepair(registry, lifecycle, *players, builder.id,
                               runtime.current.target)) {
                    finishTask(registry, lifecycle, builder.id, runtime, queue,
                               index, confirmedTick,
                               &outBridgeScaffoldIntents);
                    continue;
                }
                if (!legacyBridgeRepair &&
                    runtime.requireClearRepairTarget) {
                    const TransformComponent* targetTransform = repairTarget
                        ? ecs::try_get<TransformComponent>(registry,
                                                           *repairTarget)
                        : nullptr;
                    const ObjectGeometryComponent* targetGeometry = repairTarget
                        ? ecs::try_get<ObjectGeometryComponent>(registry,
                                                                *repairTarget)
                        : nullptr;
                    bool clearForPlayer = targetTransform != nullptr;
                    const LogicFixedVec3 targetPosition = targetTransform
                        ? readAuthoritativeObjectPosition(
                              registry, *repairTarget, *targetTransform)
                        : LogicFixedVec3{};
                    const math::q32_32 targetRadius = targetGeometry
                        ? math::q32_32::max(
                              math::q32_32{},
                              targetGeometry->boundingCircleRadiusFixed)
                        : math::q32_32{};
                    if (clearForPlayer && visibility &&
                        visibility->renderingActive) {
                        clearForPlayer = visibility->footprintHasClearCellRaw(
                            owner->player, targetPosition.x.raw(),
                            targetPosition.y.raw(), targetRadius.raw());
                        if (!clearForPlayer && players) {
                            for (const PlayerId ally : players->activePlayerIds()) {
                                if (ally == owner->player ||
                                    players->relationship(
                                        owner->player, ally) !=
                                        PlayerRelationship::Allies) {
                                    continue;
                                }
                                if (visibility->footprintHasClearCellRaw(
                                        ally, targetPosition.x.raw(),
                                        targetPosition.y.raw(),
                                        targetRadius.raw())) {
                                    clearForPlayer = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (visibility && visibility->renderingActive &&
                        !clearForPlayer) {
                        finishTask(registry, lifecycle, builder.id, runtime,
                                   queue, index, confirmedTick,
                                   &outBridgeScaffoldIntents);
                        continue;
                    }
                }
            }
            const std::optional<ecs::entity> targetEntity =
                lifecycle.entityFromId(runtime.current.target);
            if (!targetEntity) {
                finishTask(registry, lifecycle, builder.id, runtime, queue,
                           index, confirmedTick,
                           &outBridgeScaffoldIntents);
                continue;
            }
            const TransformComponent* targetTransform =
                ecs::try_get<TransformComponent>(registry, *targetEntity);
            const ObjectGeometryComponent* builderGeometry =
                ecs::try_get<ObjectGeometryComponent>(registry, builder.entity);
            const ObjectGeometryComponent* targetGeometry =
                ecs::try_get<ObjectGeometryComponent>(registry, *targetEntity);
            if (!targetTransform) {
                finishTask(registry, lifecycle, builder.id, runtime, queue,
                           index, confirmedTick,
                           &outBridgeScaffoldIntents);
                continue;
            }
            const LogicFixedVec3 builderPosition =
                readAuthoritativeObjectPosition(
                    registry, builder.entity, *builderTransform);
            const LogicFixedVec3 targetPosition =
                readAuthoritativeObjectPosition(
                    registry, *targetEntity, *targetTransform);
            const math::q32_32 builderRadius = builderGeometry
                ? math::q32_32::max(
                      math::q32_32{},
                      builderGeometry->boundingCircleRadiusFixed)
                : math::q32_32{int32_t{5}};
            const math::q32_32 targetRadius = targetGeometry
                ? math::q32_32::max(
                      math::q32_32{},
                      targetGeometry->boundingCircleRadiusFixed)
                : math::q32_32{int32_t{5}};
            const ObjectBuilderApproachResult approach =
                objectBuilderApproach(
                    builderPosition, targetPosition,
                    builderRadius, targetRadius,
                    builderLocomotion->closeEnough);
            if (!approach.arrived) {
                runtime.phase = ObjectBuilderPhase::Approaching;
                if (queue) queueBuilderMove(*queue, runtime, index, owner->player,
                                            runtime.current.target,
                                            approach.target, confirmedTick);
                continue;
            }
            clearBuilderMove(queue, index);

            if (runtime.current.kind == ObjectBuilderTaskKind::Build) {
                ObjectConstructionSiteComponent* site =
                    ecs::try_get<ObjectConstructionSiteComponent>(registry,
                                                                  *targetEntity);
                if (!site || (site->builder && site->builder != builder.id)) {
                    finishTask(registry, lifecycle, builder.id, runtime, queue,
                               index, confirmedTick,
                               &outBridgeScaffoldIntents);
                    continue;
                }
                if (site->builder != builder.id) {
                    site->builder = builder.id;
                    ++site->revision;
                    markConstructionPresentationDirty(registry,
                                                       *targetEntity);
                }
                runtime.phase = ObjectBuilderPhase::Building;
                if (site->lastProgressTick != confirmedTick) {
                    site->lastProgressTick = confirmedTick;
                    const bool enteredPartialConstruction =
                        site->completedFrames == 0 &&
                        site->completedFrames < site->requiredFrames;
                    if (site->completedFrames < site->requiredFrames)
                        ++site->completedFrames;
                    ++site->revision;
                    markObjectDirty(
                        registry, *targetEntity,
                        objectDirtyBit(ObjectDirtyDomain::RenderExtraction) |
                            (enteredPartialConstruction
                                 ? objectDirtyBit(
                                       ObjectDirtyDomain::ModelCondition)
                                 : 0u));
                    ObjectHealthComponent* health =
                        ecs::try_get<ObjectHealthComponent>(registry, *targetEntity);
                    if (health && site->requiredFrames != 0) {
                        const math::q32_32 amount = health->maximumFixed /
                            math::q32_32{static_cast<int32_t>(
                                std::min<uint32_t>(site->requiredFrames,
                                    static_cast<uint32_t>(
                                        std::numeric_limits<int32_t>::max())))};
                        outDamage.push_back({
                            .target = runtime.current.target,
                            .source = builder.id,
                            .amount = amount,
                            .damageType = game::DamageType::HEALING,
                            .deathType = game::DeathType::NORMAL,
                            .confirmedTick = confirmedTick,
                        });
                    }
                }
                if (site->completedFrames >= site->requiredFrames) {
                    outCompletedConstruction.push_back({
                        .object = runtime.current.target,
                        .builder = builder.id,
                    });
                    finishTask(registry, lifecycle, builder.id, runtime, queue,
                               index, confirmedTick,
                               &outBridgeScaffoldIntents);
                }
            } else if (runtime.current.kind == ObjectBuilderTaskKind::Repair) {
                ObjectHealthComponent* health =
                    ecs::try_get<ObjectHealthComponent>(registry, *targetEntity);
                if (!health || health->effectivelyDead ||
                    health->currentFixed >= health->maximumFixed) {
                    finishTask(registry, lifecycle, builder.id, runtime, queue,
                               index, confirmedTick,
                               &outBridgeScaffoldIntents);
                    continue;
                }
                if (const ObjectBridgeTowerComponent* tower =
                        ecs::try_get<ObjectBridgeTowerComponent>(
                            registry, *targetEntity);
                    tower && tower->bridge) {
                    const std::optional<ecs::entity> bridgeEntity =
                        lifecycle.entityFromId(tower->bridge);
                    const ObjectBridgeComponent* bridge = bridgeEntity
                        ? ecs::try_get<ObjectBridgeComponent>(
                              registry, *bridgeEntity)
                        : nullptr;
                    if (!bridge || !bridge->scaffoldingPresent) {
                        outBridgeScaffoldIntents.push_back({
                            .kind = ObjectBridgeRepairScaffoldIntentKind::EnsureCreated,
                            .bridge = tower->bridge,
                            .tower = runtime.current.target,
                            .builder = builder.id,
                            .sourceSequence = runtime.current.sourceSequence,
                            .confirmedTick = confirmedTick,
                        });
                        // RefCode does not heal a bridge tower until all
                        // scaffolds have been created and fully extended.
                        runtime.phase = ObjectBuilderPhase::Repairing;
                        continue;
                    }
                    bool scaffoldInMotion = false;
                    for (const ObjectId scaffoldObject :
                         bridge->scaffoldObjects) {
                        const std::optional<ecs::entity> scaffoldEntity =
                            lifecycle.entityFromId(scaffoldObject);
                        const ObjectBridgeScaffoldComponent* scaffold =
                            scaffoldEntity
                            ? ecs::try_get<ObjectBridgeScaffoldComponent>(
                                  registry, *scaffoldEntity)
                            : nullptr;
                        if (!scaffold || !scaffold->configured ||
                            scaffold->motion !=
                                ObjectBridgeScaffoldMotion::Still) {
                            scaffoldInMotion = true;
                            break;
                        }
                    }
                    if (scaffoldInMotion) {
                        runtime.phase = ObjectBuilderPhase::Repairing;
                        continue;
                    }
                }
                ObjectRepairBenefactorLeaseComponent* lease =
                    ecs::try_get<ObjectRepairBenefactorLeaseComponent>(registry,
                                                                       *targetEntity);
                if (!lease) lease = &ecs::emplace<
                    ObjectRepairBenefactorLeaseComponent>(registry, *targetEntity);
                if (lease->builder && lease->builder != builder.id &&
                    lease->expiresTick >= confirmedTick) {
                    finishTask(registry, lifecycle, builder.id, runtime, queue,
                               index, confirmedTick,
                               &outBridgeScaffoldIntents);
                    continue;
                }
                lease->builder = builder.id;
                lease->expiresTick = saturatingAdd(confirmedTick, 1u);
                ++lease->revision;

                // The legacy Object owns one sole-healing-benefactor lease
                // shared by dozer repair, PropagandaTower and radius
                // AutoHeal. The task lease above still arbitrates builders;
                // this common lease prevents cross-family healing stacks.
                ObjectSoleHealingBenefactorComponent* sole =
                    ecs::try_get<ObjectSoleHealingBenefactorComponent>(
                        registry, *targetEntity);
                if (!sole) {
                    sole = &ecs::emplace<
                        ObjectSoleHealingBenefactorComponent>(
                            registry, *targetEntity);
                }
                if (sole->source && sole->source != builder.id &&
                    confirmedTick <= sole->expiresTick) {
                    finishTask(registry, lifecycle, builder.id, runtime,
                               queue, index, confirmedTick,
                               &outBridgeScaffoldIntents);
                    continue;
                }
                sole->source = builder.id;
                sole->expiresTick = saturatingAdd(confirmedTick, 2u);
                runtime.phase = ObjectBuilderPhase::Repairing;
                const math::q32_32 amount =
                    (health->maximumFixed * rule.repairHealthRatioPerSecond) /
                    math::q32_32{static_cast<int32_t>(
                        std::max(1u, rules.logicFramesPerSecond))};
                if (amount > math::q32_32{}) {
                    outDamage.push_back({
                        .target = runtime.current.target,
                        .source = builder.id,
                        .amount = amount,
                        .damageType = game::DamageType::HEALING,
                        .deathType = game::DeathType::NORMAL,
                        .confirmedTick = confirmedTick,
                    });
                }
            } else {
                // RefCode declares the Fortify slot and transition, but the
                // entire source tree has no producer for it and DoAction is
                // `@todo write me`. Keep an injected slot finite without
                // inventing a fortification gameplay mechanic.
                finishTask(registry, lifecycle, builder.id, runtime, queue,
                           index, confirmedTick,
                           &outBridgeScaffoldIntents);
            }
        }
        if (content) {
            const bool builderBusy = std::any_of(
                component.runtimes.begin(), component.runtimes.end(),
                [](const ObjectBuilderRuntime& runtime) {
                    return runtime.current.kind != ObjectBuilderTaskKind::None;
                });
            ObjectCombatProfileComponent* combat =
                ecs::try_get<ObjectCombatProfileComponent>(
                    registry, builder.entity);
            const game::WeaponSetConditionMask mineClearing =
                game::weaponSetConditionBit(
                    game::WeaponSetCondition::MineClearingDetail);
            const bool mineClearingActive = combat &&
                (combat->weaponConditions & mineClearing) != 0;
            if (combat && mineClearingActive == builderBusy) {
                if (builderBusy)
                    combat->weaponConditions &= ~mineClearing;
                else
                    combat->weaponConditions |= mineClearing;
                static_cast<void>(refreshObjectWeaponSet(
                    registry, builder.entity, *content,
                    rules.logicFramesPerSecond, confirmedTick));
            }
        }
    }
    std::sort(
        outCompletedConstruction.begin(), outCompletedConstruction.end(),
        [](const ObjectConstructionCompletionIntent& left,
           const ObjectConstructionCompletionIntent& right) {
            if (left.object != right.object)
                return left.object < right.object;
            return left.builder < right.builder;
        });
    outCompletedConstruction.erase(
        std::unique(
            outCompletedConstruction.begin(),
            outCompletedConstruction.end(),
            [](const ObjectConstructionCompletionIntent& left,
               const ObjectConstructionCompletionIntent& right) {
                return left.object == right.object;
            }),
        outCompletedConstruction.end());
}

} // namespace engine
