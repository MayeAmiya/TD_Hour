#include "game/object/simulation/structure/ObjectBridgeDetail.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/data/base/ObjectSimulationRules.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>

namespace engine {

namespace {
using Fixed = math::q32_32;
} // namespace

void ObjectBridgeSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot* content) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!templateComponent || !templateComponent->archetype ||
        !templateComponent->archetype->bridgeRailPlan) {
        return;
    }
    const container::SharedPtr<const game::ObjectBridgeRailPlan> plan =
        templateComponent->archetype->bridgeRailPlan;
    const auto replaceOrEmplace = [&]<typename Component>(Component component) {
        if (Component* existing = ecs::try_get<Component>(registry, entity)) {
            *existing = std::move(component);
        } else {
            ecs::emplace<Component>(registry, entity, std::move(component));
        }
    };
    if (!plan->bridges.empty()) {
        ObjectBridgeComponent component{.plan = plan};
        component.dieOclContentByRule.resize(plan->bridges.size());
        for (size_t ruleIndex = 0; ruleIndex < plan->bridges.size();
             ++ruleIndex) {
            const game::ObjectBridgeBehaviorRule& rule =
                plan->bridges[ruleIndex];
            container::Vector<game::ObjectCreationListContentId>& resolved =
                component.dieOclContentByRule[ruleIndex];
            resolved.reserve(rule.dieOcl.size());
            for (const game::ObjectBridgeTimedResource& entry : rule.dieOcl) {
                resolved.push_back(content
                    ? content->findObjectCreationListId(entry.resource)
                    : game::INVALID_OBJECT_CREATION_LIST_CONTENT_ID);
            }
        }
        replaceOrEmplace(std::move(component));
    }
    if (!plan->scaffolds.empty()) {
        replaceOrEmplace(ObjectBridgeScaffoldComponent{.plan = plan});
    }
    if (!plan->towers.empty()) {
        replaceOrEmplace(ObjectBridgeTowerComponent{.plan = plan});
    }
    if (!plan->railroads.empty()) {
        ObjectRailroadComponent component{.plan = plan};
        component.instances.resize(plan->railroads.size());
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, entity);
        const Fixed hitchDistance = geometry
            ? Fixed::max(Fixed{}, geometry->majorRadiusFixed) *
                  Fixed{int32_t{2}}
            : Fixed{int32_t{2}};
        for (ObjectRailroadRuntime& runtime : component.instances) {
            runtime.hitchDistance = hitchDistance;
        }
        replaceOrEmplace(std::move(component));
    }
    const size_t railedTransportCount = std::max({
        plan->railedTransportContains.size(),
        plan->railedTransportDocks.size(),
        plan->railedTransportAi.size()});
    if (railedTransportCount != 0) {
        ObjectRailedTransportRuntimeComponent component{.plan = plan};
        component.instances.resize(railedTransportCount);
        const game::W3dPristineBoneCatalog* catalog = content
            ? content->pristineBoneCatalog() : nullptr;
        const RenderModelComponent* visual =
            ecs::try_get<RenderModelComponent>(registry, entity);
        const game::ThingTemplate& templateData =
            templateComponent->archetype->templateData;
        if (catalog && catalog->isLoaded() &&
            !templateData.modelConditionVisuals.empty()) {
            const size_t visualRuleIndex =
                game::selectModelConditionVisualRuleIndex(
                    templateData, visual ? visual->modelConditionFlags
                                         : game::ModelConditionMask{});
            if (visualRuleIndex < templateData.modelConditionVisuals.size()) {
                const auto dockEnd = catalog->find(
                    templateComponent->archetype->name, visualRuleIndex,
                    "DockEnd");
                const auto dockWaiting07 = catalog->find(
                    templateComponent->archetype->name, visualRuleIndex,
                    "DockWaiting07");
                for (ObjectRailedTransportRuntime& runtime :
                     component.instances) {
                    if (dockEnd) {
                        runtime.dockEndLocal = {
                            dockEnd->translation.x,
                            dockEnd->translation.y,
                            dockEnd->translation.z,
                        };
                        runtime.dockEndValid = true;
                    }
                    if (dockWaiting07) {
                        runtime.dockWaiting07Local = {
                            dockWaiting07->translation.x,
                            dockWaiting07->translation.y,
                            dockWaiting07->translation.z,
                        };
                        runtime.dockWaiting07Valid = true;
                    }
                }
            }
        }
        replaceOrEmplace(std::move(component));
    }
}

