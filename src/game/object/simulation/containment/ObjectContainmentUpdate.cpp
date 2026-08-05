#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/structure/ObjectBridge.h"

#include "core/container/string_utils.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/terrain/TerrainLogic.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include "game/object/simulation/containment/ObjectContainmentDetail.h"

namespace engine {
using namespace object_containment_detail;

void ObjectContainmentSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectBodyStateProjection>&
        bodyStateProjections,
    container::Vector<ObjectContainmentEvent>* containmentEvents,
    ObjectTransportEventStream* behaviorEvents,
    const PlayerRegistry* players,
    const game::terrain::TerrainLogic* terrain,
    const navigation::NavigationSystem* navigation) const {
    const uint32_t fps = rules.logicFramesPerSecond == 0 ? 30u
                                                         : rules.logicFramesPerSecond;
    using Candidate = ContainmentUpdateCandidate;
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectContainmentRuntimeComponent,
                                const ObjectContainmentComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id ||
            !lifecycle.entityFromIdIncludingPending(identity.id)) continue;
        candidates.push_back({identity.id, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.container < right.container;
              });

    // Tunnel networks are owner-scoped. Build their registration-ordered
    // entrance lists once, then reuse membership for tracker and heal logic.
    // Passenger snapshots are still rebuilt at each retail heal occurrence so
    // an earlier entrance update cannot make a later one observe stale contents.
    struct TunnelNetwork final {
        container::Vector<Candidate> entrances;
        ObjectId leader = INVALID_OBJECT_ID;
        const ObjectContainmentRule* shortestHealRule = nullptr;
        uint32_t shortestHealMilliseconds =
            std::numeric_limits<uint32_t>::max();
    };
    container::TreeMap<PlayerId, TunnelNetwork> tunnelNetworks;
    for (const Candidate& candidate : candidates) {
        if (!isCompletedContainmentEntrance(registry, candidate.entity))
            continue;
        const ObjectContainmentRuntimeComponent* runtime =
            ecs::try_get<ObjectContainmentRuntimeComponent>(
                registry, candidate.entity);
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
            registry, candidate.entity);
        if (!runtime || !runtime->plan || !owner || !owner->player ||
            std::none_of(runtime->plan->rules.begin(),
                         runtime->plan->rules.end(),
                         [](const ObjectContainmentRule& rule) {
                             return rule.kind ==
                                 ObjectContainmentKind::Tunnel;
                         })) {
            continue;
        }
        TunnelNetwork& network = tunnelNetworks[owner->player];
        network.entrances.push_back(candidate);
        for (const ObjectContainmentRule& rule : runtime->plan->rules) {
            if (rule.kind != ObjectContainmentKind::Tunnel ||
                rule.timeForFullHealMilliseconds >=
                    network.shortestHealMilliseconds) {
                continue;
            }
            network.shortestHealMilliseconds =
                rule.timeForFullHealMilliseconds;
            network.shortestHealRule = &rule;
        }
    }
    for (auto& [owner, network] : tunnelNetworks) {
        static_cast<void>(owner);
        std::sort(
            network.entrances.begin(), network.entrances.end(),
            [&](const Candidate& left, const Candidate& right) {
                const ObjectLifecycleComponent* leftLifecycle =
                    ecs::try_get<ObjectLifecycleComponent>(registry,
                                                            left.entity);
                const ObjectLifecycleComponent* rightLifecycle =
                    ecs::try_get<ObjectLifecycleComponent>(registry,
                                                            right.entity);
                const uint64_t leftTick = leftLifecycle
                    ? leftLifecycle->createdAtTick
                    : std::numeric_limits<uint64_t>::max();
                const uint64_t rightTick = rightLifecycle
                    ? rightLifecycle->createdAtTick
                    : std::numeric_limits<uint64_t>::max();
                if (leftTick != rightTick) return leftTick < rightTick;
                return left.container < right.container;
            });
        if (!network.entrances.empty())
            network.leader = network.entrances.front().container;
    }
    container::Vector<NetworkPassengerRecord> healRecords;
    for (const Candidate& candidate : candidates) {
        ObjectContainmentRuntimeComponent& runtime =
            ecs::get<ObjectContainmentRuntimeComponent>(registry, candidate.entity);
        const ObjectContainmentComponent& contents =
            ecs::get<ObjectContainmentComponent>(registry, candidate.entity);
        if (!runtime.plan) continue;
        const ObjectHealthComponent* containerHealth =
            ecs::try_get<ObjectHealthComponent>(registry,
                                                 candidate.entity);

        if (containerHealth && containerHealth->subdued) {
            // ActiveBody::onSubdualChange orders every contained passenger to
            // AI idle, independent of containment specialization.
            for (const ObjectContainedObjectRecord& record : contents.objects) {
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromId(record.object);
                ObjectOrderQueueComponent* queue = passenger
                    ? ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                               *passenger)
                    : nullptr;
                if (queue && !queue->orders.empty()) {
                    queue->orders.clear();
                    ++queue->revision;
                }
            }
        }

        // GarrisonContain rejects and evacuates ReallyDamaged buildings
        // unless the template explicitly remains garrisonable until death.
        // Snapshot the stable ObjectIds before Detach mutates the reverse
        // vector; no EnTT/storage order participates in the exit order.
        const ObjectKindOfComponent* containerKinds =
            ecs::try_get<ObjectKindOfComponent>(registry,
                                                 candidate.entity);
        const bool garrisonableUntilDestroyed =
            hasKind(containerKinds,
                    game::ObjectKindOf::GarrisonableUntilDestroyed);
        if (containerHealth &&
            containerHealth->damageState ==
                ObjectBodyDamageState::ReallyDamaged &&
            !garrisonableUntilDestroyed) {
            container::Vector<ObjectId> garrisonPassengers;
            for (const ObjectContainedObjectRecord& record :
                 contents.objects) {
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromId(record.object);
                const ObjectContainedByComponent* edge = passenger
                    ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                                *passenger)
                    : nullptr;
                if (!edge || edge->containmentRuleIndex >=
                        runtime.plan->rules.size() ||
                    runtime.plan->rules[
                        edge->containmentRuleIndex].kind !=
                        ObjectContainmentKind::Garrison) {
                    continue;
                }
                garrisonPassengers.push_back(record.object);
            }
            container::Vector<ObjectContainmentEvent> ignoredEvents;
            container::Vector<ObjectContainmentEvent>& events =
                containmentEvents ? *containmentEvents : ignoredEvents;
            for (const ObjectId passenger : garrisonPassengers) {
                static_cast<void>(requestDetach(
                    registry, lifecycle,
                    {.kind = ObjectContainmentRequestKind::Detach,
                     .container = candidate.container,
                     .object = passenger,
                     .confirmedTick = confirmedTick,
                     .force = true},
                    events));
            }
        }

        // OverlordContain/HelixContain use ActiveBody::setDamageState on the
        // visible portable structure. Publish a low-level Body projection;
        // this must not manufacture DamageInfo, score, retaliation, or FX.
        // Rubble remains owned by the ordered terminal containment lifecycle.
        if (containerHealth && !containerHealth->effectivelyDead &&
            containerHealth->damageState != ObjectBodyDamageState::Rubble) {
            for (const ObjectContainedObjectRecord& record :
                 contents.objects) {
                if (!record.object || !record.destroyWithContainer) continue;
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromId(record.object);
                const ObjectContainedByComponent* edge = passenger
                    ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                                *passenger)
                    : nullptr;
                const ObjectHealthComponent* passengerHealth = passenger
                    ? ecs::try_get<ObjectHealthComponent>(registry,
                                                           *passenger)
                    : nullptr;
                if (!edge || edge->enclosing || !passengerHealth ||
                    passengerHealth->effectivelyDead ||
                    edge->containmentRuleIndex >=
                        runtime.plan->rules.size()) {
                    continue;
                }
                const ObjectContainmentRule& portableRule =
                    runtime.plan->rules[edge->containmentRuleIndex];
                if (portableRule.kind != ObjectContainmentKind::Overlord &&
                    portableRule.kind != ObjectContainmentKind::Helix) {
                    continue;
                }
                if (passengerHealth->damageState ==
                    containerHealth->damageState) {
                    continue;
                }
                bodyStateProjections.push_back({
                    .object = record.object,
                    .source = candidate.container,
                    .state = containerHealth->damageState,
                    .authoredOrder = portableRule.authoredOrder,
                    .confirmedTick = confirmedTick,
                });
            }
        }

        const bool ownsTunnel = std::any_of(
            runtime.plan->rules.begin(), runtime.plan->rules.end(),
            [](const ObjectContainmentRule& rule) {
                return rule.kind == ObjectContainmentKind::Tunnel;
            });
        const TunnelNetwork* tunnelNetwork = nullptr;
        if (ownsTunnel &&
            isCompletedContainmentEntrance(registry, candidate.entity)) {
            const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
                registry, candidate.entity);
            const auto network = owner && owner->player
                ? tunnelNetworks.find(owner->player)
                : tunnelNetworks.end();
            if (network != tunnelNetworks.end())
                tunnelNetwork = &network->second;
            // The candidate list is ObjectId-sorted, so one stable leader
            // publishes TunnelTracker's shared recent-nemesis state exactly
            // once. Every entrance receives the same value for O(1) AI reads.
            if (tunnelNetwork && !tunnelNetwork->entrances.empty() &&
                tunnelNetwork->leader == candidate.container) {
                const container::Vector<Candidate>& networkEntrances =
                    tunnelNetwork->entrances;
                const OwnerComponent* leaderOwner =
                    ecs::try_get<OwnerComponent>(registry,
                                                 candidate.entity);
                const PlayerId networkOwner = leaderOwner
                    ? leaderOwner->player : INVALID_PLAYER_ID;
                ObjectId nemesis = INVALID_OBJECT_ID;
                uint64_t observedTick = 0;
                uint64_t expiresTick = 0;
                for (const Candidate& entrance : networkEntrances) {
                    const ObjectTunnelNetworkCombatHandoffComponent* handoff =
                        ecs::try_get<
                            ObjectTunnelNetworkCombatHandoffComponent>(
                                registry, entrance.entity);
                    if (!handoff || handoff->networkOwner != networkOwner ||
                        !handoff->recentNemesis ||
                        confirmedTick > handoff->expiresTick ||
                        !liveTunnelNemesis(
                            registry, lifecycle,
                            handoff->recentNemesis)) {
                        continue;
                    }
                    nemesis = handoff->recentNemesis;
                    observedTick = handoff->observedTick;
                    expiresTick = handoff->expiresTick;
                    break;
                }

                for (const Candidate& entrance : networkEntrances) {
                    const ObjectHealthComponent* health =
                        ecs::try_get<ObjectHealthComponent>(
                            registry, entrance.entity);
                    if (!health || !health->hasLastDamageInfo ||
                        !health->lastDamageSource ||
                        health->lastDamageTick > confirmedTick ||
                        confirmedTick - health->lastDamageTick >= fps ||
                        !liveTunnelNemesis(
                            registry, lifecycle,
                            health->lastDamageSource)) {
                        continue;
                    }
                    const std::optional<ecs::entity> attacker =
                        lifecycle.entityFromId(health->lastDamageSource);
                    if (!attacker) continue;
                    const OwnerComponent* tunnelOwner =
                        ecs::try_get<OwnerComponent>(registry,
                                                     entrance.entity);
                    const OwnerComponent* attackerOwner =
                        ecs::try_get<OwnerComponent>(registry, *attacker);
                    bool enemy = false;
                    if (tunnelOwner && attackerOwner && tunnelOwner->player &&
                        attackerOwner->player) {
                        enemy = players
                            ? players->relationship(tunnelOwner->player,
                                                    attackerOwner->player) ==
                                PlayerRelationship::Enemies
                            : tunnelOwner->player != attackerOwner->player &&
                                tunnelOwner->player != NEUTRAL_PLAYER_ID &&
                                attackerOwner->player != NEUTRAL_PLAYER_ID;
                    }
                    if (!enemy || (nemesis && nemesis !=
                                             health->lastDamageSource)) {
                        continue;
                    }
                    nemesis = health->lastDamageSource;
                    observedTick = confirmedTick;
                    expiresTick = saturatingAddTicks(
                        confirmedTick, static_cast<uint64_t>(fps) * 4u);
                    // TunnelTracker keeps the first live enemy and only the
                    // same enemy refreshes it until expiry.
                    break;
                }

                for (const Candidate& entrance : networkEntrances) {
                    ObjectTunnelNetworkCombatHandoffComponent* handoff =
                        ecs::try_get<
                            ObjectTunnelNetworkCombatHandoffComponent>(
                                registry, entrance.entity);
                    if (!handoff) {
                        handoff = &ecs::emplace<
                            ObjectTunnelNetworkCombatHandoffComponent>(
                                registry, entrance.entity);
                    }
                    if (handoff->networkOwner == networkOwner &&
                        handoff->recentNemesis == nemesis &&
                        handoff->observedTick == observedTick &&
                        handoff->expiresTick == expiresTick) {
                        continue;
                    }
                    handoff->networkOwner = networkOwner;
                    handoff->recentNemesis = nemesis;
                    handoff->observedTick = observedTick;
                    handoff->expiresTick = expiresTick;
                    ++handoff->revision;
                    if (handoff->revision == 0) ++handoff->revision;
                }
            }
        }
        if (runtime.doorCloseTick != 0 &&
            confirmedTick >= runtime.doorCloseTick) {
            runtime.doorCloseTick = 0;
            projectContainmentDoorTransition(registry, candidate.entity, false,
                                             confirmedTick);
        }
        for (size_t ruleIndex = 0; ruleIndex < runtime.plan->rules.size();
             ++ruleIndex) {
            const ObjectContainmentRule& rule = runtime.plan->rules[ruleIndex];
            if (rule.kind != ObjectContainmentKind::Parachute || !terrain)
                continue;
            // A chute may itself still be enclosed by the aircraft that
            // created it.  RefCode leaves ParachuteContain asleep until that
            // outer container releases it.
            if (isObjectDisabledBy(
                    registry, candidate.entity, ObjectDisabledReason::Held,
                    confirmedTick)) {
                continue;
            }
            ObjectId riderId = INVALID_OBJECT_ID;
            ecs::entity riderEntity = ecs::null;
            for (const ObjectContainedObjectRecord& record : contents.objects) {
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromId(record.object);
                const ObjectContainedByComponent* edge = passenger
                    ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                                *passenger)
                    : nullptr;
                if (edge && edge->containmentRuleIndex == ruleIndex) {
                    riderId = record.object;
                    riderEntity = *passenger;
                    break;
                }
            }
            if (!riderId) {
                static_cast<void>(lifecycle.requestDestroy(
                    candidate.container, ObjectDestroyReason::System,
                    confirmedTick));
                continue;
            }
            TransformComponent* transform =
                ecs::try_get<TransformComponent>(registry, candidate.entity);
            if (!transform) continue;
            LogicFixedVec3 currentPosition =
                readAuthoritativeObjectPosition(
                    registry, candidate.entity, *transform);
            LogicFixedVec3 sampledPosition = currentPosition;
            const math::q32_32 currentZ = currentPosition.z;
            game::terrain::TerrainPathfindLayerId pathfindLayer =
                terrain->highestPathfindLayerAtRaw(
                    sampledPosition.x.raw(), sampledPosition.y.raw(),
                    sampledPosition.z.raw());
            int64_t layerHeightRaw = terrain->pathfindLayerHeightRawAt(
                pathfindLayer, sampledPosition.x.raw(), sampledPosition.y.raw())
                .value_or(terrain->groundHeightRaw(
                    sampledPosition.x.raw(), sampledPosition.y.raw()));
            ObjectTerrainLayerComponent* parachuteLayer =
                ecs::try_get<ObjectTerrainLayerComponent>(
                    registry, candidate.entity);
            if (!parachuteLayer) {
                parachuteLayer = &ecs::emplace<
                    ObjectTerrainLayerComponent>(registry, candidate.entity);
            }
            static_cast<void>(parachuteLayer->assign(
                pathfindLayer, confirmedTick));
            ObjectTerrainLayerComponent* riderLayer =
                ecs::try_get<ObjectTerrainLayerComponent>(
                    registry, riderEntity);
            if (!riderLayer) {
                riderLayer = &ecs::emplace<ObjectTerrainLayerComponent>(
                    registry, riderEntity);
            }
            static_cast<void>(riderLayer->assign(
                pathfindLayer, confirmedTick));
            math::q32_32 groundZ = math::q32_32::from_raw(layerHeightRaw);
            if (runtime.parachuteHasLandingTarget) {
                const math::q32_32 span =
                    runtime.parachuteOverrideStartZ -
                    runtime.parachuteLandingZ;
                math::q32_32 progress = span > math::q32_32{}
                    ? (runtime.parachuteOverrideStartZ - currentZ) / span
                    : math::q32_32{int32_t{1}};
                progress = math::q32_32::max(
                    math::q32_32{}, math::q32_32::min(
                        math::q32_32{int32_t{1}}, progress));
                const math::q32_32 x =
                    runtime.parachuteOverrideStartX +
                    (runtime.parachuteLandingX -
                     runtime.parachuteOverrideStartX) * progress;
                const math::q32_32 y =
                    runtime.parachuteOverrideStartY +
                    (runtime.parachuteLandingY -
                     runtime.parachuteOverrideStartY) * progress;
                writeAuthoritativeObjectPosition(
                    registry, candidate.entity,
                    LogicFixedVec3{x, y, currentZ});
                currentPosition.x = x;
                currentPosition.y = y;
                sampledPosition = {x, y, currentZ};
                pathfindLayer = terrain->highestPathfindLayerAtRaw(
                    sampledPosition.x.raw(), sampledPosition.y.raw(),
                    sampledPosition.z.raw());
                layerHeightRaw = terrain->pathfindLayerHeightRawAt(
                    pathfindLayer, sampledPosition.x.raw(),
                    sampledPosition.y.raw()).value_or(
                        terrain->groundHeightRaw(
                            sampledPosition.x.raw(), sampledPosition.y.raw()));
                static_cast<void>(parachuteLayer->assign(
                    pathfindLayer, confirmedTick));
                static_cast<void>(riderLayer->assign(
                    pathfindLayer, confirmedTick));
                // The override's Z guides horizontal convergence only.  As
                // in RefCode, actual contact is owned by the current terrain
                // or bridge layer rather than an authored destination Z.
                groundZ = math::q32_32::from_raw(layerHeightRaw);
                // synchronizeTransforms() ran before this ParachuteContain
                // update.  Its deterministic landing correction changes the
                // host X/Y here, so publish the corrected rider pose before a
                // possible same-frame detach at ground contact.
                synchronizeOne(registry, riderEntity, candidate.entity);
            }
            if (!runtime.parachuteHasStartZ) {
                runtime.parachuteStartZ = currentZ;
                if (runtime.parachuteStartZ - groundZ <
                        rule.parachuteOpenDistance * math::q32_32{2}) {
                    runtime.parachuteStartZ = groundZ +
                        rule.parachuteOpenDistance * math::q32_32{2};
                }
                runtime.parachuteHasStartZ = true;
            }
            if (!runtime.parachuteOpened &&
                math::q32_32::abs(runtime.parachuteStartZ - currentZ) >=
                    rule.parachuteOpenDistance) {
                runtime.parachuteOpened = true;
                const game::ObjectStatusMask noCollisions =
                    game::objectStatusBit(
                        game::ObjectStatusFlag::NoCollisions);
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, candidate.entity,
                    {.clearMask = noCollisions,
                     .confirmedTick = confirmedTick}));
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, riderEntity,
                    {.clearMask = noCollisions,
                     .confirmedTick = confirmedTick}));
                if (RenderModelComponent* parachuteRender =
                        ecs::try_get<RenderModelComponent>(
                            registry, candidate.entity)) {
                    parachuteRender->hidden = false;
                    parachuteRender->modelConditionFlags.clear(
                        game::modelConditionMaskOf(game::ModelConditionFlag::FreeFall));
                    const game::ModelConditionMask opened =
                        game::modelConditionMaskOf(game::ModelConditionFlag::Parachuting);
                    for (size_t word = 0; word < opened.words.size(); ++word)
                        parachuteRender->modelConditionFlags.words[word] |=
                            opened.words[word];
                }
                if (RenderModelComponent* riderRender =
                        ecs::try_get<RenderModelComponent>(registry,
                                                           riderEntity)) {
                    riderRender->modelConditionFlags.clear(
                        game::modelConditionMaskOf(game::ModelConditionFlag::FreeFall));
                    const game::ModelConditionMask opened =
                        game::modelConditionMaskOf(game::ModelConditionFlag::Parachuting);
                    for (size_t word = 0; word < opened.words.size(); ++word)
                        riderRender->modelConditionFlags.words[word] |=
                            opened.words[word];
                }
                if (!rule.parachuteOpenSound.empty() && behaviorEvents) {
                    pushTransportPresentationEvent(
                        *behaviorEvents, ObjectTransportAudioPresentation{
                        .object = riderId,
                        .eventName = rule.parachuteOpenSound,
                        .x = currentPosition.x,
                        .y = currentPosition.y,
                        .z = currentZ,
                    });
                }
                if (!runtime.parachuteHasLandingTarget && navigation &&
                    navigation->isInitialized()) {
                    navigation::NavigationLayerId navigationLayer;
                    if (navigation::tryNavigationLayerFromTerrainPathfindLayer(
                            pathfindLayer, navigationLayer)) {
                        const navigation::NavigationDestinationAdjustmentResult
                            adjusted = navigation::adjustNavigationDestination(
                                navigation->layers(),
                                {.desired = {
                                     currentPosition.x.raw(),
                                     currentPosition.y.raw(),
                                     currentZ.raw()},
                                 .layer = navigationLayer,
                                 .movementMask =
                                     navigation::NavigationMovement::Ground,
                                 .clearance = [&]() noexcept {
                                     const ObjectGeometryComponent* geometry =
                                         ecs::try_get<ObjectGeometryComponent>(
                                             registry, riderEntity);
                                     return geometry
                                         ? navigation::clearanceClassForRadiusRaw(
                                               math::q32_32::max(
                                                   math::q32_32{},
                                                   geometry->
                                                       boundingCircleRadiusFixed)
                                                   .raw(),
                                               navigation->grid().transform()
                                                   .cellSizeRaw)
                                         : navigation::NavigationClearanceClass::
                                               Centered1x1;
                                 }(),
                                 .allowAdjustment = true});
                        if (adjusted.accepted()) {
                            const math::q32_32 adjustedX =
                                math::q32_32::from_raw(
                                    adjusted.position.xRaw);
                            const math::q32_32 adjustedY =
                                math::q32_32::from_raw(
                                    adjusted.position.yRaw);
                            const math::q32_32 dx =
                                adjustedX - currentPosition.x;
                            const math::q32_32 dy =
                                adjustedY - currentPosition.y;
                            const math::q32_32 maximumDistance{100};
                            if (dx * dx + dy * dy <=
                                maximumDistance * maximumDistance) {
                                runtime.parachuteHasLandingTarget = true;
                                runtime.parachuteOverrideStartX =
                                    currentPosition.x;
                                runtime.parachuteOverrideStartY =
                                    currentPosition.y;
                                runtime.parachuteOverrideStartZ = currentZ;
                                runtime.parachuteLandingX = adjustedX;
                                runtime.parachuteLandingY = adjustedY;
                                runtime.parachuteLandingZ =
                                    math::q32_32::from_raw(
                                        adjusted.position.zRaw);
                            }
                        }
                    }
                }
            }
            if (runtime.parachuteOpened) {
                const math::q32_32 altitude =
                    math::q32_32::max(math::q32_32{}, currentZ - groundZ);
                const math::q32_32 damping = altitude <= math::q32_32{20}
                    ? rule.lowAltitudeDamping : math::q32_32{};
                runtime.parachutePitchRate -=
                    runtime.parachutePitch * math::q32_32::from_fraction(1, 20) +
                    runtime.parachutePitchRate * damping;
                runtime.parachuteRollRate -=
                    runtime.parachuteRoll * math::q32_32::from_fraction(1, 20) +
                    runtime.parachuteRollRate * damping;
                runtime.parachutePitch += runtime.parachutePitchRate;
                runtime.parachuteRoll += runtime.parachuteRollRate;
            }
            const std::optional<int64_t> water = terrain->waterHeightRawAt(
                sampledPosition.x.raw(), sampledPosition.y.raw());
            const bool waterLanding =
                pathfindLayer == game::terrain::kGroundPathfindLayer &&
                water && sampledPosition.z <= math::q32_32::from_raw(*water) +
                    rule.killWhenLandingInWaterSlop;
            // RefCode explicitly forces the water collision because the
            // ordinary ground collider can miss it.  Do not wait for the
            // parachute to sink to terrain height beneath the water plane.
            if (!waterLanding &&
                currentZ > groundZ + math::q32_32::from_fraction(1, 20)) {
                continue;
            }
            const ObjectHealthComponent* riderHealth =
                ecs::try_get<ObjectHealthComponent>(registry, riderEntity);
            const ObjectMapStatusComponent* riderMapStatus =
                ecs::try_get<ObjectMapStatusComponent>(registry,
                                                        riderEntity);
            bool illegalLanding =
                (riderMapStatus && riderMapStatus->offMap) ||
                !terrain->map().isInsidePlayableRaw(
                    sampledPosition.x.raw(), sampledPosition.y.raw()) ||
                (pathfindLayer ==
                     game::terrain::kGroundPathfindLayer &&
                 (waterLanding || terrain->map().isCliffCellRaw(
                     sampledPosition.x.raw(), sampledPosition.y.raw())));
            if (!illegalLanding && navigation &&
                navigation->isInitialized()) {
                navigation::NavigationLayerId navigationLayer;
                if (!navigation::tryNavigationLayerFromTerrainPathfindLayer(
                        pathfindLayer, navigationLayer)) {
                    illegalLanding = true;
                } else if (const navigation::NavigationGrid* grid =
                               navigation->staticLayers().find(
                                   navigationLayer)) {
                    const navigation::NavigationCellId cell = grid->cellAt({
                        sampledPosition.x.raw(),
                        sampledPosition.y.raw(),
                        currentZ.raw(),
                    });
                    illegalLanding = !grid->traversable(
                        cell, navigation::NavigationMovement::Ground,
                        navigationLayer);
                } else {
                    illegalLanding = true;
                }
            }
            if (illegalLanding) {
                outDamage.push_back({
                    .target = riderId,
                    .source = INVALID_OBJECT_ID,
                    .sourceSequence = rule.authoredOrder,
                    .submissionOrdinal = reserveTransportGameplayOrdinal(
                        nextGameplaySubmissionOrdinal),
                    .amount = riderHealth
                        ? riderHealth->maximumFixed
                        : math::q32_32{},
                    .damageType = waterLanding
                        ? game::DamageType::WATER
                        : game::DamageType::UNRESISTABLE,
                    .deathType = waterLanding
                        ? game::DeathType::FLOODED
                        : game::DeathType::NORMAL,
                    .forceKill = true,
                    .confirmedTick = confirmedTick,
                });
            }
            container::Vector<ObjectContainmentEvent> ignoredEvents;
            container::Vector<ObjectContainmentEvent>& events =
                containmentEvents ? *containmentEvents : ignoredEvents;
            const ObjectProducerComponent* chuteProducer =
                ecs::try_get<ObjectProducerComponent>(registry,
                                                       candidate.entity);
            static_cast<void>(requestDetach(
                registry, lifecycle,
                {.kind = ObjectContainmentRequestKind::Detach,
                 .container = candidate.container,
                 .object = riderId,
                 .confirmedTick = confirmedTick,
                 .force = true,
                 .parachuteLandingTransport = chuteProducer
                     ? chuteProducer->producer : INVALID_OBJECT_ID}, events));
            static_cast<void>(lifecycle.requestDestroy(
                candidate.container, ObjectDestroyReason::System,
                confirmedTick));
        }
        if (runtime.riderScuttleTick != 0 &&
            confirmedTick >= runtime.riderScuttleTick) {
            const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(registry,
                                                     candidate.entity);
            outDamage.push_back({
                .target = candidate.container,
                .source = candidate.container,
                .submissionOrdinal = reserveTransportGameplayOrdinal(
                    nextGameplaySubmissionOrdinal),
                .amount = health
                    ? health->maximumFixed
                    : math::q32_32{},
                .damageType = game::DamageType::UNRESISTABLE,
                .deathType = game::DeathType::TOPPLED,
                .forceKill = true,
                .confirmedTick = confirmedTick,
            });
            runtime.riderScuttleTick = 0;
        }
        container::Vector<ObjectId> completedHealPassengers;
        for (size_t ruleIndex = 0; ruleIndex < runtime.plan->rules.size();
             ++ruleIndex) {
            const ObjectContainmentRule& rule = runtime.plan->rules[ruleIndex];
            if (containerHealth && containerHealth->effectivelyDead) break;
            const ObjectContainmentRule* healRule = &rule;
            ObjectId healSource = candidate.container;
            healRecords.clear();

            if (rule.kind == ObjectContainmentKind::Tunnel) {
                if (!isCompletedContainmentEntrance(registry,
                                                     candidate.entity) ||
                    !tunnelNetwork) {
                    continue;
                }
                if (rules.preserveTunnelHealStacking) {
                    // Retail calls TunnelTracker::healObjects once from every
                    // entrance update, using that entrance's authored period.
                    collectNetworkPassengers(
                        registry, lifecycle, tunnelNetwork->entrances,
                        ObjectContainmentKind::Tunnel, healRecords);
                } else {
                    // The corrected RefCode branch caches the shortest period
                    // across the network and invokes one heal pass. Select the
                    // lowest ObjectId entrance as the deterministic producer.
                    const auto firstTunnelRule = std::find_if(
                        runtime.plan->rules.begin(),
                        runtime.plan->rules.end(),
                        [](const ObjectContainmentRule& candidateRule) {
                            return candidateRule.kind ==
                                ObjectContainmentKind::Tunnel;
                        });
                    if (!tunnelNetwork->leader ||
                        candidate.container != tunnelNetwork->leader ||
                        firstTunnelRule == runtime.plan->rules.end() ||
                        static_cast<size_t>(firstTunnelRule -
                            runtime.plan->rules.begin()) != ruleIndex ||
                        !tunnelNetwork->shortestHealRule) {
                        continue;
                    }
                    healRule = tunnelNetwork->shortestHealRule;
                    healSource = tunnelNetwork->leader;
                    collectNetworkPassengers(
                        registry, lifecycle, tunnelNetwork->entrances,
                        ObjectContainmentKind::Tunnel, healRecords);
                }
            } else {
                healRecords.reserve(contents.objects.size());
                for (const ObjectContainedObjectRecord& record :
                     contents.objects) {
                    const std::optional<ecs::entity> passenger =
                        lifecycle.entityFromId(record.object);
                    const ObjectContainedByComponent* edge = passenger
                        ? ecs::try_get<ObjectContainedByComponent>(
                            registry, *passenger)
                        : nullptr;
                    if (!edge || edge->container != candidate.container ||
                        edge->containmentRuleIndex != ruleIndex) {
                        continue;
                    }
                    healRecords.push_back(
                        {record, candidate.container, candidate.entity,
                         static_cast<uint32_t>(ruleIndex)});
                }
            }

            const bool durationHeal =
                healRule->kind == ObjectContainmentKind::Tunnel ||
                (healRule->kind == ObjectContainmentKind::Garrison &&
                 healRule->healGarrisonObjects);
            if (healRule->kind != ObjectContainmentKind::Heal &&
                healRule->healAmountPerSecond == math::q32_32{} &&
                (!durationHeal ||
                 healRule->timeForFullHealMilliseconds == 0)) {
                continue;
            }
            for (const NetworkPassengerRecord& healRecord : healRecords) {
                const ObjectContainedObjectRecord& record = healRecord.record;
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromId(record.object);
                if (!passenger) continue;
                ObjectHealthComponent* health =
                    ecs::try_get<ObjectHealthComponent>(registry, *passenger);
                const ObjectContainedByComponent* edge =
                    ecs::try_get<ObjectContainedByComponent>(registry,
                                                              *passenger);
                if (!health || health->effectivelyDead || !edge) {
                    continue;
                }

                const bool timedHealContain =
                    healRule->kind == ObjectContainmentKind::Heal;
                const uint64_t fullHealTicks =
                    (timedHealContain || durationHeal)
                    ? millisecondsToTicks(
                        healRule->timeForFullHealMilliseconds, fps)
                    : 0;
                const uint64_t enteredTick = edge->confirmedEnteredTick;
                const bool fullHealDeadlineReached =
                    (timedHealContain || durationHeal) &&
                    fullHealTicks != 0 && confirmedTick >= enteredTick &&
                    confirmedTick - enteredTick >= fullHealTicks;

                math::q32_32 amount{};
                if (fullHealDeadlineReached) {
                    // RefCode attempts a maximum-health heal before reserving
                    // the exit door. The Body barrier performs the final
                    // clamp, so this also closes any fixed-point remainder.
                    amount = health->maximumFixed;
                    if (timedHealContain)
                        completedHealPassengers.push_back(record.object);
                } else if (health->currentFixed < health->maximumFixed) {
                    amount = healRule->healAmountPerSecond > math::q32_32{}
                        ? healRule->healAmountPerSecond /
                            math::q32_32{static_cast<int32_t>(fps)}
                        : healRule->healAmountPerSecond < math::q32_32{}
                            ? health->maximumFixed *
                                (-healRule->healAmountPerSecond) /
                                math::q32_32{static_cast<int32_t>(fps)}
                            : durationHeal && fullHealTicks != 0
                                ? health->maximumFixed /
                                  math::q32_32{static_cast<int32_t>(
                                      std::min<uint64_t>(
                                          fullHealTicks,
                                          static_cast<uint64_t>(
                                              std::numeric_limits<int32_t>::max())))}
                                : math::q32_32{};
                }
                if (amount <= math::q32_32{} && timedHealContain &&
                    !fullHealDeadlineReached && fullHealTicks != 0) {
                    amount = health->maximumFixed /
                        math::q32_32{static_cast<int32_t>(std::min<uint64_t>(
                            fullHealTicks,
                            static_cast<uint64_t>(
                                std::numeric_limits<int32_t>::max())))};
                }
                if (amount <= math::q32_32{}) continue;
                outDamage.push_back({
                    .target = record.object,
                    .source = healSource,
                    .sourceSequence = healRule->authoredOrder,
                    .submissionOrdinal = reserveTransportGameplayOrdinal(
                        nextGameplaySubmissionOrdinal),
                    .amount = amount,
                    .damageType = game::DamageType::HEALING,
                    .deathType = game::DeathType::NORMAL,
                    .confirmedTick = confirmedTick,
                });
            }
        }
        if (!completedHealPassengers.empty()) {
            container::Vector<ObjectContainmentEvent> ignoredEvents;
            container::Vector<ObjectContainmentEvent>& events =
                containmentEvents ? *containmentEvents : ignoredEvents;
            for (const ObjectId passenger : completedHealPassengers) {
                // requestDetach is the shared door reservation/exit-path
                // transaction. A busy door rejects this attempt and leaves the
                // edge intact, so HealContain retries on the next fixed tick as
                // the original module does.
                static_cast<void>(requestDetach(
                    registry, lifecycle,
                    {.kind = ObjectContainmentRequestKind::Detach,
                     .container = candidate.container,
                     .object = passenger,
                     .confirmedTick = confirmedTick},
                    events));
            }
        }

        updateTransportBehaviors(
            *this, registry, lifecycle, rules, confirmedTick, outDamage,
            candidate,
            runtime, contents, containmentEvents, behaviorEvents,
            nextGameplaySubmissionOrdinal, players, terrain, fps);
    }
}

void ObjectContainmentSystem::synchronizeTransforms(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot* content) const {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
        ObjectId container = INVALID_OBJECT_ID;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectContainedByComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectContainedByComponent& edge =
            view.template get<const ObjectContainedByComponent>(entity);
        if (!identity.id || !edge.container ||
            !edge.followsContainerTransform ||
            !lifecycle.entityFromId(identity.id)) continue;
        candidates.push_back({identity.id, entity, edge.container});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });
    for (const Candidate& candidate : candidates) {
        const std::optional<ecs::entity> container =
            lifecycle.entityFromId(candidate.container);
        if (container)
            synchronizeOne(
                registry, candidate.entity, *container, content);
    }
}


} // namespace engine
