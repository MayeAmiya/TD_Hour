#include "game/object/simulation/structure/ObjectBridgeDetail.h"

#include "core/container/string_utils.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/structure/ObjectMinefield.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/data/base/ObjectSimulationRules.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>

namespace engine {

namespace {
using Fixed = math::q32_32;
} // namespace

void ObjectBridgeSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick,
    container::Vector<ObjectBridgeStateEvent>& events) const {
    container::Vector<ObjectRailedTransportDockAttachCompletion> discard;
    update(registry, lifecycle, terrain, rules, confirmedTick, events,
           discard);
}

void ObjectBridgeSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick,
    container::Vector<ObjectBridgeStateEvent>& events,
    container::Vector<ObjectRailedTransportDockAttachCompletion>&
        dockCompletions) const {
    container::Vector<ObjectRailroadCarriageSpawnRequest> carriageSpawns;
    container::Vector<ObjectRailroadDisembarkRequest> disembarks;
    container::Vector<ObjectDamageRequest> railroadDamage;
    container::Vector<ObjectRailroadPresentationEvent> presentationEvents;
    uint64_t nextGameplaySubmissionOrdinal = 1;
    update(registry, lifecycle, terrain, rules, confirmedTick, events,
           nextGameplaySubmissionOrdinal, dockCompletions, carriageSpawns,
           disembarks, railroadDamage, presentationEvents);
    for (const ObjectRailroadCarriageSpawnRequest& request : carriageSpawns) {
        static_cast<void>(acknowledgeCarriageSpawn(
            registry, lifecycle, request, INVALID_OBJECT_ID, false));
    }
}

void ObjectBridgeSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick,
    container::Vector<ObjectBridgeStateEvent>& events,
    uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectRailedTransportDockAttachCompletion>&
        dockCompletions,
    container::Vector<ObjectRailroadCarriageSpawnRequest>& carriageSpawns,
    container::Vector<ObjectRailroadDisembarkRequest>& disembarks,
    container::Vector<ObjectDamageRequest>& railroadDamage,
    container::Vector<ObjectRailroadPresentationEvent>&
        presentationEvents) const {
    const auto reserveGameplayOrdinal = [&]() noexcept {
        const uint64_t result = nextGameplaySubmissionOrdinal++;
        if (nextGameplaySubmissionOrdinal == 0)
            ++nextGameplaySubmissionOrdinal;
        return result;
    };
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view =
        ecs::view<ObjectIdentityComponent, ObjectBridgeComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromIdIncludingPending(identity.id)) {
            continue;
        }
        candidates.push_back({identity.id, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    const auto nearestBridge = [&](ecs::entity source) {
        const TransformComponent* sourceTransform =
            ecs::try_get<TransformComponent>(registry, source);
        if (!sourceTransform) return INVALID_OBJECT_ID;
        const LogicFixedVec3 sourcePosition =
            readAuthoritativeObjectPosition(
                registry, source, *sourceTransform);
        ObjectId best = INVALID_OBJECT_ID;
        Fixed bestDistance =
            Fixed::from_raw(std::numeric_limits<int64_t>::max());
        for (const Candidate& bridgeCandidate : candidates) {
            const TransformComponent* bridgeTransform =
                ecs::try_get<TransformComponent>(registry,
                                                 bridgeCandidate.entity);
            if (!bridgeTransform) continue;
            const LogicFixedVec3 bridgePosition =
                readAuthoritativeObjectPosition(
                    registry, bridgeCandidate.entity, *bridgeTransform);
            const Fixed current = detail::distanceSquared(
                sourcePosition, bridgePosition);
            if (!best || current < bestDistance ||
                (current == bestDistance && bridgeCandidate.object < best)) {
                best = bridgeCandidate.object;
                bestDistance = current;
            }
        }
        return best;
    };

    const auto scaffoldView = ecs::view<ObjectBridgeScaffoldComponent>(registry);
    const auto towerView = ecs::view<ObjectBridgeTowerComponent>(registry);
    for (const ecs::entity entity : towerView) {
        ObjectBridgeTowerComponent& tower =
            towerView.template get<ObjectBridgeTowerComponent>(entity);
        if (!tower.bridge) tower.bridge = nearestBridge(entity);
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        const bool dead = health && health->effectivelyDead;
        if (dead != tower.effectivelyDead) {
            tower.effectivelyDead = dead;
            ++tower.revision;
        }
    }

    for (const Candidate& candidate : candidates) {
        ObjectBridgeComponent& bridge =
            ecs::get<ObjectBridgeComponent>(registry, candidate.entity);
        if (!bridge.plan || bridge.plan->bridges.empty()) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, candidate.entity);
        bool active = !(health && health->effectivelyDead) &&
            !lifecycle.isPendingDestroy(candidate.object) &&
            !bridge.scaffoldingPresent;
        for (const ecs::entity towerEntity : towerView) {
            const ObjectBridgeTowerComponent& tower =
                towerView.template get<ObjectBridgeTowerComponent>(towerEntity);
            if (tower.bridge == candidate.object && tower.effectivelyDead) {
                active = false;
                break;
            }
        }
        if (bridge.navigationStatePublished &&
            bridge.lastNavigationActive == active) {
            continue;
        }
        bridge.navigationStatePublished = true;
        bridge.lastNavigationActive = active;
        events.push_back({
            .object = candidate.object,
            .active = active,
            .submissionOrdinal = reserveGameplayOrdinal(),
            .confirmedTick = confirmedTick,
        });
    }

    struct ScaffoldCandidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<ScaffoldCandidate> scaffoldCandidates;
    for (const ecs::entity entity : scaffoldView) {
        const ObjectIdentityComponent* identity =
            ecs::try_get<ObjectIdentityComponent>(registry, entity);
        if (!identity || !identity->id ||
            !lifecycle.entityFromId(identity->id)) {
            continue;
        }
        scaffoldCandidates.push_back({identity->id, entity});
    }
    std::sort(scaffoldCandidates.begin(), scaffoldCandidates.end(),
              [](const ScaffoldCandidate& left,
                 const ScaffoldCandidate& right) {
                  return left.object < right.object;
              });
    for (const ScaffoldCandidate& candidate : scaffoldCandidates) {
        ObjectBridgeScaffoldComponent& scaffold =
            ecs::get<ObjectBridgeScaffoldComponent>(registry,
                                                     candidate.entity);
        bool active = false;
        if (const std::optional<ecs::entity> bridgeEntity =
                lifecycle.entityFromIdIncludingPending(scaffold.bridge)) {
            if (const ObjectBridgeComponent* bridge =
                    ecs::try_get<ObjectBridgeComponent>(registry,
                                                        *bridgeEntity)) {
                active = bridge->navigationStatePublished &&
                    bridge->lastNavigationActive;
            }
        }
        if (active != scaffold.bridgeActive) {
            scaffold.bridgeActive = active;
            ++scaffold.revision;
        }
        const ObjectBridgeScaffoldMotion before = scaffold.motion;
        const LogicFixedVec3 beforePosition = scaffold.position;
        if (detail::advanceScaffoldMotion(scaffold)) {
            writeAuthoritativeObjectPosition(registry, candidate.entity,
                                             scaffold.position);
            if (scaffold.motion != before ||
                scaffold.position.x != beforePosition.x ||
                scaffold.position.y != beforePosition.y ||
                scaffold.position.z != beforePosition.z) {
                ++scaffold.revision;
            }
        }
        if (before == ObjectBridgeScaffoldMotion::Sink &&
            scaffold.motion == ObjectBridgeScaffoldMotion::Sink &&
            scaffold.position.x == scaffold.createPosition.x &&
            scaffold.position.y == scaffold.createPosition.y &&
            scaffold.position.z == scaffold.createPosition.z &&
            !scaffold.destroyRequested) {
            scaffold.destroyRequested = lifecycle.requestDestroy(
                candidate.object, ObjectDestroyReason::System,
                confirmedTick);
            if (scaffold.destroyRequested) ++scaffold.revision;
        }
    }
    for (const ecs::entity entity : towerView) {
        ObjectBridgeTowerComponent& tower =
            towerView.template get<ObjectBridgeTowerComponent>(entity);
        bool active = false;
        if (const std::optional<ecs::entity> bridgeEntity =
                lifecycle.entityFromIdIncludingPending(tower.bridge)) {
            if (const ObjectBridgeComponent* bridge =
                    ecs::try_get<ObjectBridgeComponent>(registry,
                                                        *bridgeEntity)) {
                active = bridge->navigationStatePublished &&
                    bridge->lastNavigationActive;
            }
        }
        if (active != tower.bridgeActive) {
            tower.bridgeActive = active;
            ++tower.revision;
        }
    }

    struct RuntimeCandidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<RuntimeCandidate> runtimeCandidates;
    const auto railroadView =
        ecs::view<ObjectIdentityComponent, ObjectRailroadComponent>(registry);
    runtimeCandidates.reserve(railroadView.size_hint());
    for (const ecs::entity entity : railroadView) {
        const ObjectId object =
            railroadView.template get<ObjectIdentityComponent>(entity).id;
        if (object && lifecycle.entityFromIdIncludingPending(object)) {
            runtimeCandidates.push_back({object, entity});
        }
    }
    std::sort(runtimeCandidates.begin(), runtimeCandidates.end(),
              [](const RuntimeCandidate& left, const RuntimeCandidate& right) {
                  return left.object < right.object;
              });
    for (const RuntimeCandidate& candidate : runtimeCandidates) {
        ObjectRailroadComponent& railroad =
            ecs::get<ObjectRailroadComponent>(registry, candidate.entity);
        TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, candidate.entity);
        if (!railroad.plan || !transform) continue;
        ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, candidate.entity);
        ObjectFixedTransformComponent* fixedTransform =
            ecs::try_get<ObjectFixedTransformComponent>(registry,
                                                         candidate.entity);
        if (!fixedTransform || !fixedTransform->authoritative) continue;
        const size_t count = std::min(railroad.instances.size(),
                                      railroad.plan->railroads.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectRailroadRule& rule =
                railroad.plan->railroads[index];
            ObjectRailroadRuntime& runtime = railroad.instances[index];
            const auto publishRunningSound = [&](bool enabled) {
                if (runtime.runningSoundActive == enabled) return;
                runtime.runningSoundActive = enabled;
                if (rule.runningSound.empty()) return;
                presentationEvents.push_back({
                    .kind = enabled
                        ? ObjectRailroadPresentationEventKind::RunningLoopStarted
                        : ObjectRailroadPresentationEventKind::RunningLoopStopped,
                    .object = candidate.object,
                    .eventName = rule.runningSound,
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                });
            };
            if (!runtime.trackDataLoaded && rule.isLocomotive) {
                const LogicFixedVec3 initialPosition =
                    fixedTransform->position;
                detail::loadRailroadTrack(
                    runtime, initialPosition, terrain);
            }
            if (rule.isLocomotive) {
                runtime.locomotive = candidate.object;
                runtime.leadCarriage = true;
                if (!runtime.chainTail) runtime.chainTail = candidate.object;
            }
            if (runtime.waypointIds.size() < 2 ||
                runtime.trackPoints.size() != runtime.waypointIds.size() ||
                runtime.trackLength <= Fixed{}) continue;

            // A map artist may already have placed a complete consist behind
            // the engine.  Select the closest never-hitched allied (currently
            // represented by the same controlling Player) carriage in stable
            // ObjectId order before falling back to authored template spawns.
            if (rule.isLocomotive && !runtime.carriagesInitialized &&
                !runtime.pendingCarriageSpawn) {
                const ObjectId pullerId = runtime.chainTail
                    ? runtime.chainTail : candidate.object;
                const std::optional<ecs::entity> pullerEntity =
                    lifecycle.entityFromId(pullerId);
                ObjectRailroadComponent* pullerComponent = pullerEntity
                    ? ecs::try_get<ObjectRailroadComponent>(registry,
                                                            *pullerEntity)
                    : nullptr;
                TransformComponent* pullerTransform = pullerEntity
                    ? ecs::try_get<TransformComponent>(registry,
                                                       *pullerEntity)
                    : nullptr;
                const OwnerComponent* pullerOwner = pullerEntity
                    ? ecs::try_get<OwnerComponent>(registry, *pullerEntity)
                    : nullptr;
                ObjectId closest = INVALID_OBJECT_ID;
                Fixed closestSquared = Fixed::from_raw(
                    std::numeric_limits<int64_t>::max());
                const Fixed searchRadius = pullerComponent &&
                        index < pullerComponent->instances.size()
                    ? Fixed::max(pullerComponent->instances[index]
                                     .hitchDistance,
                                 Fixed{int32_t{1}})
                    : Fixed{int32_t{2}};
                LogicFixedVec3 hitch{};
                if (pullerTransform) {
                    const Fixed yaw = readAuthoritativeObjectYaw(
                        registry, *pullerEntity, *pullerTransform);
                    const math::q32_32_sincos facing = math::fixed_sincos(yaw);
                    hitch = readAuthoritativeObjectPosition(
                        registry, *pullerEntity, *pullerTransform);
                    hitch.x -= facing.cosine * searchRadius;
                    hitch.y -= facing.sine * searchRadius;
                }
                if (pullerComponent && pullerTransform &&
                    index < pullerComponent->instances.size()) {
                    for (const RuntimeCandidate& possible : runtimeCandidates) {
                        if (possible.object == pullerId ||
                            possible.object == candidate.object) continue;
                        ObjectRailroadComponent* possibleComponent =
                            ecs::try_get<ObjectRailroadComponent>(
                                registry, possible.entity);
                        const TransformComponent* possibleTransform =
                            ecs::try_get<TransformComponent>(registry,
                                                             possible.entity);
                        const OwnerComponent* possibleOwner =
                            ecs::try_get<OwnerComponent>(registry,
                                                         possible.entity);
                        if (!possibleComponent || !possibleComponent->plan ||
                            index >= possibleComponent->instances.size() ||
                            index >= possibleComponent->plan->railroads.size() ||
                            possibleComponent->plan->railroads[index]
                                .isLocomotive ||
                            possibleComponent->instances[index]
                                .hasEverBeenHitched ||
                            !possibleTransform || !pullerOwner ||
                            !possibleOwner ||
                            possibleOwner->player != pullerOwner->player) {
                            continue;
                        }
                        const LogicFixedVec3 possiblePosition =
                            readAuthoritativeObjectPosition(
                                registry, possible.entity,
                                *possibleTransform);
                        const Fixed dx = possiblePosition.x - hitch.x;
                        const Fixed dy = possiblePosition.y - hitch.y;
                        const Fixed dz = possiblePosition.z - hitch.z;
                        const Fixed squared = dx * dx + dy * dy + dz * dz;
                        if (squared <= searchRadius * searchRadius &&
                            (squared < closestSquared ||
                             (squared == closestSquared &&
                              possible.object < closest))) {
                            closest = possible.object;
                            closestSquared = squared;
                        }
                    }
                }

                if (closest) {
                    const std::optional<ecs::entity> carriageEntity =
                        lifecycle.entityFromId(closest);
                    ObjectRailroadComponent* carriageComponent = carriageEntity
                        ? ecs::try_get<ObjectRailroadComponent>(registry,
                                                                *carriageEntity)
                        : nullptr;
                    if (pullerComponent && carriageComponent &&
                        index < pullerComponent->instances.size() &&
                        index < carriageComponent->instances.size()) {
                        ObjectRailroadRuntime& pullerRuntime =
                            pullerComponent->instances[index];
                        ObjectRailroadRuntime& carriageRuntime =
                            carriageComponent->instances[index];
                        carriageRuntime.waypointIds = runtime.waypointIds;
                        carriageRuntime.trackPoints = runtime.trackPoints;
                        carriageRuntime.trackLength = runtime.trackLength;
                        carriageRuntime.trackDistance =
                            pullerRuntime.trackDistance -
                            carriageRuntime.hitchDistance;
                        carriageRuntime.speed = pullerRuntime.speed;
                        carriageRuntime.direction = pullerRuntime.direction;
                        carriageRuntime.locomotive = candidate.object;
                        carriageRuntime.puller = pullerId;
                        carriageRuntime.chainTail = closest;
                        carriageRuntime.trackDataLoaded = true;
                        carriageRuntime.looping = runtime.looping;
                        carriageRuntime.hasEverBeenHitched = true;
                        carriageRuntime.leadCarriage = false;
                        carriageRuntime.waitingInWings =
                            carriageRuntime.trackDistance < Fixed{};
                        pullerRuntime.trailer = closest;
                        runtime.chainTail = closest;
                        runtime.proximityChain = true;
                        if (ObjectProducerComponent* producer =
                                ecs::try_get<ObjectProducerComponent>(
                                    registry, *carriageEntity)) {
                            producer->producer = pullerId;
                        } else {
                            ecs::emplace<ObjectProducerComponent>(
                                registry, *carriageEntity,
                                ObjectProducerComponent{
                                    .producer = pullerId});
                        }
                        ++pullerRuntime.revision;
                        ++carriageRuntime.revision;
                        ++runtime.revision;
                    }
                } else if (runtime.proximityChain ||
                           rule.carriageTemplateNames.empty()) {
                    runtime.carriagesInitialized = true;
                    ++runtime.revision;
                } else if (runtime.nextCarriageTemplateIndex <
                           rule.carriageTemplateNames.size() && pullerEntity &&
                           pullerTransform && pullerOwner) {
                    const PrimaryTeamComponent* team =
                        ecs::try_get<PrimaryTeamComponent>(registry,
                                                           *pullerEntity);
                    const uint32_t sequence = runtime.nextSpawnSequence++;
                    if (runtime.nextSpawnSequence == 0)
                        ++runtime.nextSpawnSequence;
                    runtime.pendingCarriageSpawn = true;
                    runtime.pendingSpawnSequence = sequence;
                    runtime.pendingCarriageTemplateIndex =
                        runtime.nextCarriageTemplateIndex;
                    carriageSpawns.push_back({
                        .locomotive = candidate.object,
                        .puller = pullerId,
                        .templateName = rule.carriageTemplateNames[
                            runtime.nextCarriageTemplateIndex],
                        .owner = pullerOwner->player,
                        .primaryTeam = team ? team->team
                                            : INVALID_OBJECT_TEAM_ID,
                        .transform = ObjectFixedTransformComponent{
                            .position = readAuthoritativeObjectPosition(
                                registry, *pullerEntity, *pullerTransform),
                            .yawRadians = readAuthoritativeObjectYaw(
                                registry, *pullerEntity, *pullerTransform),
                            .authoritative = true,
                        },
                        .railroadRuleIndex = static_cast<uint32_t>(index),
                        .carriageTemplateIndex =
                            runtime.nextCarriageTemplateIndex,
                        .requestSequence = sequence,
                        .submissionOrdinal = reserveGameplayOrdinal(),
                        .confirmedTick = confirmedTick,
                    });
                    ++runtime.revision;
                }
            }

            if (!rule.isLocomotive && runtime.puller) {
                const std::optional<ecs::entity> pullerEntity =
                    lifecycle.entityFromId(runtime.puller);
                ObjectRailroadComponent* pullerComponent = pullerEntity
                    ? ecs::try_get<ObjectRailroadComponent>(registry,
                                                            *pullerEntity)
                    : nullptr;
                if (pullerComponent && index <
                        pullerComponent->instances.size()) {
                    const ObjectRailroadRuntime& puller =
                        pullerComponent->instances[index];
                    runtime.trackDistance = puller.trackDistance -
                        runtime.hitchDistance;
                    runtime.speed = puller.speed;
                    runtime.direction = puller.direction;
                    runtime.state = puller.state;
                    runtime.unpulledTicks = 0;
                } else if (++runtime.unpulledTicks > 2u) {
                    runtime.puller = INVALID_OBJECT_ID;
                    runtime.leadCarriage = true;
                    runtime.state = ObjectRailroadConductorState::Coasting;
                }
            }

            const bool ownsTrackAdvance = rule.isLocomotive ||
                runtime.leadCarriage;
            if (ownsTrackAdvance &&
                runtime.state != ObjectRailroadConductorState::EndOfLine) {
                if (runtime.state ==
                        ObjectRailroadConductorState::WaitingAtStation) {
                    // Script RailroadHeld affects only departure.  It never
                    // freezes an engine between stations.
                    if (confirmedTick < runtime.waitUntilTick ||
                        runtime.held) {
                        const uint64_t waitTicks = detail::millisecondsToTicks(
                            rule.waitAtStationMilliseconds,
                            rules.logicFramesPerSecond);
                        if (!runtime.stationWhistlePlayed &&
                            confirmedTick < runtime.waitUntilTick &&
                            runtime.waitUntilTick - confirmedTick <=
                                waitTicks / 4u) {
                            runtime.stationWhistlePlayed = true;
                            if (!rule.whistleSound.empty()) {
                                presentationEvents.push_back({
                                    .kind = ObjectRailroadPresentationEventKind::Whistle,
                                    .object = candidate.object,
                                    .eventName = rule.whistleSound,
                                    .authoredOrder = rule.authoredOrder,
                                    .confirmedTick = confirmedTick,
                                });
                            }
                        }
                        publishRunningSound(false);
                        static_cast<void>(detail::publishRailroadPosition(
                            runtime, *transform, physics, fixedTransform));
                        markObjectDirty(
                            registry, candidate.entity,
                            objectDirtyBit(ObjectDirtyDomain::Spatial) |
                                objectDirtyBit(
                                    ObjectDirtyDomain::RenderExtraction));
                        continue;
                    }
                    runtime.state =
                        ObjectRailroadConductorState::Accelerating;
                    runtime.speed = Fixed::from_fraction(1, 20) *
                        Fixed{runtime.direction};
                    runtime.stationWhistlePlayed = false;
                }
                if (runtime.state == ObjectRailroadConductorState::Braking) {
                    runtime.speed *= rule.braking;
                    if (Fixed::abs(runtime.speed) < Fixed::from_fraction(1, 10)) {
                        runtime.speed = {};
                        runtime.state =
                            ObjectRailroadConductorState::WaitingAtStation;
                        runtime.stationWhistlePlayed = false;
                        runtime.waitUntilTick = detail::saturatingAdd(
                            confirmedTick, detail::millisecondsToTicks(
                                rule.waitAtStationMilliseconds,
                                rules.logicFramesPerSecond));
                        if (runtime.disembarkAtStop) {
                            ObjectId carriage = candidate.object;
                            container::Vector<ObjectId> visited;
                            while (carriage &&
                                   std::find(visited.begin(), visited.end(),
                                             carriage) == visited.end()) {
                                visited.push_back(carriage);
                                disembarks.push_back({
                                    .carriage = carriage,
                                    .railroadRuleIndex =
                                        static_cast<uint32_t>(index),
                                    .submissionOrdinal =
                                        reserveGameplayOrdinal(),
                                    .confirmedTick = confirmedTick,
                                });
                                const std::optional<ecs::entity> entity =
                                    lifecycle.entityFromId(carriage);
                                const ObjectRailroadComponent* component =
                                    entity ? ecs::try_get<
                                        ObjectRailroadComponent>(registry,
                                                                  *entity)
                                           : nullptr;
                                carriage = component && index <
                                        component->instances.size()
                                    ? component->instances[index].trailer
                                    : INVALID_OBJECT_ID;
                            }
                            runtime.disembarkAtStop = false;
                        }
                    }
                } else if (runtime.state ==
                           ObjectRailroadConductorState::Coasting) {
                    runtime.speed *= rule.friction;
                } else if (runtime.state ==
                           ObjectRailroadConductorState::Accelerating) {
                    runtime.speed += Fixed::from_fraction(1, 50) *
                        Fixed{runtime.direction};
                    runtime.speed *= rule.acceleration;
                }
                runtime.speed = std::clamp(runtime.speed, -rule.speedMax,
                                           rule.speedMax);
                runtime.trackDistance += runtime.speed;
                if (runtime.looping && runtime.trackLength > Fixed{}) {
                    while (runtime.trackDistance >= runtime.trackLength)
                        runtime.trackDistance -= runtime.trackLength;
                    while (runtime.trackDistance < Fixed{})
                        runtime.trackDistance += runtime.trackLength;
                } else if (runtime.trackDistance >= runtime.trackLength) {
                    runtime.trackDistance = runtime.trackLength;
                    runtime.speed = {};
                    runtime.endOfLine = true;
                    runtime.state =
                        ObjectRailroadConductorState::EndOfLine;
                } else if (runtime.trackDistance < Fixed{}) {
                    runtime.trackDistance = {};
                    runtime.speed = {};
                    runtime.waitingInWings = true;
                } else {
                    runtime.waitingInWings = false;
                }
            }

            const uint32_t segment = detail::publishRailroadPosition(
                runtime, *transform, physics, fixedTransform);
            markObjectDirty(
                registry, candidate.entity,
                objectDirtyBit(ObjectDirtyDomain::Spatial) |
                    objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
            if (rule.isLocomotive && segment !=
                    std::numeric_limits<uint32_t>::max() &&
                segment != runtime.currentSegment &&
                segment < runtime.waypointIds.size()) {
                const game::terrain::WaypointRecord* point =
                    terrain.waypointById(runtime.waypointIds[segment]);
                if (point) {
                    const container::StringView name = point->name;
                    if (!container::endsWithIgnoreCase(name, "Tunnel") &&
                        !rule.clicketyClackSound.empty()) {
                        presentationEvents.push_back({
                            .kind = ObjectRailroadPresentationEventKind::ClicketyClack,
                            .object = candidate.object,
                            .eventName = rule.clicketyClackSound,
                            .volumeScale = Fixed::abs(runtime.speed) /
                                Fixed{int32_t{10}},
                            .authoredOrder = rule.authoredOrder,
                            .confirmedTick = confirmedTick,
                        });
                    }
                    if (container::endsWithIgnoreCase(name, "Disembark")) {
                        runtime.state =
                            ObjectRailroadConductorState::Braking;
                        runtime.disembarkAtStop = true;
                    } else if (container::endsWithIgnoreCase(name, "Station")) {
                        runtime.state =
                            ObjectRailroadConductorState::Braking;
                        runtime.disembarkAtStop = false;
                    } else if (container::endsWithIgnoreCase(name, "PingPong")) {
                        runtime.state =
                            ObjectRailroadConductorState::Braking;
                        runtime.disembarkAtStop = false;
                        runtime.direction = -runtime.direction;
                    }
                }
                runtime.currentSegment = segment;
            }
            publishRunningSound(
                runtime.state == ObjectRailroadConductorState::Accelerating &&
                runtime.speed != Fixed{});
            if (runtime.endOfLine && !runtime.trailer) {
                static_cast<void>(lifecycle.requestDestroy(
                    candidate.object, ObjectDestroyReason::System,
                    confirmedTick));
            }
            ++runtime.revision;
        }
    }

    // Resolve every attached consist root-to-tail after all conductors have
    // advanced.  Map object IDs are not required to place the locomotive
    // before its pre-authored carriages, so a single EnTT/ObjectId pass would
    // otherwise leave lower-ID cars one confirmed frame behind.
    for (const RuntimeCandidate& candidate : runtimeCandidates) {
        ObjectRailroadComponent* rootComponent =
            ecs::try_get<ObjectRailroadComponent>(registry,
                                                   candidate.entity);
        if (!rootComponent || !rootComponent->plan) continue;
        const size_t count = std::min(rootComponent->instances.size(),
                                      rootComponent->plan->railroads.size());
        for (size_t index = 0; index < count; ++index) {
            if (!rootComponent->plan->railroads[index].isLocomotive)
                continue;
            ObjectId pullerId = candidate.object;
            ObjectRailroadRuntime* puller =
                &rootComponent->instances[index];
            container::Vector<ObjectId> visited{candidate.object};
            while (puller->trailer &&
                   std::find(visited.begin(), visited.end(),
                             puller->trailer) == visited.end()) {
                const ObjectId carriageId = puller->trailer;
                visited.push_back(carriageId);
                const std::optional<ecs::entity> carriageEntity =
                    lifecycle.entityFromId(carriageId);
                ObjectRailroadComponent* carriageComponent = carriageEntity
                    ? ecs::try_get<ObjectRailroadComponent>(registry,
                                                             *carriageEntity)
                    : nullptr;
                TransformComponent* carriageTransform = carriageEntity
                    ? ecs::try_get<TransformComponent>(registry,
                                                        *carriageEntity)
                    : nullptr;
                if (!carriageComponent ||
                    index >= carriageComponent->instances.size() ||
                    !carriageTransform) {
                    puller->trailer = INVALID_OBJECT_ID;
                    ++puller->revision;
                    break;
                }
                ObjectRailroadRuntime& carriage =
                    carriageComponent->instances[index];
                carriage.trackDistance = puller->trackDistance -
                    carriage.hitchDistance;
                carriage.speed = puller->speed;
                carriage.direction = puller->direction;
                carriage.state = puller->state;
                carriage.puller = pullerId;
                carriage.unpulledTicks = 0;
                carriage.waitingInWings =
                    !carriage.looping && carriage.trackDistance < Fixed{};
                carriage.endOfLine = !carriage.looping &&
                    carriage.trackDistance >= carriage.trackLength;
                ObjectPhysicsComponent* carriagePhysics =
                    ecs::try_get<ObjectPhysicsComponent>(registry,
                                                          *carriageEntity);
                ObjectFixedTransformComponent* carriageFixedTransform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, *carriageEntity);
                if (!carriageFixedTransform ||
                    !carriageFixedTransform->authoritative) {
                    puller->trailer = INVALID_OBJECT_ID;
                    ++puller->revision;
                    break;
                }
                static_cast<void>(detail::publishRailroadPosition(
                    carriage, *carriageTransform, carriagePhysics,
                    carriageFixedTransform));
                markObjectDirty(
                    registry, *carriageEntity,
                    objectDirtyBit(ObjectDirtyDomain::Spatial) |
                        objectDirtyBit(
                            ObjectDirtyDomain::RenderExtraction));
                ++carriage.revision;
                pullerId = carriageId;
                puller = &carriage;
            }
        }
    }

    // Railroad publishes after ordinary free-body collision resolution, so
    // its track-constrained motion owns a narrow deterministic contact pass.
    // This reproduces RailroadBehavior::onCollide without restoring callback
    // pointers: contacts become Body damage intents and Physics value edits.
    struct CollisionCandidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<CollisionCandidate> collisionCandidates;
    const auto collisionView = ecs::view<ObjectIdentityComponent,
        TransformComponent, ObjectGeometryComponent>(registry);
    collisionCandidates.reserve(collisionView.size_hint());
    for (const ecs::entity entity : collisionView) {
        const ObjectId object = collisionView
            .template get<ObjectIdentityComponent>(entity).id;
        const ObjectMapStatusComponent* map =
            ecs::try_get<ObjectMapStatusComponent>(registry, entity);
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, entity);
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, entity);
        if (!object || !lifecycle.entityFromId(object) ||
            lifecycle.isPendingDestroy(object) || (map && map->offMap) ||
            ecs::try_get<ObjectContainedByComponent>(registry, entity) ||
            detail::railroadHasKind(kinds, game::ObjectKindOf::NoCollide) ||
            (status && status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::NoCollisions)))) {
            continue;
        }
        collisionCandidates.push_back({object, entity});
    }
    std::sort(collisionCandidates.begin(), collisionCandidates.end(),
        [](const CollisionCandidate& left,
           const CollisionCandidate& right) {
            return left.object < right.object;
        });
    container::Vector<ObjectId> consumedDemoTraps;
    uint32_t railroadDamageSequence = 1;
    for (const RuntimeCandidate& train : runtimeCandidates) {
        ObjectRailroadComponent* component =
            ecs::try_get<ObjectRailroadComponent>(registry, train.entity);
        if (!component || !component->plan) continue;
        const TransformComponent* trainTransform =
            ecs::try_get<TransformComponent>(registry, train.entity);
        const ObjectGeometryComponent* trainGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, train.entity);
        if (!trainTransform || !trainGeometry) continue;
        const LogicFixedVec3 trainPosition =
            readAuthoritativeObjectPosition(
                registry, train.entity, *trainTransform);
        const Fixed trainYaw = readAuthoritativeObjectYaw(
            registry, train.entity, *trainTransform);
        const size_t railroadCount = std::min(
            component->instances.size(), component->plan->railroads.size());
        for (size_t railroadIndex = 0; railroadIndex < railroadCount;
             ++railroadIndex) {
            ObjectRailroadRuntime& runtime =
                component->instances[railroadIndex];
            const game::ObjectRailroadRule& rule =
                component->plan->railroads[railroadIndex];
            if (!runtime.trackDataLoaded || runtime.waitingInWings ||
                runtime.endOfLine) {
                runtime.collisionWhistleActive = false;
                continue;
            }
            bool collisionContact = false;
            bool trainKilledByDemoTrap = false;
            for (const CollisionCandidate& victim : collisionCandidates) {
                if (victim.object == train.object ||
                    lifecycle.isPendingDestroy(victim.object)) continue;
                const TransformComponent& victimTransform =
                    ecs::get<TransformComponent>(registry, victim.entity);
                const ObjectGeometryComponent& victimGeometry =
                    ecs::get<ObjectGeometryComponent>(registry, victim.entity);
                const LogicFixedVec3 victimPosition =
                    readAuthoritativeObjectPosition(
                        registry, victim.entity, victimTransform);
                const Fixed victimYaw = readAuthoritativeObjectYaw(
                    registry, victim.entity, victimTransform);
                ObjectCollisionContact contact;
                if (!computeObjectCollisionContact(
                        trainPosition, trainYaw, *trainGeometry,
                        victimPosition, victimYaw, victimGeometry,
                        contact)) continue;
                collisionContact = true;
                if (!runtime.collisionWhistleActive &&
                    !rule.whistleSound.empty()) {
                    runtime.collisionWhistleActive = true;
                    presentationEvents.push_back({
                        .kind = ObjectRailroadPresentationEventKind::Whistle,
                        .object = train.object,
                        .eventName = rule.whistleSound,
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                }

                ObjectRailroadComponent* victimRailroad =
                    ecs::try_get<ObjectRailroadComponent>(registry,
                                                           victim.entity);
                if (victimRailroad && victimRailroad->plan && railroadIndex <
                        victimRailroad->instances.size() &&
                    railroadIndex < victimRailroad->plan->railroads.size()) {
                    // Process each train pair once. Cars of the same stable
                    // consist never attack one another even when tight bends
                    // make their gameplay geometry overlap.
                    if (victim.object < train.object) continue;
                    const ObjectRailroadRuntime& other =
                        victimRailroad->instances[railroadIndex];
                    const ObjectId ourRoot = runtime.locomotive
                        ? runtime.locomotive : train.object;
                    const ObjectId theirRoot = other.locomotive
                        ? other.locomotive : victim.object;
                    if (ourRoot == theirRoot) continue;
                    const bool otherLocomotive =
                        victimRailroad->plan->railroads[railroadIndex]
                            .isLocomotive;
                    if (rule.isLocomotive) {
                        railroadDamage.push_back({
                            .target = victim.object,
                            .source = train.object,
                            .sourceSequence = railroadDamageSequence++,
                            .submissionOrdinal = reserveGameplayOrdinal(),
                            .causalGroup = ourRoot,
                            .damageType = game::DamageType::UNRESISTABLE,
                            .deathType = game::DeathType::CRUSHED,
                            .forceKill = true,
                            .confirmedTick = confirmedTick,
                        });
                    } else if (otherLocomotive) {
                        railroadDamage.push_back({
                            .target = train.object,
                            .source = victim.object,
                            .sourceSequence = railroadDamageSequence++,
                            .submissionOrdinal = reserveGameplayOrdinal(),
                            .causalGroup = theirRoot,
                            .damageType = game::DamageType::UNRESISTABLE,
                            .deathType = game::DeathType::CRUSHED,
                            .forceKill = true,
                            .confirmedTick = confirmedTick,
                        });
                    } else if (runtime.leadCarriage || other.leadCarriage) {
                        for (const auto [target, source, group] :
                             {std::tuple{train.object, victim.object,
                                         theirRoot},
                              std::tuple{victim.object, train.object,
                                         ourRoot}}) {
                            railroadDamage.push_back({
                                .target = target,
                                .source = source,
                                .sourceSequence = railroadDamageSequence++,
                                .submissionOrdinal = reserveGameplayOrdinal(),
                                .causalGroup = group,
                                .damageType = game::DamageType::UNRESISTABLE,
                                .deathType = game::DeathType::CRUSHED,
                                .forceKill = true,
                                .confirmedTick = confirmedTick,
                            });
                        }
                    }
                    continue;
                }

                const ObjectContainedByComponent* contained =
                    ecs::try_get<ObjectContainedByComponent>(registry,
                                                              victim.entity);
                if (contained && contained->container == train.object)
                    continue;
                const ObjectKindOfComponent* victimKinds =
                    ecs::try_get<ObjectKindOfComponent>(registry,
                                                         victim.entity);
                if (detail::railroadHasKind(
                        victimKinds, game::ObjectKindOf::Structure)) {
                    const ObjectMinefieldComponent* minefield =
                        ecs::try_get<ObjectMinefieldComponent>(
                            registry, victim.entity);
                    const bool demoTrap = minefield && minefield->plan &&
                        !minefield->plan->demoTraps.empty();
                    if (demoTrap) {
                        // RailroadBehavior gives DemoTrapUpdate a dedicated
                        // collision path before ordinary structure handling.
                        // A completed trap kills the train, then the train
                        // kills the trap; an under-construction trap can be
                        // crushed but cannot detonate on the locomotive.
                        if (std::find(consumedDemoTraps.begin(),
                                      consumedDemoTraps.end(),
                                      victim.object) !=
                            consumedDemoTraps.end()) {
                            continue;
                        }
                        consumedDemoTraps.push_back(victim.object);
                        const ObjectStatusComponent* victimStatus =
                            ecs::try_get<ObjectStatusComponent>(
                                registry, victim.entity);
                        const bool underConstruction = victimStatus &&
                            victimStatus->hasAny(game::objectStatusBit(
                                game::ObjectStatusFlag::UnderConstruction));
                        if (!underConstruction) {
                            railroadDamage.push_back({
                                .target = train.object,
                                .source = victim.object,
                                .sourceSequence = railroadDamageSequence++,
                                .submissionOrdinal = reserveGameplayOrdinal(),
                                .causalGroup = victim.object,
                                .damageType = game::DamageType::UNRESISTABLE,
                                .deathType = game::DeathType::NORMAL,
                                .forceKill = true,
                                .confirmedTick = confirmedTick,
                            });
                            trainKilledByDemoTrap = true;
                        }
                        railroadDamage.push_back({
                            .target = victim.object,
                            .source = train.object,
                            .sourceSequence = railroadDamageSequence++,
                            .submissionOrdinal = reserveGameplayOrdinal(),
                            .causalGroup = runtime.locomotive
                                ? runtime.locomotive : train.object,
                            .damageType = game::DamageType::UNRESISTABLE,
                            .deathType = game::DeathType::NORMAL,
                            .forceKill = true,
                            .confirmedTick = confirmedTick,
                        });
                        if (trainKilledByDemoTrap) break;
                        continue;
                    }
                    const bool factionStructure =
                        detail::railroadHasKind(
                            victimKinds, game::ObjectKindOf::FsPower) ||
                        detail::railroadHasKind(
                            victimKinds, game::ObjectKindOf::FsFactory) ||
                        detail::railroadHasKind(
                            victimKinds, game::ObjectKindOf::FsBaseDefense) ||
                        detail::railroadHasKind(
                            victimKinds, game::ObjectKindOf::FsTechnology) ||
                        detail::railroadHasKind(
                            victimKinds, game::ObjectKindOf::RebuildHole);
                    if (factionStructure) {
                        railroadDamage.push_back({
                            .target = victim.object,
                            .source = train.object,
                            .sourceSequence = railroadDamageSequence++,
                            .submissionOrdinal = reserveGameplayOrdinal(),
                            .causalGroup = runtime.locomotive
                                ? runtime.locomotive : train.object,
                            .damageType = game::DamageType::UNRESISTABLE,
                            .deathType = game::DeathType::CRUSHED,
                            .forceKill = true,
                            .confirmedTick = confirmedTick,
                        });
                    }
                    continue;
                }
                ObjectPhysicsComponent* victimPhysics =
                    ecs::try_get<ObjectPhysicsComponent>(registry,
                                                          victim.entity);
                if (!victimPhysics || !rule.isLocomotive ||
                    runtime.state ==
                        ObjectRailroadConductorState::WaitingAtStation ||
                    (runtime.state == ObjectRailroadConductorState::Coasting &&
                     Fixed::abs(runtime.speed) <
                         rule.runningGarrisonSpeedMax)) {
                    continue;
                }
                const Fixed impactSpeed = Fixed::abs(runtime.speed);
                const Fixed impactDamage = impactSpeed >= rule.killSpeedMin
                    ? Fixed{} : impactSpeed * Fixed{int32_t{10}};
                railroadDamage.push_back({
                    .target = victim.object,
                    .source = train.object,
                    .sourceSequence = railroadDamageSequence++,
                    .submissionOrdinal = reserveGameplayOrdinal(),
                    .causalGroup = runtime.locomotive
                        ? runtime.locomotive : train.object,
                    .amount = impactDamage,
                    .damageType = impactSpeed >= rule.killSpeedMin
                        ? game::DamageType::UNRESISTABLE
                        : game::DamageType::CRUSH,
                    .deathType = game::DeathType::CRUSHED,
                    .forceKill = impactSpeed >= rule.killSpeedMin,
                    .confirmedTick = confirmedTick,
                });

                const Fixed impulse = Fixed::min(
                    Fixed::from_fraction(7, 5),
                    impactSpeed * Fixed::from_fraction(33, 50)) *
                    Fixed{static_cast<int32_t>(std::max(
                        1u, rules.logicFramesPerSecond))};
                const math::q32_32_sincos facing =
                    math::fixed_sincos(trainYaw);
                victimPhysics->velocityUnitsPerSecond.x +=
                    facing.cosine * impulse;
                victimPhysics->velocityUnitsPerSecond.y +=
                    facing.sine * impulse;
                victimPhysics->velocityUnitsPerSecond.z = Fixed::max(
                    victimPhysics->velocityUnitsPerSecond.z,
                    Fixed::from_fraction(1, 20) * Fixed{static_cast<int32_t>(std::max(
                        1u, rules.logicFramesPerSecond))});
                victimPhysics->allowToFall = true;
                victimPhysics->allowBouncing = true;
                victimPhysics->applyFriction2DWhenAirborne = true;
                victimPhysics->sleeping = false;
            }
            runtime.collisionWhistleActive = collisionContact;
        }
    }

    runtimeCandidates.clear();
    const auto transportView = ecs::view<ObjectIdentityComponent,
        ObjectRailedTransportRuntimeComponent>(registry);
    runtimeCandidates.reserve(transportView.size_hint());
    for (const ecs::entity entity : transportView) {
        const ObjectId object =
            transportView.template get<ObjectIdentityComponent>(entity).id;
        if (object && lifecycle.entityFromIdIncludingPending(object)) {
            runtimeCandidates.push_back({object, entity});
        }
    }
    std::sort(runtimeCandidates.begin(), runtimeCandidates.end(),
              [](const RuntimeCandidate& left, const RuntimeCandidate& right) {
                  return left.object < right.object;
              });
    for (const RuntimeCandidate& candidate : runtimeCandidates) {
        ObjectRailedTransportRuntimeComponent& component =
            ecs::get<ObjectRailedTransportRuntimeComponent>(registry,
                                                             candidate.entity);
        if (!component.plan) continue;
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, candidate.entity);
        const LogicFixedVec3 transportPosition = transform
            ? readAuthoritativeObjectPosition(
                  registry, candidate.entity, *transform)
            : LogicFixedVec3{};
        const Fixed transportYaw = transform
            ? readAuthoritativeObjectYaw(
                  registry, candidate.entity, *transform)
            : Fixed{};
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                     candidate.entity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, candidate.entity);
        const ObjectContainmentComponent* contents =
            ecs::try_get<ObjectContainmentComponent>(registry,
                                                      candidate.entity);
        for (size_t index = 0; index < component.instances.size(); ++index) {
            ObjectRailedTransportRuntime& runtime = component.instances[index];
            const game::ObjectRailedTransportAiRule* aiRule =
                index < component.plan->railedTransportAi.size()
                ? &component.plan->railedTransportAi[index]
                : nullptr;
            const game::ObjectRailedTransportDockRule* dockRule =
                index == 0 &&
                    index < component.plan->railedTransportDocks.size()
                ? &component.plan->railedTransportDocks[index]
                : nullptr;

            const uint64_t containmentRevision =
                contents ? contents->revision : 0;
            container::Vector<ObjectId> currentContained;
            if (contents && dockRule) {
                currentContained.reserve(contents->objects.size());
                for (const ObjectContainedObjectRecord& record :
                     contents->objects) {
                    if (record.object) currentContained.push_back(record.object);
                }
                std::sort(currentContained.begin(), currentContained.end());
                currentContained.erase(
                    std::unique(currentContained.begin(),
                                currentContained.end()),
                    currentContained.end());
            }
            if (!runtime.containmentSnapshotInitialized) {
                runtime.containmentSnapshotInitialized = true;
                runtime.containedObjects = currentContained;
                runtime.observedContainmentRevision = containmentRevision;
                runtime.lastContainedCount = currentContained.size();
            } else if (containmentRevision !=
                       runtime.observedContainmentRevision) {
                container::Vector<ObjectId> removed;
                std::set_difference(
                    runtime.containedObjects.begin(),
                    runtime.containedObjects.end(),
                    currentContained.begin(), currentContained.end(),
                    std::back_inserter(removed));
                for (const ObjectId object : removed) {
                    if (object == runtime.unloadingObject ||
                        std::find(runtime.pendingUnloadObjects.begin(),
                                  runtime.pendingUnloadObjects.end(),
                                  object) !=
                            runtime.pendingUnloadObjects.end()) {
                        continue;
                    }
                    runtime.pendingUnloadObjects.push_back(object);
                }
                std::sort(runtime.pendingUnloadObjects.begin(),
                          runtime.pendingUnloadObjects.end());
                const bool loading = currentContained.size() >
                    runtime.containedObjects.size();
                runtime.containedObjects = currentContained;
                runtime.observedContainmentRevision = containmentRevision;
                runtime.lastContainedCount = currentContained.size();
                if (loading && dockRule &&
                    dockRule->pullInsideDurationMilliseconds != 0) {
                    runtime.loadingOrUnloading = true;
                    runtime.transitionEndsTick = detail::saturatingAdd(
                        confirmedTick,
                        detail::millisecondsToTicks(
                            dockRule->pullInsideDurationMilliseconds,
                            rules.logicFramesPerSecond));
                }
                if (!removed.empty()) {
                    runtime.loadingOrUnloading = true;
                    // Push-out completion, not a stale earlier pull-in
                    // deadline, decides when this dock becomes available.
                    runtime.transitionEndsTick = confirmedTick;
                }
                if (runtime.loadingOrUnloading) runtime.dockOpen = false;
                ++runtime.revision;
            }

            if (runtime.dockingObject && !runtime.dockingAwaitingCommit) {
                const ObjectId docker = runtime.dockingObject;
                const std::optional<ecs::entity> objectEntity =
                    lifecycle.entityFromId(docker);
                const auto finishDocking = [&](bool accepted) {
                    dockCompletions.push_back({
                        .request = {
                            .container = candidate.object,
                            .object = docker,
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
                            .submissionOrdinal = reserveGameplayOrdinal(),
                            .confirmedTick = confirmedTick,
                        },
                        .accepted = accepted,
                    });
                    // Completion is only phase one.  Keep the exact runtime
                    // transaction closed until containment acknowledges its
                    // structural commit (or rejection), and emit only once.
                    runtime.dockingAwaitingCommit = true;
                    ++runtime.revision;
                };
                if (!transform || !objectEntity ||
                    lifecycle.isPendingDestroy(docker)) {
                    finishDocking(false);
                } else {
                    TransformComponent* dockerTransform =
                        ecs::try_get<TransformComponent>(
                            registry, *objectEntity);
                    if (!dockerTransform) {
                        finishDocking(false);
                    } else {
                        LogicFixedVec3 position =
                            readAuthoritativeObjectPosition(
                                registry, *objectEntity,
                                *dockerTransform);
                        const LogicFixedVec3 center = transportPosition;
                        const Fixed dx = center.x - position.x;
                        const Fixed dy = center.y - position.y;
                        const Fixed distance = Fixed::sqrt(
                            dx * dx + dy * dy);
                        const Fixed closeEnough{int32_t{6}};
                        const bool finished = distance <= closeEnough ||
                            runtime.pullInsideDistancePerFrame <= Fixed{} ||
                            runtime.pullInsideDistancePerFrame >= distance;
                        if (finished) {
                            position.x = center.x;
                            position.y = center.y;
                        } else {
                            position.x += dx / distance *
                                runtime.pullInsideDistancePerFrame;
                            position.y += dy / distance *
                                runtime.pullInsideDistancePerFrame;
                        }
                        // RefCode deliberately keeps the docker's ground Z
                        // while faking the horizontal pull into the ferry.
                        writeAuthoritativeObjectPosition(
                            registry, *objectEntity, position);
                        if (finished) finishDocking(true);
                    }
                }
            }

            // RailedTransportContain deliberately leaves ordinary
            // ExitStart/ExitEnd placement to this dock.  A removed stable ID
            // is pushed from the ferry center to pristine DockEnd, then given
            // one typed Move toward DockWaiting07 so normal locomotion clears
            // the loading lane. Multiple removals remain ObjectId-stable and
            // are processed one at a time like RefCode unloadAll().
            if (!runtime.unloadingObject &&
                !runtime.pendingUnloadObjects.empty() && transform) {
                while (!runtime.pendingUnloadObjects.empty() &&
                       !runtime.unloadingObject) {
                    const ObjectId object =
                        runtime.pendingUnloadObjects.front();
                    runtime.pendingUnloadObjects.erase(
                        runtime.pendingUnloadObjects.begin());
                    const std::optional<ecs::entity> objectEntity =
                        lifecycle.entityFromId(object);
                    if (!objectEntity ||
                        lifecycle.isPendingDestroy(object)) continue;
                    runtime.unloadingObject = object;
                    const ObjectTerrainLayerComponent* ferryLayer =
                        ecs::try_get<ObjectTerrainLayerComponent>(
                            registry, candidate.entity);
                    if (ferryLayer) {
                        ObjectTerrainLayerComponent* dockerLayer =
                            ecs::try_get<ObjectTerrainLayerComponent>(
                                registry, *objectEntity);
                        if (dockerLayer) {
                            static_cast<void>(dockerLayer->assign(
                                ferryLayer->pathfindLayer,
                                confirmedTick));
                        } else {
                            ecs::emplace<ObjectTerrainLayerComponent>(
                                registry, *objectEntity,
                                ObjectTerrainLayerComponent{
                                    .pathfindLayer =
                                        ferryLayer->pathfindLayer,
                                    .lastChangedTick = confirmedTick,
                                });
                        }
                    }
                    const LogicFixedVec3 start = transportPosition;
                    writeAuthoritativeObjectTransform(
                        registry, *objectEntity, start, transportYaw);
                    runtime.unloadDestination = runtime.dockEndValid
                        ? detail::transformLocalBridgePoint(
                              transportPosition, transportYaw,
                              runtime.dockEndLocal)
                        : start;
                    if (terrain.isLoaded()) {
                        const uint32_t pathfindLayer = ferryLayer
                            ? ferryLayer->pathfindLayer
                            : game::terrain::kGroundPathfindLayer;
                        runtime.unloadDestination.z =
                            math::q32_32::from_raw(
                                terrain.pathfindLayerHeightRawAt(
                                    pathfindLayer,
                                    runtime.unloadDestination.x.raw(),
                                    runtime.unloadDestination.y.raw())
                                    .value_or(terrain.groundHeightRaw(
                                        runtime.unloadDestination.x.raw(),
                                        runtime.unloadDestination.y.raw())));
                    }
                    const uint64_t durationTicks = std::max<uint64_t>(
                        1u, detail::millisecondsToTicks(
                            dockRule
                                ? dockRule->pushOutsideDurationMilliseconds
                                : 0u,
                            rules.logicFramesPerSecond));
                    runtime.pushOutsideDistancePerFrame =
                        detail::fixedDistance(start, runtime.unloadDestination) /
                        Fixed{static_cast<int32_t>(std::min<uint64_t>(
                            durationTicks,
                            static_cast<uint64_t>(
                                std::numeric_limits<int32_t>::max())))};
                    static_cast<void>(ObjectDisabledSystem::setUntil(
                        registry, *objectEntity, ObjectDisabledReason::Held,
                        std::numeric_limits<uint64_t>::max(), confirmedTick));
                    static_cast<void>(ObjectStatusSystem::apply(
                        registry, *objectEntity,
                        {.setMask = game::objectStatusBit(
                             game::ObjectStatusFlag::Unselectable),
                         .confirmedTick = confirmedTick}));
                    detail::projectRailedDockerMoving(
                        registry, *objectEntity, true, confirmedTick);
                    runtime.loadingOrUnloading = true;
                    runtime.dockOpen = false;
                    ++runtime.revision;
                }
            }
            if (runtime.unloadingObject) {
                const std::optional<ecs::entity> objectEntity =
                    lifecycle.entityFromId(runtime.unloadingObject);
                if (!objectEntity ||
                    lifecycle.isPendingDestroy(runtime.unloadingObject)) {
                    runtime.unloadingObject = INVALID_OBJECT_ID;
                    ++runtime.revision;
                } else {
                    TransformComponent* objectTransform =
                        ecs::try_get<TransformComponent>(
                            registry, *objectEntity);
                    if (!objectTransform) {
                        runtime.unloadingObject = INVALID_OBJECT_ID;
                        ++runtime.revision;
                    } else {
                        LogicFixedVec3 position =
                            readAuthoritativeObjectPosition(
                                registry, *objectEntity,
                                *objectTransform);
                        const Fixed dx = runtime.unloadDestination.x -
                            position.x;
                        const Fixed dy = runtime.unloadDestination.y -
                            position.y;
                        const Fixed distance = Fixed::sqrt(
                            dx * dx + dy * dy);
                        const Fixed closeEnough{int32_t{3}};
                        const bool finished = distance <= closeEnough ||
                            runtime.pushOutsideDistancePerFrame <= Fixed{} ||
                            runtime.pushOutsideDistancePerFrame >= distance;
                        if (finished) {
                            position = runtime.unloadDestination;
                        } else {
                            position.x += dx / distance *
                                runtime.pushOutsideDistancePerFrame;
                            position.y += dy / distance *
                                runtime.pushOutsideDistancePerFrame;
                            position.z = runtime.unloadDestination.z;
                        }
                        writeAuthoritativeObjectPosition(
                            registry, *objectEntity, position);
                        if (finished) {
                            const uint64_t ignoreUntil = confirmedTick >
                                    std::numeric_limits<uint64_t>::max() -
                                        rules.logicFramesPerSecond
                                ? std::numeric_limits<uint64_t>::max()
                                : confirmedTick +
                                      rules.logicFramesPerSecond;
                            if (ObjectTemporaryCollisionIgnoreComponent* ignore =
                                    ecs::try_get<
                                        ObjectTemporaryCollisionIgnoreComponent>(
                                        registry, *objectEntity)) {
                                ignore->untilTick = std::max(
                                    ignore->untilTick, ignoreUntil);
                                ignore->other = INVALID_OBJECT_ID;
                            } else {
                                ecs::emplace<
                                    ObjectTemporaryCollisionIgnoreComponent>(
                                    registry, *objectEntity,
                                    ObjectTemporaryCollisionIgnoreComponent{
                                        .untilTick = ignoreUntil,
                                        .other = INVALID_OBJECT_ID,
                                    });
                            }
                            static_cast<void>(ObjectDisabledSystem::clear(
                                registry, *objectEntity,
                                ObjectDisabledReason::Held, confirmedTick));
                            static_cast<void>(ObjectStatusSystem::apply(
                                registry, *objectEntity,
                                {.clearMask = game::objectStatusBit(
                                     game::ObjectStatusFlag::Unselectable),
                                 .confirmedTick = confirmedTick}));
                            detail::projectRailedDockerMoving(
                                registry, *objectEntity, false,
                                confirmedTick);
                            if (runtime.dockWaiting07Valid) {
                                ObjectOrderQueueComponent* objectQueue =
                                    ecs::try_get<ObjectOrderQueueComponent>(
                                        registry, *objectEntity);
                                if (objectQueue) {
                                    const LogicFixedVec3 waiting =
                                        detail::transformLocalBridgePoint(
                                            transportPosition, transportYaw,
                                            runtime.dockWaiting07Local);
                                    if (objectQueue->orders.size() >=
                                        ObjectOrderQueueComponent::
                                            MaximumQueuedOrders) {
                                        objectQueue->orders.pop_back();
                                    }
                                    objectQueue->orders.insert(
                                        objectQueue->orders.begin(),
                                        ObjectOrderIntent{
                                            .kind = ObjectOrderKind::Move,
                                            .source =
                                                ObjectOrderSource::System,
                                            .issuedTick = confirmedTick,
                                            .sourceSequence = [&runtime]() {
                                                uint32_t sequence =
                                                    runtime.nextCommandSequence++;
                                                if (sequence == 0) {
                                                    sequence =
                                                        runtime.nextCommandSequence++;
                                                }
                                                if (runtime.nextCommandSequence ==
                                                    0) {
                                                    runtime.nextCommandSequence =
                                                        1;
                                                }
                                                return sequence;
                                            }(),
                                            .targetX = waiting.x,
                                            .targetY = waiting.y,
                                            .targetZ = waiting.z,
                                            .hasTargetPosition = true,
                                            .systemPurpose =
                                                ObjectOrderSystemPurpose::
                                                    ContainmentExit,
                                            .systemPurposeInstance =
                                                static_cast<uint32_t>(index),
                                        });
                                    ++objectQueue->revision;
                                }
                            }
                            runtime.unloadingObject = INVALID_OBJECT_ID;
                            ++runtime.revision;
                        }
                    }
                }
            }
            if (runtime.loadingOrUnloading &&
                !runtime.unloadingObject &&
                runtime.pendingUnloadObjects.empty() &&
                confirmedTick >= runtime.transitionEndsTick) {
                runtime.loadingOrUnloading = false;
                runtime.dockOpen = !runtime.inTransit;
                ++runtime.revision;
            }

            if (!aiRule || !transform || !queue) continue;
            if (!runtime.waypointDataLoaded) {
                detail::loadRailedTransportPaths(runtime, *aiRule, terrain);
                ++runtime.revision;
            }
            // RefCode initializes a railed transport by moving it to the End
            // waypoint of the closest authored path. That establishes
            // currentPath before the player can execute the next segment.
            // Keep the modern specialized owner and its ordinary Move handoff,
            // but preserve the same endpoint/segment ordering.
            if (runtime.currentPath >= runtime.pathCount &&
                runtime.pathCount != 0 && queue->orders.empty() &&
                !runtime.executeRequested) {
                size_t closestPath = runtime.pathCount;
                Fixed closestDistance = Fixed::from_raw(
                    std::numeric_limits<int64_t>::max());
                for (size_t pathIndex = 0;
                     pathIndex < runtime.pathCount; ++pathIndex) {
                    const ObjectRailedTransportWaypointPath& path =
                        runtime.paths[pathIndex];
                    const Fixed current = detail::distanceSquared(
                        transportPosition, path.endPosition);
                    if (current < closestDistance) {
                        closestPath = pathIndex;
                        closestDistance = current;
                    }
                }
                if (closestPath < runtime.pathCount &&
                    detail::replaceRailedTransportMove(
                        *queue, runtime, index,
                        owner ? owner->player : INVALID_PLAYER_ID,
                        runtime.paths[closestPath].endPosition,
                        confirmedTick)) {
                    runtime.currentPath = closestPath;
                    runtime.inTransit = true;
                    runtime.dockOpen = false;
                    ++runtime.revision;
                }
            }
            if (runtime.observedExternalOrderRevision !=
                queue->externalRevision) {
                runtime.observedExternalOrderRevision =
                    queue->externalRevision;
                if (!queue->orders.empty() &&
                    !detail::isRailedTransportOrder(queue->orders.front(), index)) {
                    runtime.inTransit = false;
                    runtime.executeRequested = false;
                    runtime.dockOpen = !runtime.loadingOrUnloading;
                    ++runtime.revision;
                }
            }
            if (!queue->orders.empty() &&
                !detail::isRailedTransportOrder(queue->orders.front(), index)) {
                continue;
            }
            if (runtime.executeRequested && runtime.pathCount == 0) {
                runtime.executeRequested = false;
                ++runtime.revision;
            }
            if (runtime.executeRequested && runtime.pathCount != 0 &&
                !runtime.loadingOrUnloading) {
                size_t nextPath = 0;
                if (runtime.currentPath < runtime.pathCount) {
                    nextPath = (runtime.currentPath + 1u) %
                        runtime.pathCount;
                } else {
                    size_t closestPath = runtime.pathCount;
                    Fixed closestDistance = Fixed::from_raw(
                        std::numeric_limits<int64_t>::max());
                    for (size_t pathIndex = 0;
                         pathIndex < runtime.pathCount; ++pathIndex) {
                        const ObjectRailedTransportWaypointPath& path =
                            runtime.paths[pathIndex];
                        const Fixed current = detail::distanceSquared(
                            transportPosition, path.endPosition);
                        if (current < closestDistance) {
                            closestPath = pathIndex;
                            closestDistance = current;
                        }
                    }
                    // The object is conceptually stationed at closestPath's
                    // End; EXECUTE_RAILED_TRANSPORT advances to the following
                    // authored path, matching ++m_currentPath in ZH.
                    nextPath = closestPath < runtime.pathCount
                        ? (closestPath + 1u) % runtime.pathCount
                        : 0u;
                }
                const bool started = detail::replaceRailedTransportMove(
                    *queue, runtime, index,
                    owner ? owner->player : INVALID_PLAYER_ID,
                    runtime.paths[nextPath].endPosition,
                    confirmedTick);
                runtime.executeRequested = false;
                if (started) {
                    runtime.currentPath = nextPath;
                    runtime.inTransit = true;
                    runtime.dockOpen = false;
                }
                ++runtime.revision;
            }
            if (!runtime.inTransit || runtime.currentPath >= runtime.pathCount) {
                continue;
            }
            const bool ownsFront = !queue->orders.empty() &&
                detail::isRailedTransportOrder(queue->orders.front(), index);
            const bool arrived = detail::distanceSquared(
                transportPosition,
                runtime.paths[runtime.currentPath].endPosition) <=
                    Fixed{int32_t{25}};
            if (arrived || !ownsFront) {
                if (arrived && ownsFront) {
                    queue->orders.erase(queue->orders.begin());
                    ++queue->revision;
                }
                runtime.inTransit = false;
                runtime.dockOpen = !runtime.loadingOrUnloading;
                ++runtime.revision;
            }
        }
    }
}

} // namespace engine