ObjectRailedTransportDockAdmission
ObjectBridgeSystem::beginRailedTransportDockAttach(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectRailedTransportDockAttachRequest& request) const {
    if (!request.container || !request.object ||
        request.containmentRuleIndex ==
            std::numeric_limits<uint32_t>::max() ||
        lifecycle.isPendingDestroy(request.container) ||
        lifecycle.isPendingDestroy(request.object)) {
        return ObjectRailedTransportDockAdmission::Rejected;
    }
    const std::optional<ecs::entity> containerEntity =
        lifecycle.entityFromId(request.container);
    const std::optional<ecs::entity> objectEntity =
        lifecycle.entityFromId(request.object);
    if (!containerEntity || !objectEntity) {
        return ObjectRailedTransportDockAdmission::Rejected;
    }
    ObjectRailedTransportRuntimeComponent* component =
        ecs::try_get<ObjectRailedTransportRuntimeComponent>(
            registry, *containerEntity);
    if (!component || !component->plan ||
        component->plan->railedTransportDocks.empty()) {
        return ObjectRailedTransportDockAdmission::NotApplicable;
    }
    const TransformComponent* containerTransform =
        ecs::try_get<TransformComponent>(registry, *containerEntity);
    TransformComponent* objectTransform =
        ecs::try_get<TransformComponent>(registry, *objectEntity);
    if (!containerTransform || !objectTransform) {
        return ObjectRailedTransportDockAdmission::Rejected;
    }

    const size_t count = std::min(
        component->instances.size(),
        component->plan->railedTransportDocks.size());
    if (count == 0) return ObjectRailedTransportDockAdmission::NotApplicable;
    const size_t index = request.dockRuleIndex ==
            std::numeric_limits<uint32_t>::max()
        ? 0u : static_cast<size_t>(request.dockRuleIndex);
    // RefCode getRailedTransportDockUpdateInterface() returns the first
    // authored interface. Additional compiled occurrences must not create
    // parallel pull lanes for one containment edge.
    if (index != 0 || index >= count) {
        return ObjectRailedTransportDockAdmission::Rejected;
    }

    // One stable object may have only one pending attach globally, before the
    // ObjectContainedBy edge exists.  An identical retry is idempotent; any
    // other host/occurrence/edge is a conflict.  Attach admission is cold, so
    // scanning the sparse ferry set avoids adding a second source of truth.
    const auto dockView = ecs::view<ObjectRailedTransportRuntimeComponent>(
        registry);
    for (const ecs::entity host : dockView) {
        const ObjectRailedTransportRuntimeComponent& hostComponent =
            dockView.template get<ObjectRailedTransportRuntimeComponent>(host);
        for (size_t current = 0;
             current < hostComponent.instances.size(); ++current) {
            const ObjectRailedTransportRuntime& pending =
                hostComponent.instances[current];
            if (pending.dockingObject != request.object) continue;
            const bool exactRetry = host == *containerEntity &&
                current == index &&
                pending.dockingContainmentRuleIndex ==
                    request.containmentRuleIndex &&
                pending.dockingDestroyWithContainer ==
                    request.destroyWithContainer &&
                pending.dockingEnclosing == request.enclosing &&
                pending.dockingFollowsContainerTransform ==
                    request.followsContainerTransform &&
                pending.dockingLogicFramesPerSecond ==
                    std::max(1u, request.logicFramesPerSecond);
            return exactRetry ? ObjectRailedTransportDockAdmission::Deferred
                              : ObjectRailedTransportDockAdmission::Rejected;
        }
    }
    ObjectRailedTransportRuntime& runtime = component->instances[index];
    const game::ObjectRailedTransportDockRule& rule =
        component->plan->railedTransportDocks[index];
    if (runtime.dockingObject || runtime.unloadingObject ||
        !runtime.pendingUnloadObjects.empty() || runtime.inTransit ||
        runtime.loadingOrUnloading || !runtime.dockOpen) {
        return ObjectRailedTransportDockAdmission::Rejected;
    }
    const LogicFixedVec3 objectPosition = readAuthoritativeObjectPosition(
        registry, *objectEntity, *objectTransform);
    const LogicFixedVec3 center = readAuthoritativeObjectPosition(
        registry, *containerEntity, *containerTransform);
    const Fixed distance = detail::fixedDistance(objectPosition, center);
    if (distance > rule.toleranceDistance) {
        return ObjectRailedTransportDockAdmission::Rejected;
    }
    const uint64_t durationTicks = std::max<uint64_t>(
        1u, detail::millisecondsToTicks(
            rule.pullInsideDurationMilliseconds,
            request.logicFramesPerSecond));
    runtime.dockingObject = request.object;
    runtime.dockingStartPosition = objectPosition;
    runtime.dockingStartRotation = readAuthoritativeObjectYaw(
        registry, *objectEntity, *objectTransform);
    runtime.dockingAwaitingCommit = false;
    runtime.dockingContainmentRuleIndex =
        request.containmentRuleIndex;
    runtime.dockingLogicFramesPerSecond =
        std::max(1u, request.logicFramesPerSecond);
    runtime.dockingDestroyWithContainer =
        request.destroyWithContainer;
    runtime.dockingEnclosing = request.enclosing;
    runtime.dockingFollowsContainerTransform =
        request.followsContainerTransform;
    runtime.pullInsideDistancePerFrame = distance /
        Fixed{static_cast<int32_t>(std::min<uint64_t>(
            durationTicks, static_cast<uint64_t>(
                std::numeric_limits<int32_t>::max())))};
    runtime.loadingOrUnloading = true;
    runtime.dockOpen = false;
    runtime.transitionEndsTick = std::numeric_limits<uint64_t>::max();
    writeAuthoritativeObjectYaw(
        registry, *objectEntity,
        math::fixed_atan2(center.y - objectPosition.y,
                          center.x - objectPosition.x));
    static_cast<void>(ObjectDisabledSystem::setUntil(
        registry, *objectEntity, ObjectDisabledReason::Held,
        std::numeric_limits<uint64_t>::max(), request.confirmedTick));
    static_cast<void>(ObjectStatusSystem::apply(
        registry, *objectEntity,
        {.setMask = game::objectStatusBit(
             game::ObjectStatusFlag::Unselectable),
         .confirmedTick = request.confirmedTick}));
    detail::projectRailedDockerMoving(registry, *objectEntity, true,
                              request.confirmedTick);
    ++runtime.revision;
    return ObjectRailedTransportDockAdmission::Deferred;
}

bool ObjectBridgeSystem::requestRailedTransportExecute(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId transport, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(transport);
    ObjectRailedTransportRuntimeComponent* component = entity
        ? ecs::try_get<ObjectRailedTransportRuntimeComponent>(registry,
                                                               *entity)
        : nullptr;
    if (!component || !component->plan) return false;
    const size_t count = std::min(
        component->instances.size(),
        component->plan->railedTransportAi.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectRailedTransportRuntime& runtime = component->instances[index];
        if (!runtime.dockOpen || runtime.inTransit ||
            runtime.loadingOrUnloading || runtime.dockingObject ||
            runtime.unloadingObject || runtime.executeRequested) {
            continue;
        }
        runtime.executeRequested = true;
        runtime.transitionEndsTick = confirmedTick;
        ++runtime.revision;
        return true;
    }
    return false;
}

bool ObjectBridgeSystem::acknowledgeCarriageSpawn(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectRailroadCarriageSpawnRequest& request,
    ObjectId spawnedCarriage, bool accepted) const {
    const std::optional<ecs::entity> locomotiveEntity =
        lifecycle.entityFromId(request.locomotive);
    if (!locomotiveEntity) return false;
    ObjectRailroadComponent* locomotiveComponent =
        ecs::try_get<ObjectRailroadComponent>(registry, *locomotiveEntity);
    if (!locomotiveComponent || !locomotiveComponent->plan ||
        request.railroadRuleIndex >= locomotiveComponent->instances.size() ||
        request.railroadRuleIndex >=
            locomotiveComponent->plan->railroads.size()) {
        return false;
    }
    ObjectRailroadRuntime& root =
        locomotiveComponent->instances[request.railroadRuleIndex];
    if (!root.pendingCarriageSpawn ||
        root.pendingSpawnSequence != request.requestSequence ||
        root.pendingCarriageTemplateIndex !=
            request.carriageTemplateIndex ||
        root.chainTail != request.puller) {
        return false;
    }
    root.pendingCarriageSpawn = false;
    root.pendingSpawnSequence = 0;
    root.pendingCarriageTemplateIndex =
        std::numeric_limits<uint32_t>::max();

    const std::optional<ecs::entity> pullerEntity =
        lifecycle.entityFromId(request.puller);
    const std::optional<ecs::entity> carriageEntity = accepted
        ? lifecycle.entityFromId(spawnedCarriage) : std::nullopt;
    if (!accepted || !pullerEntity || !carriageEntity) {
        root.carriagesInitialized = true;
        ++root.revision;
        return true;
    }
    ObjectRailroadComponent* pullerComponent =
        ecs::try_get<ObjectRailroadComponent>(registry, *pullerEntity);
    ObjectRailroadComponent* carriageComponent =
        ecs::try_get<ObjectRailroadComponent>(registry, *carriageEntity);
    if (!pullerComponent || !carriageComponent ||
        request.railroadRuleIndex >= pullerComponent->instances.size() ||
        request.railroadRuleIndex >= carriageComponent->instances.size() ||
        !carriageComponent->plan || request.railroadRuleIndex >=
            carriageComponent->plan->railroads.size() ||
        carriageComponent->plan->railroads[request.railroadRuleIndex]
            .isLocomotive) {
        root.carriagesInitialized = true;
        ++root.revision;
        return false;
    }

    ObjectRailroadRuntime& puller =
        pullerComponent->instances[request.railroadRuleIndex];
    ObjectRailroadRuntime& carriage =
        carriageComponent->instances[request.railroadRuleIndex];
    if (puller.trailer || carriage.hasEverBeenHitched) {
        root.carriagesInitialized = true;
        ++root.revision;
        return false;
    }
    carriage.waypointIds = root.waypointIds;
    carriage.trackPoints = root.trackPoints;
    carriage.trackLength = root.trackLength;
    carriage.trackDistance = puller.trackDistance - carriage.hitchDistance;
    carriage.speed = puller.speed;
    carriage.direction = puller.direction;
    carriage.locomotive = request.locomotive;
    carriage.puller = request.puller;
    carriage.chainTail = spawnedCarriage;
    carriage.trackDataLoaded = root.trackDataLoaded;
    carriage.looping = root.looping;
    carriage.hasEverBeenHitched = true;
    carriage.leadCarriage = false;
    carriage.waitingInWings = carriage.trackDistance < Fixed{};
    carriage.endOfLine = false;
    ++carriage.revision;

    puller.trailer = spawnedCarriage;
    ++puller.revision;
    root.chainTail = spawnedCarriage;
    root.nextCarriageTemplateIndex =
        request.carriageTemplateIndex + 1u;
    if (root.nextCarriageTemplateIndex >=
        locomotiveComponent->plan->railroads[request.railroadRuleIndex]
            .carriageTemplateNames.size()) {
        root.carriagesInitialized = true;
    }
    ++root.revision;
    if (ObjectProducerComponent* producer =
            ecs::try_get<ObjectProducerComponent>(registry,
                                                   *carriageEntity)) {
        producer->producer = request.locomotive;
    } else {
        ecs::emplace<ObjectProducerComponent>(
            registry, *carriageEntity,
            ObjectProducerComponent{.producer = request.locomotive});
    }
    return true;
}

void ObjectBridgeSystem::acknowledgeRailedTransportDockAttach(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId container, ObjectId object, uint32_t dockRuleIndex,
    bool accepted,
    uint64_t confirmedTick) const {
    if (!container || !object) return;
    const std::optional<ecs::entity> containerEntity =
        lifecycle.entityFromIdIncludingPending(container);
    const std::optional<ecs::entity> objectEntity =
        lifecycle.entityFromIdIncludingPending(object);
    bool matchedTransaction = false;
    if (containerEntity) {
        if (ObjectRailedTransportRuntimeComponent* component =
                ecs::try_get<ObjectRailedTransportRuntimeComponent>(
                    registry, *containerEntity)) {
            if (dockRuleIndex < component->instances.size()) {
                ObjectRailedTransportRuntime& runtime =
                    component->instances[dockRuleIndex];
                if (runtime.dockingObject == object &&
                    runtime.dockingAwaitingCommit) {
                    matchedTransaction = true;
                    const auto found = std::lower_bound(
                        runtime.containedObjects.begin(),
                        runtime.containedObjects.end(), object);
                    if (accepted) {
                        if (found == runtime.containedObjects.end() ||
                            *found != object) {
                            runtime.containedObjects.insert(found, object);
                        }
                    } else {
                        if (found != runtime.containedObjects.end() &&
                            *found == object) {
                            runtime.containedObjects.erase(found);
                        }
                        if (objectEntity) {
                            writeAuthoritativeObjectTransform(
                                registry, *objectEntity,
                                runtime.dockingStartPosition,
                                runtime.dockingStartRotation);
                        }
                    }
                    runtime.lastContainedCount =
                        runtime.containedObjects.size();
                    runtime.dockingObject = INVALID_OBJECT_ID;
                    runtime.dockingContainmentRuleIndex =
                        std::numeric_limits<uint32_t>::max();
                    runtime.dockingLogicFramesPerSecond = 30;
                    runtime.dockingStartPosition = {};
                    runtime.dockingStartRotation = {};
                    runtime.dockingAwaitingCommit = false;
                    runtime.pullInsideDistancePerFrame = {};
                    runtime.loadingOrUnloading = false;
                    runtime.dockOpen = !runtime.inTransit;
                    runtime.transitionEndsTick = confirmedTick;
                    ++runtime.revision;
                }
            }
        }
    }
    if (!matchedTransaction || !objectEntity) return;
    detail::projectRailedDockerMoving(registry, *objectEntity, false,
                              confirmedTick);
    // TransportContain::onContaining owns HELD after a successful commit and
    // the dock's UNSELECTABLE projection remains until push-out completes.
    // Only a rejected transaction releases those two ingress guards.
    if (accepted) return;
    static_cast<void>(ObjectDisabledSystem::clear(
        registry, *objectEntity, ObjectDisabledReason::Held,
        confirmedTick));
    static_cast<void>(ObjectStatusSystem::apply(
        registry, *objectEntity,
        {.clearMask = game::objectStatusBit(
             game::ObjectStatusFlag::Unselectable),
         .confirmedTick = confirmedTick}));
}

void ObjectBridgeSystem::detachObjectRelationships(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick,
    container::Vector<ObjectRailedTransportDockAttachCompletion>&
        cancelledDockAttach) const {
    if (!object) return;
    if (const std::optional<ecs::entity> railroadEntity =
            lifecycle.entityFromIdIncludingPending(object)) {
        if (ObjectRailroadComponent* railroad =
                ecs::try_get<ObjectRailroadComponent>(registry,
                                                       *railroadEntity)) {
            for (size_t index = 0; index < railroad->instances.size();
                 ++index) {
                ObjectRailroadRuntime& runtime = railroad->instances[index];
                if (const std::optional<ecs::entity> pullerEntity =
                        lifecycle.entityFromIdIncludingPending(
                            runtime.puller)) {
                    if (ObjectRailroadComponent* puller =
                            ecs::try_get<ObjectRailroadComponent>(
                                registry, *pullerEntity);
                        puller && index < puller->instances.size() &&
                        puller->instances[index].trailer == object) {
                        puller->instances[index].trailer = INVALID_OBJECT_ID;
                        ++puller->instances[index].revision;
                    }
                }
                if (const std::optional<ecs::entity> trailerEntity =
                        lifecycle.entityFromIdIncludingPending(
                            runtime.trailer)) {
                    if (ObjectRailroadComponent* trailer =
                            ecs::try_get<ObjectRailroadComponent>(
                                registry, *trailerEntity);
                        trailer && index < trailer->instances.size() &&
                        trailer->instances[index].puller == object) {
                        // Preserve the edge break as stable state. The
                        // carriage promotes itself to a coasting lead only
                        // after the original two-frame grace period.
                        trailer->instances[index].puller = object;
                        trailer->instances[index].unpulledTicks = 0;
                        ++trailer->instances[index].revision;
                    }
                }
            }
        }
    }
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<ObjectIdentityComponent,
                                ObjectRailedTransportRuntimeComponent>(
        registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectId host =
            view.template get<ObjectIdentityComponent>(entity).id;
        if (host && lifecycle.entityFromIdIncludingPending(host)) {
            candidates.push_back({host, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    const auto clearProjection = [&](ObjectId target) {
        const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(target);
        if (!entity) return;
        static_cast<void>(ObjectDisabledSystem::clear(
            registry, *entity, ObjectDisabledReason::Held,
            confirmedTick));
        static_cast<void>(ObjectStatusSystem::apply(
            registry, *entity,
            {.clearMask = game::objectStatusBit(
                 game::ObjectStatusFlag::Unselectable),
             .confirmedTick = confirmedTick}));
        detail::projectRailedDockerMoving(registry, *entity, false, confirmedTick);
    };
    for (const Candidate& candidate : candidates) {
        ObjectRailedTransportRuntimeComponent& component =
            ecs::get<ObjectRailedTransportRuntimeComponent>(
                registry, candidate.entity);
        for (size_t index = 0; index < component.instances.size(); ++index) {
            ObjectRailedTransportRuntime& runtime =
                component.instances[index];
            const bool hostDestroyed = candidate.object == object;
            bool changed = false;
            if (runtime.dockingObject &&
                (hostDestroyed || runtime.dockingObject == object)) {
                cancelledDockAttach.push_back({
                    .request = {
                        .container = candidate.object,
                        .object = runtime.dockingObject,
                        .dockRuleIndex = static_cast<uint32_t>(index),
                        .containmentRuleIndex =
                            runtime.dockingContainmentRuleIndex,
                        .destroyWithContainer =
                            runtime.dockingDestroyWithContainer,
                        .enclosing = runtime.dockingEnclosing,
                        .followsContainerTransform =
                            runtime.dockingFollowsContainerTransform,
                        .logicFramesPerSecond =
                            runtime.dockingLogicFramesPerSecond,
                        .confirmedTick = confirmedTick,
                    },
                    .accepted = false,
                });
                if (hostDestroyed && runtime.dockingObject != object) {
                    if (const std::optional<ecs::entity> dockerEntity =
                            lifecycle.entityFromIdIncludingPending(
                                runtime.dockingObject)) {
                        writeAuthoritativeObjectTransform(
                            registry, *dockerEntity,
                            runtime.dockingStartPosition,
                            runtime.dockingStartRotation);
                    }
                }
                clearProjection(runtime.dockingObject);
                runtime.dockingObject = INVALID_OBJECT_ID;
                runtime.dockingContainmentRuleIndex =
                    std::numeric_limits<uint32_t>::max();
                runtime.dockingLogicFramesPerSecond = 30;
                runtime.dockingStartPosition = {};
                runtime.dockingStartRotation = {};
                runtime.dockingAwaitingCommit = false;
                runtime.pullInsideDistancePerFrame = {};
                changed = true;
            }
            if (runtime.unloadingObject &&
                (hostDestroyed || runtime.unloadingObject == object)) {
                clearProjection(runtime.unloadingObject);
                runtime.unloadingObject = INVALID_OBJECT_ID;
                runtime.pushOutsideDistancePerFrame = {};
                changed = true;
            }
            const auto eraseObject = [object](auto& values) {
                const size_t before = values.size();
                values.erase(std::remove(values.begin(), values.end(), object),
                             values.end());
                return values.size() != before;
            };
            if (hostDestroyed) {
                changed |= !runtime.pendingUnloadObjects.empty() ||
                    !runtime.containedObjects.empty();
                runtime.pendingUnloadObjects.clear();
                runtime.containedObjects.clear();
            } else {
                changed |= eraseObject(runtime.pendingUnloadObjects);
                changed |= eraseObject(runtime.containedObjects);
            }
            if (!changed) continue;
            runtime.lastContainedCount = runtime.containedObjects.size();
            runtime.loadingOrUnloading = runtime.dockingObject ||
                runtime.unloadingObject ||
                !runtime.pendingUnloadObjects.empty();
            runtime.dockOpen = !runtime.inTransit &&
                !runtime.loadingOrUnloading;
            runtime.transitionEndsTick = confirmedTick;
            ++runtime.revision;
        }
    }
}

} // namespace engine
