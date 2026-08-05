#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/structure/ObjectBridge.h"

#include "core/container/string_utils.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
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

[[nodiscard]] static uint64_t recoveryTicksForTurnRate(
    uint32_t framesPerSecond, math::q32_32 turnRate) noexcept {
    if (turnRate <= math::q32_32{}) return framesPerSecond;
    const math::q32_32 ticks =
        math::q32_32{static_cast<int32_t>(framesPerSecond)} / turnRate;
    const int64_t whole = ticks.raw() >> 32u;
    return std::max<uint64_t>(
        1u, static_cast<uint64_t>(whole) +
            ((ticks.raw() & 0xffffffffll) != 0 ? 1u : 0u));
}

[[nodiscard]] static bool battleBusDeathBlocksContinuation(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity entity, ObjectId object) noexcept {
    if (lifecycle.isPendingDestroy(object)) return true;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    return health && (health->effectivelyDead || health->terminalDeathIssued);
}

size_t ObjectContainmentSystem::fanoutDirectAttackOrder(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId container, const ObjectOrderIntent& order, bool queued,
    uint64_t confirmedTick) {
    const bool strategicAI = order.source == ObjectOrderSource::System &&
        order.systemPurpose == ObjectOrderSystemPurpose::StrategicAI;
    if (order.kind != ObjectOrderKind::Attack ||
        (order.source != ObjectOrderSource::Player &&
         order.source != ObjectOrderSource::Script && !strategicAI)) {
        return 0;
    }

    size_t admitted = 0;
    const auto fanout = [&](auto&& self, ObjectId host) -> void {
        const std::optional<ecs::entity> hostEntity =
            lifecycle.entityFromId(host);
        if (!hostEntity) return;
        const ObjectContainmentRuntimeComponent* runtime =
            ecs::try_get<ObjectContainmentRuntimeComponent>(
                registry, *hostEntity);
        const ObjectContainmentComponent* contents =
            ecs::try_get<ObjectContainmentComponent>(registry, *hostEntity);
        if (!runtime || !runtime->plan || !contents ||
            !contents->passengersAllowedToFire) {
            return;
        }
        const bool transportAi = std::any_of(
            runtime->plan->behaviorRules.begin(),
            runtime->plan->behaviorRules.end(),
            [](const ObjectTransportBehaviorRule& rule) {
                return rule.kind == ObjectTransportBehaviorKind::TransportAI;
            });
        if (!transportAi) return;

        // ObjectContainmentComponent owns stable ObjectId order. Recursive
        // child-first admission matches TransportAIUpdate calling each
        // passenger's AI before its own base privateAttack* implementation.
        for (const ObjectContainedObjectRecord& record : contents->objects) {
            const std::optional<ecs::entity> passenger =
                lifecycle.entityFromId(record.object);
            if (!passenger) continue;
            const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(registry, *passenger);
            if (!type || !type->archetype ||
                !type->archetype->hasAiUpdate) {
                continue;
            }
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(registry, *passenger);
            const bool portableStructure = kinds && game::objectHasKind(
                kinds->mask, game::ObjectKindOf::PortableStructure);
            if (portableStructure &&
                (isObjectDisabledBy(
                     registry, *passenger, ObjectDisabledReason::Hacked,
                     confirmedTick) ||
                 isObjectDisabledBy(
                     registry, *passenger, ObjectDisabledReason::Emp,
                     confirmedTick) ||
                 isObjectDisabledBy(
                     registry, *passenger, ObjectDisabledReason::Subdued,
                     confirmedTick) ||
                 isObjectDisabledBy(
                     registry, *passenger, ObjectDisabledReason::Paralyzed,
                     confirmedTick))) {
                continue;
            }

            self(self, record.object);
            ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(registry, *passenger);
            if (queued && queue && queue->orders.size() >=
                    ObjectOrderQueueComponent::MaximumQueuedOrders) {
                continue;
            }
            if (!queue) {
                queue = &ecs::emplace<ObjectOrderQueueComponent>(
                    registry, *passenger);
            }
            if (!queued) queue->orders.clear();
            queue->orders.push_back(order);
            ++queue->revision;
            if (!strategicAI) {
                ++queue->externalRevision;
                if (queue->externalRevision == 0) ++queue->externalRevision;
                if (!queued) {
                    queue->replacementExternalRevision =
                        queue->externalRevision;
                    queue->replacementExternalSource = order.source;
                    queue->replacementExternalKind = order.kind;
                }
            }
            ++admitted;
        }
    };
    fanout(fanout, container);
    return admitted;
}

bool ObjectContainmentSystem::beginHijackerRelease(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId hijacker, uint32_t ruleIndex, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> object =
        lifecycle.entityFromIdIncludingPending(hijacker);
    if (!object) return false;
    ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *object);
    if (!runtime || !runtime->plan ||
        ruleIndex >= runtime->behaviorStates.size() ||
        ruleIndex >= runtime->plan->behaviorRules.size() ||
        runtime->plan->behaviorRules[ruleIndex].kind !=
            ObjectTransportBehaviorKind::Hijacker) {
        return false;
    }
    ObjectTransportBehaviorState& state =
        runtime->behaviorStates[ruleIndex];
    if (state.phase != ObjectTransportBehaviorPhase::HijackerAttached) {
        return false;
    }
    setHijackerStatus(registry, *object, false, confirmedTick);
    if (ObjectMapStatusComponent* mapStatus =
            ecs::try_get<ObjectMapStatusComponent>(registry, *object)) {
        if (state.hadMapStatus) mapStatus->offMap = state.previousOffMap;
        else ecs::remove<ObjectMapStatusComponent>(registry, *object);
    }
    if (ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, *object)) {
        queue->orders.clear();
        ++queue->revision;
    }
    return true;
}

bool ObjectContainmentSystem::finishHijackerRelease(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId hijacker, uint32_t ruleIndex) const {
    const std::optional<ecs::entity> object =
        lifecycle.entityFromIdIncludingPending(hijacker);
    if (!object) return false;
    ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *object);
    if (!runtime || !runtime->plan ||
        ruleIndex >= runtime->behaviorStates.size() ||
        ruleIndex >= runtime->plan->behaviorRules.size() ||
        runtime->plan->behaviorRules[ruleIndex].kind !=
            ObjectTransportBehaviorKind::Hijacker) {
        return false;
    }
    ObjectTransportBehaviorState& state =
        runtime->behaviorStates[ruleIndex];
    if (state.phase != ObjectTransportBehaviorPhase::HijackerAttached) {
        return false;
    }
    state = {};
    return true;
}

bool ObjectContainmentSystem::cancelBattleBusUndeathForRealDeath(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId battleBus, uint32_t authoredOrder) const {
    const std::optional<ecs::entity> object =
        lifecycle.entityFromIdIncludingPending(battleBus);
    if (!object) return false;
    ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *object);
    if (!runtime || !runtime->plan) return false;
    const size_t count = std::min(runtime->plan->behaviorRules.size(),
                                  runtime->behaviorStates.size());
    for (size_t index = 0; index < count; ++index) {
        const ObjectTransportBehaviorRule& rule =
            runtime->plan->behaviorRules[index];
        if (rule.kind != ObjectTransportBehaviorKind::BattleBusSlowDeath ||
            rule.authoredOrder != authoredOrder) {
            continue;
        }
        // BattleBusSlowDeathBehavior::onDie sets m_isRealDeath first and
        // clears m_isInFirstDeath before invoking the common SlowDeath path.
        // Reset both the airborne continuation and the landed empty-hulk
        // timer; stale already-published transactions are rejected by the
        // terminal Body gate in beginBattleBus* below.
        runtime->behaviorStates[index] = {};
        return true;
    }
    return false;
}

bool ObjectContainmentSystem::beginBattleBusUndeath(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId battleBus, uint32_t ruleIndex, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> object =
        lifecycle.entityFromIdIncludingPending(battleBus);
    if (!object || battleBusDeathBlocksContinuation(
            registry, lifecycle, *object, battleBus)) return false;
    ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *object);
    if (!runtime || !runtime->plan ||
        ruleIndex >= runtime->behaviorStates.size() ||
        ruleIndex >= runtime->plan->behaviorRules.size() ||
        runtime->plan->behaviorRules[ruleIndex].kind !=
            ObjectTransportBehaviorKind::BattleBusSlowDeath) {
        return false;
    }
    ObjectTransportBehaviorState& state = runtime->behaviorStates[ruleIndex];
    if (state.phase == ObjectTransportBehaviorPhase::BattleBusUndeath ||
        state.phase == ObjectTransportBehaviorPhase::BattleBusLanded) {
        return false;
    }
    state.phase = ObjectTransportBehaviorPhase::BattleBusUndeath;
    state.nextTick = saturatingAddTicks(confirmedTick, 10u);
    return true;
}

bool ObjectContainmentSystem::finishBattleBusUndeath(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId battleBus, uint32_t ruleIndex, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> object =
        lifecycle.entityFromIdIncludingPending(battleBus);
    if (!object) return false;
    ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *object);
    if (!runtime || !runtime->plan ||
        ruleIndex >= runtime->behaviorStates.size() ||
        ruleIndex >= runtime->plan->behaviorRules.size() ||
        runtime->plan->behaviorRules[ruleIndex].kind !=
            ObjectTransportBehaviorKind::BattleBusSlowDeath ||
        runtime->behaviorStates[ruleIndex].phase !=
            ObjectTransportBehaviorPhase::BattleBusUndeath) {
        return false;
    }
    const ObjectTransportBehaviorRule& rule =
        runtime->plan->behaviorRules[ruleIndex];
    if (ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, *object)) {
        queue->orders.clear();
        ++queue->revision;
    }
    if (ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, *object)) {
        physics->velocityUnitsPerSecond.x = {};
        physics->velocityUnitsPerSecond.y = {};
        physics->velocityUnitsPerSecond.z += rule.throwForce;
        physics->pendingForce = {};
        physics->yawRate = deterministicVariance(
            math::q32_32{1},
            (static_cast<uint64_t>(battleBus.value) << 32u) ^ confirmedTick);
        physics->pitchRate = deterministicVariance(
            math::q32_32{1}, confirmedTick + 1u);
        physics->rollRate = deterministicVariance(
            math::q32_32{1}, confirmedTick + 2u);
        physics->ownsAttitude = true;
    }
    return true;
}

bool ObjectContainmentSystem::beginBattleBusLanded(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId battleBus, uint32_t ruleIndex, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> object =
        lifecycle.entityFromIdIncludingPending(battleBus);
    if (!object || battleBusDeathBlocksContinuation(
            registry, lifecycle, *object, battleBus)) return false;
    ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *object);
    if (!runtime || !runtime->plan ||
        ruleIndex >= runtime->behaviorStates.size() ||
        ruleIndex >= runtime->plan->behaviorRules.size() ||
        runtime->plan->behaviorRules[ruleIndex].kind !=
            ObjectTransportBehaviorKind::BattleBusSlowDeath) {
        return false;
    }
    ObjectTransportBehaviorState& state = runtime->behaviorStates[ruleIndex];
    if (state.phase != ObjectTransportBehaviorPhase::BattleBusUndeath) {
        return false;
    }
    state.phase = ObjectTransportBehaviorPhase::BattleBusLanded;
    state.nextTick = 0;
    state.nextPollTick = confirmedTick;
    return true;
}

bool ObjectContainmentSystem::finishBattleBusLanded(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId battleBus, uint32_t ruleIndex, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> object =
        lifecycle.entityFromIdIncludingPending(battleBus);
    if (!object) return false;
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *object);
    if (!runtime || !runtime->plan ||
        ruleIndex >= runtime->behaviorStates.size() ||
        ruleIndex >= runtime->plan->behaviorRules.size() ||
        runtime->plan->behaviorRules[ruleIndex].kind !=
            ObjectTransportBehaviorKind::BattleBusSlowDeath ||
        runtime->behaviorStates[ruleIndex].phase !=
            ObjectTransportBehaviorPhase::BattleBusLanded) {
        return false;
    }
    if (ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, *object)) {
        queue->orders.clear();
        ++queue->revision;
    }
    if (ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, *object)) {
        physics->velocityUnitsPerSecond = {};
        physics->pendingForce = {};
    }
    static_cast<void>(ObjectDisabledSystem::setUntil(
        registry, *object, ObjectDisabledReason::Held,
        OBJECT_DISABLED_FOREVER_TICK, confirmedTick));
    const game::ModelConditionMask secondLife =
        game::modelConditionMaskOf(game::ModelConditionFlag::SecondLife);
    publishObjectModelConditionContribution(
        registry, *object,
        ObjectModelConditionContributionSource::Tactical,
        {}, secondLife, confirmedTick, ruleIndex);
    return true;
}

bool ObjectContainmentSystem::requestBehavior(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules,
    const ObjectTransportBehaviorRequest& request,
    container::Vector<ObjectDamageRequest>& outDamage,
    container::Vector<ObjectContainmentEvent>& containmentEvents,
    ObjectTransportEventStream& behaviorEvents,
    uint64_t& nextGameplaySubmissionOrdinal) const {
    const ObjectTransportBehaviorKind kind = requestBehaviorKind(request.kind);
    const std::optional<ecs::entity> object =
        request.kind == ObjectTransportBehaviorRequestKind::BunkerBust
            ? lifecycle.entityFromIdIncludingPending(request.object)
            : lifecycle.entityFromId(request.object);
    if (!object) {
        return false;
    }
    ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *object);
    if (!runtime || !runtime->plan) {
        return false;
    }
    const size_t ruleIndex = behaviorRuleIndex(
        *runtime, kind, request.authoredOrder);
    if (ruleIndex == std::numeric_limits<size_t>::max() ||
        ruleIndex >= runtime->behaviorStates.size()) {
        return false;
    }
    const ObjectTransportBehaviorRule& rule =
        runtime->plan->behaviorRules[ruleIndex];
    ObjectTransportBehaviorState& state = runtime->behaviorStates[ruleIndex];
    const uint32_t fps = rules.logicFramesPerSecond == 0
        ? 30u : rules.logicFramesPerSecond;
    const ObjectTerrainLayerComponent* terrainLayer =
        ecs::try_get<ObjectTerrainLayerComponent>(registry, *object);
    const uint32_t sourcePathfindLayer = terrainLayer
        ? terrainLayer->pathfindLayer
        : game::terrain::kGroundPathfindLayer;

    switch (request.kind) {
    case ObjectTransportBehaviorRequestKind::BunkerBust: {
        if (!request.requiredUpgradeSatisfied) break;
        const std::optional<ecs::entity> target =
            lifecycle.entityFromId(request.target);
        const ObjectContainmentComponent* contents = target
            ? ecs::try_get<ObjectContainmentComponent>(registry, *target) : nullptr;
        const ObjectContainmentRuntimeComponent* targetRuntime = target
            ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry,
                                                               *target)
            : nullptr;
        bool bustable = false;
        std::optional<ObjectContainmentKind> networkKind;
        if (targetRuntime && targetRuntime->plan) {
            for (const ObjectContainmentRule& containRule :
                 targetRuntime->plan->rules) {
                if (containRule.kind == ObjectContainmentKind::Cave ||
                    containRule.kind == ObjectContainmentKind::Tunnel) {
                    bustable = true;
                    networkKind = containRule.kind;
                    break;
                }
                bustable = bustable ||
                    containRule.kind == ObjectContainmentKind::Garrison;
            }
        }
        container::Vector<NetworkPassengerRecord> occupants;
        if (bustable && target && targetRuntime && networkKind) {
            occupants = collectNetworkPassengers(
                registry, lifecycle, *target, *targetRuntime, *networkKind);
        } else if (bustable && target && contents) {
            occupants.reserve(contents->objects.size());
            for (const ObjectContainedObjectRecord& occupant :
                 contents->objects) {
                occupants.push_back(
                    {occupant, request.target, *target,
                     std::numeric_limits<uint32_t>::max()});
            }
        }
        const ecs::entity anchorEntity = target ? *target : *object;
        const ObjectFixedTransformComponent* anchorTransform =
            ecs::try_get<ObjectFixedTransformComponent>(registry,
                                                        anchorEntity);
        const LogicFixedVec3 anchorPosition =
            anchorTransform && anchorTransform->authoritative
            ? anchorTransform->position
            : LogicFixedVec3{};
        ObjectTransportBunkerBustTransaction transaction{
            .source = request.object,
            .target = request.target,
            .detonationFx = rule.fxStart,
            .shockwaveWeapon = rule.shockwaveWeapon,
            .x = anchorPosition.x,
            .y = anchorPosition.y,
            .z = anchorPosition.z,
            .seismicRadius = rule.seismicRadius,
            .seismicMagnitude = rule.seismicMagnitude,
            .authoredOrder = rule.authoredOrder,
            .confirmedTick = request.confirmedTick,
        };
        if (bustable) {
            transaction.occupants.reserve(occupants.size());
            for (const NetworkPassengerRecord& occupant : occupants) {
                if (!occupant.record.object) continue;
                transaction.occupants.push_back({
                    .entrance = occupant.entrance,
                    .damage = {
                        .target = occupant.record.object,
                        .source = request.object,
                        .sourceSequence = rule.authoredOrder,
                        .amount = request.hasBunkerOccupantDamage
                            ? math::q32_32{100}
                            : math::q32_32{},
                        .damageType = request.hasBunkerOccupantDamage
                            ? request.bunkerOccupantDamageType
                            : game::DamageType::UNRESISTABLE,
                        .deathType = request.hasBunkerOccupantDamage
                            ? request.bunkerOccupantDeathType
                            : game::DeathType::NORMAL,
                        .forceKill = !request.hasBunkerOccupantDamage,
                        .confirmedTick = request.confirmedTick,
                    },
                });
            }
        }
        pushTransportGameplayTransaction(
            behaviorEvents, std::move(transaction),
            nextGameplaySubmissionOrdinal);
        return true;
    }
    case ObjectTransportBehaviorRequestKind::BattleBusStartUndeath: {
        container::Vector<ObjectDamageRequest> passengerDamage;
        const ObjectContainmentComponent* contents =
            ecs::try_get<ObjectContainmentComponent>(registry, *object);
        const bool burnedDeathToUnits = runtime->plan->rules.empty() ||
            runtime->plan->rules.front().burnedDeathToUnits;
        if (contents && rule.passengerDamageFraction > math::q32_32{}) {
            for (const ObjectContainedObjectRecord& record : contents->objects) {
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromId(record.object);
                if (!passenger) continue;
                const ObjectHealthComponent* health =
                    ecs::try_get<ObjectHealthComponent>(registry, *passenger);
                if (!health || health->effectivelyDead) continue;
                const math::q32_32 damage =
                    health->maximumFixed * rule.passengerDamageFraction;
                if (damage <= math::q32_32{}) continue;
                passengerDamage.push_back({
                    .target = record.object,
                    .source = request.object,
                    .sourceSequence = rule.authoredOrder,
                    .amount = damage,
                    .damageType = game::DamageType::UNRESISTABLE,
                    .deathType = burnedDeathToUnits
                        ? game::DeathType::BURNED
                        : game::DeathType::NORMAL,
                    .confirmedTick = request.confirmedTick,
                });
            }
        }
        std::optional<ObjectTransportOclTransaction> ocl;
        if (!rule.oclStart.empty()) {
            ocl = freezeTransportOclTransaction(
                registry, lifecycle, request.object, request.confirmedTick,
                rule.authoredOrder, rule.oclStart, sourcePathfindLayer);
        }
        pushTransportGameplayTransaction(
            behaviorEvents, ObjectTransportBattleBusStartTransaction{
                .battleBus = request.object,
                .fxList = rule.fxStart,
                .objectCreationList = std::move(ocl),
                .passengerDamage = std::move(passengerDamage),
                .ruleIndex = static_cast<uint32_t>(ruleIndex),
                .confirmedTick = request.confirmedTick,
            }, nextGameplaySubmissionOrdinal);
        return true;
    }
    case ObjectTransportBehaviorRequestKind::BattleBusLanded:
        if (state.phase != ObjectTransportBehaviorPhase::BattleBusUndeath) break;
        {
            std::optional<ObjectTransportOclTransaction> ocl;
            if (!rule.oclFinish.empty()) {
                ocl = freezeTransportOclTransaction(
                    registry, lifecycle, request.object,
                    request.confirmedTick, rule.authoredOrder,
                    rule.oclFinish, sourcePathfindLayer);
            }
            pushTransportGameplayTransaction(
                behaviorEvents, ObjectTransportBattleBusLandedTransaction{
                    .battleBus = request.object,
                    .fxList = rule.fxFinish,
                    .objectCreationList = std::move(ocl),
                    .ruleIndex = static_cast<uint32_t>(ruleIndex),
                    .confirmedTick = request.confirmedTick,
                }, nextGameplaySubmissionOrdinal);
        }
        return true;
    case ObjectTransportBehaviorRequestKind::HijackTarget: {
        const std::optional<ecs::entity> target =
            lifecycle.entityFromId(request.target);
        if (!target || request.target == request.object) break;
        state.target = request.target;
        state.phase = ObjectTransportBehaviorPhase::HijackerAttached;
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(registry, *target);
        state.targetWasAirborne = airborne && airborne->isAirborne;
        ObjectMapStatusComponent* mapStatus =
            ecs::try_get<ObjectMapStatusComponent>(registry, *object);
        state.hadMapStatus = mapStatus != nullptr;
        state.previousOffMap = mapStatus && mapStatus->offMap;
        if (!mapStatus)
            mapStatus = &ecs::emplace<ObjectMapStatusComponent>(registry, *object);
        mapStatus->offMap = true;
        setHijackerStatus(registry, *object, true, request.confirmedTick);
        synchronizeOne(registry, *object, *target);
        return true;
    }
    case ObjectTransportBehaviorRequestKind::ReleaseHijacker:
        if (state.phase != ObjectTransportBehaviorPhase::HijackerAttached) break;
        pushTransportGameplayTransaction(
            behaviorEvents, ObjectTransportHijackerReleaseTransaction{
                .hijacker = request.object,
                .parachuteTemplate = state.targetWasAirborne
                    ? rule.parachuteTemplate : container::String{},
                .ruleIndex = static_cast<uint32_t>(ruleIndex),
                .confirmedTick = request.confirmedTick,
            }, nextGameplaySubmissionOrdinal);
        return true;
    case ObjectTransportBehaviorRequestKind::PilotFindVehicle:
        state.pilotReturnToBaseIssued = false;
        state.nextTick = saturatingAddTicks(
            request.confirmedTick,
            millisecondsToTicks(rule.scanRateMilliseconds, fps));
        return true;
    case ObjectTransportBehaviorRequestKind::AssaultTransportUpdate: {
        if (!state.assaultOrderArmed) {
            return false;
        }
        state.phase = ObjectTransportBehaviorPhase::AssaultActive;
        // The external Attack/AttackMove command arms the module, while the
        // DEPLOY weapon hit supplies the actual designated victim.  A direct
        // request remains useful to scripts/tests and may provide its own
        // target/goal, but must not replace the host's external order with a
        // synthetic Move event.
        if (request.target) state.target = request.target;
        if (request.x != math::q32_32{} || request.y != math::q32_32{} ||
            request.z != math::q32_32{}) {
            state.assaultGoalX = request.x;
            state.assaultGoalY = request.y;
            state.assaultGoalZ = request.z;
        }
        // beginAssault only supplies the DEPLOY weapon's victim. Occupants
        // which boarded after the external Attack command remain new members
        // and must wait for the next external attack before they may deploy.
        state.assaultOrderArmed = false;
        return true;
    }
    case ObjectTransportBehaviorRequestKind::DeliverPayload:
        if (state.deliveryDoorOpen)
            projectContainmentDoorTransition(
                registry, *object, false, request.confirmedTick);
        if (ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(registry, *object))
            locomotion->usePreciseZPosition = false;
        state.target = request.target;
        state.attempts = 0;
        state.deliveredCount = 0;
        state.phase = ObjectTransportBehaviorPhase::DeliveryApproach;
        state.hasDeliveryOverride = request.hasDeliveryOverride;
        state.deliveryContainerTemplate = request.payloadContainerTemplate;
        state.deliveryPayloadWeapon = request.payloadWeaponTemplate;
        state.deliveryTargetX = request.x;
        state.deliveryTargetY = request.y;
        state.deliveryTargetZ = request.z;
        state.deliveryRouteX = request.hasDeliveryRouteTarget
            ? request.routeX : request.x;
        state.deliveryRouteY = request.hasDeliveryRouteTarget
            ? request.routeY : request.y;
        state.deliveryRouteZ = request.hasDeliveryRouteTarget
            ? request.routeZ : request.z;
        state.deliveryDistance = request.deliveryDistance;
        state.exitPitchRate = request.hasDeliveryOverride
            ? request.exitPitchRate : rule.exitPitchRate;
        state.diveStartDistance = request.hasDeliveryOverride
            ? request.diveStartDistance : rule.diveStartDistance;
        state.diveEndDistance = request.hasDeliveryOverride
            ? request.diveEndDistance : rule.diveEndDistance;
        state.strafeLength = request.hasDeliveryOverride
            ? request.strafeLength : rule.strafeLength;
        // PreOpenDistance exists only on the per-delivery OCL payload. The
        // scripted module-data path intentionally supplies zero.
        state.preOpenDistance = request.preOpenDistance;
        state.dropOffsetX = request.dropOffsetX;
        state.dropOffsetY = request.dropOffsetY;
        state.dropOffsetZ = request.dropOffsetZ;
        state.dropVarianceX = request.dropVarianceX;
        state.dropVarianceY = request.dropVarianceY;
        state.dropVarianceZ = request.dropVarianceZ;
        state.dropDelayMilliseconds = request.dropDelayMilliseconds;
        state.maximumAttempts = request.maximumAttempts;
        state.visibleItemsDroppedPerInterval = request.hasDeliveryOverride
            ? request.visibleItemsDroppedPerInterval
            : rule.visibleItemsDroppedPerInterval;
        state.visiblePayloadCount = request.hasDeliveryOverride
            ? request.visiblePayloadCount : rule.visiblePayloadCount;
        state.visiblePayloadsDelivered = 0;
        state.deliveryDiveState = 0;
        state.deliveryDoorOpen = false;
        state.deliveryFreeToExit = false;
        state.deliveryExitHeadingArmed = false;
        state.inheritTransportVelocity = request.hasDeliveryOverride
            ? request.inheritTransportVelocity
            : rule.inheritTransportVelocity;
        state.parachuteDirectly = request.hasDeliveryOverride
            ? request.parachuteDirectly : rule.parachuteDirectly;
        state.selfDestructAfterDelivery = request.hasDeliveryOverride
            ? request.selfDestructAfterDelivery
            : rule.selfDestructAfterDelivery;
        state.fireWeaponPayload = request.hasDeliveryOverride
            ? request.fireWeaponPayload : rule.fireWeaponPayload;
        state.deliveryDecal = request.deliveryDecal;
        state.deliveryDecalRadius = request.deliveryDecalRadius;
        state.deliveryDecalShadowTypeMask =
            request.deliveryDecalShadowTypeMask;
        state.deliveryDecalMinimumOpacity =
            request.deliveryDecalMinimumOpacity;
        state.deliveryDecalMaximumOpacity =
            request.deliveryDecalMaximumOpacity;
        state.deliveryDecalOpacityThrobTicks =
            request.deliveryDecalOpacityThrobTicks;
        state.deliveryDecalColor = request.deliveryDecalColor;
        state.deliveryDecalUsesPlayerColor =
            request.deliveryDecalUsesPlayerColor;
        state.deliveryDecalOnlyVisibleToOwningPlayer =
            request.deliveryDecalOnlyVisibleToOwningPlayer;
        state.visiblePayloadTemplate = request.hasDeliveryOverride
            ? request.visiblePayloadTemplate : rule.visiblePayloadTemplate;
        state.visiblePayloadWeapon = request.hasDeliveryOverride
            ? request.visiblePayloadWeapon : rule.visiblePayloadWeapon;
        state.visibleDropBoneBaseName = request.hasDeliveryOverride
            ? request.visibleDropBoneBaseName
            : rule.visibleDropBoneBaseName;
        state.visibleSubObjectBaseName = request.hasDeliveryOverride
            ? request.visibleSubObjectBaseName
            : rule.visibleSubObjectBaseName;
        state.strafingWeaponSlot = request.hasDeliveryOverride
            ? request.strafingWeaponSlot : rule.strafingWeaponSlot;
        state.strafeWeaponFx = request.hasDeliveryOverride
            ? request.strafeWeaponFx : rule.strafeWeaponFx;
        state.hasPreviousDeliveryDistance = false;
        state.nextTick = 0;
        {
            const container::String& decal = state.hasDeliveryOverride
                ? state.deliveryDecal : rule.deliveryDecal;
            const math::q32_32 radius = state.hasDeliveryOverride
                ? state.deliveryDecalRadius : rule.deliveryDecalRadius;
            // A new delivery always replaces the previous marker.  Empty
            // descriptors are therefore meaningful: the session consumer
            // clears any active decal and simply does not create a new one.
            pushTransportPresentationEvent(
                behaviorEvents, ObjectTransportDeliveryStartedPresentation{
                .transport = request.object,
                .decalTexture = decal,
                .subObjectBaseName = state.visibleSubObjectBaseName,
                .x = request.x,
                .y = request.y,
                .z = request.z,
                .radius = radius,
                .decalShadowTypeMask = state.hasDeliveryOverride
                    ? state.deliveryDecalShadowTypeMask
                    : rule.deliveryDecalShadowTypeMask,
                .decalMinimumOpacity = state.hasDeliveryOverride
                    ? state.deliveryDecalMinimumOpacity
                    : rule.deliveryDecalMinimumOpacity,
                .decalMaximumOpacity = state.hasDeliveryOverride
                    ? state.deliveryDecalMaximumOpacity
                    : rule.deliveryDecalMaximumOpacity,
                .decalOpacityThrobTicks = state.hasDeliveryOverride
                    ? state.deliveryDecalOpacityThrobTicks
                    : std::max<uint64_t>(
                          1u, millisecondsToTicks(
                                  rule.deliveryDecalOpacityThrobMilliseconds,
                                  fps)),
                .decalColor = state.hasDeliveryOverride
                    ? state.deliveryDecalColor : rule.deliveryDecalColor,
                .visibleSubObjectCount = state.visiblePayloadCount,
                .confirmedTick = request.confirmedTick,
                .decalUsesPlayerColor = state.hasDeliveryOverride
                    ? state.deliveryDecalUsesPlayerColor
                    : rule.deliveryDecalUsesPlayerColor,
                    .decalOnlyVisibleToOwningPlayer = state.hasDeliveryOverride
                        ? state.deliveryDecalOnlyVisibleToOwningPlayer
                        : rule.deliveryDecalOnlyVisibleToOwningPlayer,
                });
        }
        setSystemMoveOrder(
            registry, *object, INVALID_OBJECT_ID,
            {state.deliveryRouteX, state.deliveryRouteY,
             state.deliveryRouteZ},
            ObjectOrderSystemPurpose::DeliverPayload,
            rule.authoredOrder, request.confirmedTick);
        return true;
    }

    return false;
}

bool ObjectContainmentSystem::acknowledgePayloadDrop(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId transport, ObjectId payload, uint32_t ruleIndex,
    uint32_t attempt) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(transport);
    ObjectContainmentRuntimeComponent* runtime = entity
        ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *entity)
        : nullptr;
    if (!runtime || !runtime->plan ||
        ruleIndex >= runtime->plan->behaviorRules.size() ||
        ruleIndex >= runtime->behaviorStates.size() ||
        runtime->plan->behaviorRules[ruleIndex].kind !=
            ObjectTransportBehaviorKind::DeliverPayloadAI) {
        return false;
    }
    ObjectTransportBehaviorState& state = runtime->behaviorStates[ruleIndex];
    if (state.phase != ObjectTransportBehaviorPhase::AwaitingDelivery ||
        attempt != state.deliveredCount + 1u) {
        return false;
    }
    const std::optional<ecs::entity> payloadEntity =
        lifecycle.entityFromIdIncludingPending(payload);
    const ObjectContainedByComponent* edge = payloadEntity
        ? ecs::try_get<ObjectContainedByComponent>(registry, *payloadEntity)
        : nullptr;
    if (edge && edge->container == transport) return false;
    state.deliveredCount = attempt;
    return true;
}


namespace object_containment_detail {

void updateTransportBehaviors(
    const ObjectContainmentSystem& system,
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage,
    const ContainmentUpdateCandidate& candidate,
    ObjectContainmentRuntimeComponent& runtime,
    const ObjectContainmentComponent& contents,
    container::Vector<ObjectContainmentEvent>* containmentEvents,
    ObjectTransportEventStream* behaviorEvents,
    uint64_t& nextGameplaySubmissionOrdinal,
    const PlayerRegistry* players,
    const game::terrain::TerrainLogic* terrain, uint32_t fps) {
        const size_t behaviorCount = std::min(
            runtime.plan->behaviorRules.size(), runtime.behaviorStates.size());
        for (size_t ruleIndex = 0; ruleIndex < behaviorCount; ++ruleIndex) {
            const ObjectTransportBehaviorRule& rule =
                runtime.plan->behaviorRules[ruleIndex];
            ObjectTransportBehaviorState& state =
                runtime.behaviorStates[ruleIndex];

            // OCL DelayDeliveryMax projects DISABLED_DEFAULT onto a newly
            // created transport.  The legacy AI update does not advance while
            // that reason is active; merely pausing locomotion would still let
            // a close spawn open its doors or drop payload early.
            if (rule.kind == ObjectTransportBehaviorKind::DeliverPayloadAI &&
                isObjectDisabledBy(registry, candidate.entity,
                                   ObjectDisabledReason::Default,
                                   confirmedTick)) {
                continue;
            }

            if (rule.kind == ObjectTransportBehaviorKind::BunkerBuster) {
                const ObjectMissileProjectileComponent* missile =
                    ecs::try_get<ObjectMissileProjectileComponent>(
                        registry, candidate.entity);
                const uint32_t frequency = std::max<uint32_t>(
                    1u, rule.crashThroughBunkerFxFrequency);
                if (behaviorEvents && missile &&
                    missile->state == ObjectMissileProjectileState::KillSelf &&
                    !rule.fxFinish.empty() && confirmedTick % frequency == 0) {
                    pushTransportPresentationEvent(
                        *behaviorEvents, ObjectTransportFxPresentation{
                            .object = candidate.container,
                            .fxList = rule.fxFinish,
                        });
                }
                continue;
            }

            if (rule.kind == ObjectTransportBehaviorKind::BattleBusSlowDeath &&
                state.phase == ObjectTransportBehaviorPhase::BattleBusUndeath) {
                if (confirmedTick <= state.nextTick || !terrain) continue;
                const ObjectFixedTransformComponent* transform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, candidate.entity);
                const LogicFixedVec3 position = transform
                    ? transform->position : LogicFixedVec3{};
                const math::q32_32 ground = transform
                    ? math::q32_32::from_raw(terrain->groundHeightRaw(
                          position.x.raw(), position.y.raw()))
                    : math::q32_32{};
                if (!transform || !transform->authoritative || position.z >
                    ground + math::q32_32::from_fraction(1, 20)) {
                    continue;
                }
                if (behaviorEvents) {
                    const ObjectTerrainLayerComponent* layer =
                        ecs::try_get<ObjectTerrainLayerComponent>(
                            registry, candidate.entity);
                    std::optional<ObjectTransportOclTransaction> ocl;
                    if (!rule.oclFinish.empty()) {
                        ocl = freezeTransportOclTransaction(
                            registry, lifecycle, candidate.container,
                            confirmedTick, rule.authoredOrder,
                            rule.oclFinish,
                            layer ? layer->pathfindLayer
                                  : game::terrain::kGroundPathfindLayer);
                    }
                    pushTransportGameplayTransaction(
                        *behaviorEvents,
                        ObjectTransportBattleBusLandedTransaction{
                            .battleBus = candidate.container,
                            .fxList = rule.fxFinish,
                            .objectCreationList = std::move(ocl),
                            .ruleIndex = static_cast<uint32_t>(ruleIndex),
                            .confirmedTick = confirmedTick,
                        }, nextGameplaySubmissionOrdinal);
                } else if (system.beginBattleBusLanded(
                               registry, lifecycle, candidate.container,
                               static_cast<uint32_t>(ruleIndex),
                               confirmedTick)) {
                    static_cast<void>(system.finishBattleBusLanded(
                        registry, lifecycle, candidate.container,
                        static_cast<uint32_t>(ruleIndex), confirmedTick));
                }
                continue;
            }

            if (rule.kind == ObjectTransportBehaviorKind::BattleBusSlowDeath &&
                state.phase == ObjectTransportBehaviorPhase::BattleBusLanded) {
                if (rule.emptyDestructionDelayMilliseconds == 0) continue;
                if (confirmedTick < state.nextPollTick) continue;
                state.nextPollTick = saturatingAddTicks(confirmedTick, 15u);
                if (!contents.objects.empty()) {
                    state.nextTick = 0;
                } else if (state.nextTick == 0) {
                    state.nextTick = saturatingAddTicks(
                        confirmedTick, millisecondsToTicks(
                            rule.emptyDestructionDelayMilliseconds, fps));
                } else if (confirmedTick > state.nextTick) {
                    outDamage.push_back({
                        .target = candidate.container,
                        .source = INVALID_OBJECT_ID,
                        .sourceSequence = rule.authoredOrder,
                        .submissionOrdinal = reserveTransportGameplayOrdinal(
                            nextGameplaySubmissionOrdinal),
                        .amount = math::q32_32{},
                        .damageType = game::DamageType::PENALTY,
                        .deathType = game::DeathType::EXTRA_4,
                        .forceKill = true,
                        .confirmedTick = confirmedTick,
                    });
                    state = {};
                }
                continue;
            }

            if (rule.kind == ObjectTransportBehaviorKind::Hijacker &&
                state.phase == ObjectTransportBehaviorPhase::HijackerAttached) {
                const std::optional<ecs::entity> target =
                    lifecycle.entityFromId(state.target);
                if (target) {
                    const ObjectAirborneComponent* airborne =
                        ecs::try_get<ObjectAirborneComponent>(registry, *target);
                    state.targetWasAirborne = airborne && airborne->isAirborne;
                    synchronizeOne(registry, candidate.entity, *target);
                    const ObjectVeterancyComponent* hijackerVeterancy =
                        ecs::try_get<ObjectVeterancyComponent>(
                            registry, candidate.entity);
                    const ObjectVeterancyComponent* targetVeterancy =
                        ecs::try_get<ObjectVeterancyComponent>(registry, *target);
                    if (behaviorEvents && hijackerVeterancy && targetVeterancy &&
                        hijackerVeterancy->level != targetVeterancy->level) {
                        const bool hijackerLower =
                            hijackerVeterancy->level < targetVeterancy->level;
                        pushTransportGameplayTransaction(
                            *behaviorEvents,
                            ObjectTransportVeterancySyncTransaction{
                                .lower = hijackerLower
                                    ? candidate.container : state.target,
                                .higher = hijackerLower
                                    ? state.target : candidate.container,
                                .level = hijackerLower
                                    ? targetVeterancy->level
                                    : hijackerVeterancy->level,
                                .confirmedTick = confirmedTick,
                            }, nextGameplaySubmissionOrdinal);
                    }
                } else {
                    if (behaviorEvents) {
                        pushTransportGameplayTransaction(
                            *behaviorEvents,
                            ObjectTransportHijackerReleaseTransaction{
                                .hijacker = candidate.container,
                                .parachuteTemplate = state.targetWasAirborne
                                    ? rule.parachuteTemplate
                                    : container::String{},
                                .ruleIndex = static_cast<uint32_t>(ruleIndex),
                                .confirmedTick = confirmedTick,
                            }, nextGameplaySubmissionOrdinal);
                    } else {
                        static_cast<void>(system.beginHijackerRelease(
                            registry, lifecycle, candidate.container,
                            static_cast<uint32_t>(ruleIndex), confirmedTick));
                        static_cast<void>(system.finishHijackerRelease(
                            registry, lifecycle, candidate.container,
                            static_cast<uint32_t>(ruleIndex)));
                    }
                }
                continue;
            }

            if (rule.kind == ObjectTransportBehaviorKind::PilotFindVehicle &&
                state.phase == ObjectTransportBehaviorPhase::Idle &&
                !ecs::try_get<ObjectContainedByComponent>(registry,
                                                          candidate.entity)) {
                const OwnerComponent* pilotOwnerForController =
                    ecs::try_get<OwnerComponent>(registry, candidate.entity);
                const PlayerState* pilotPlayer =
                    players && pilotOwnerForController
                        ? players->get(pilotOwnerForController->player) : nullptr;
                if (players && (!pilotPlayer ||
                    pilotPlayer->controller != PlayerControllerKind::Ai)) {
                    continue;
                }
                ObjectOrderQueueComponent* pilotQueue =
                    ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                            candidate.entity);
                if (pilotQueue && !pilotQueue->orders.empty() &&
                    pilotQueue->orders.front().source != ObjectOrderSource::System) {
                    state.target = INVALID_OBJECT_ID;
                    continue;
                }

                const ObjectFixedTransformComponent* pilotTransform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, candidate.entity);
                if (!pilotTransform || !pilotTransform->authoritative)
                    continue;
                const LogicFixedVec3 pilotPosition =
                    pilotTransform->position;
                const OwnerComponent* pilotOwner =
                    ecs::try_get<OwnerComponent>(registry, candidate.entity);
                const ThingTemplateComponent* pilotType =
                    ecs::try_get<ThingTemplateComponent>(registry,
                                                         candidate.entity);
                const auto pilotWouldCollide = [&](const ObjectKindOfComponent* kinds) {
                    if (!pilotType || !pilotType->archetype ||
                        !pilotType->archetype->crateCollidePlan) return false;
                    for (const game::ObjectCrateCollideRule& collideRule :
                         pilotType->archetype->crateCollidePlan->rules) {
                        if (collideRule.kind !=
                            game::ObjectCrateCollideKind::Veterancy) continue;
                        if (kinds && game::objectKindsMatch(
                                kinds->mask, collideRule.requiredKinds,
                                collideRule.forbiddenKinds)) return true;
                    }
                    return false;
                };
                auto validVehicle = [&](ObjectId id)
                    -> std::optional<std::pair<ecs::entity, LogicFixedVec3>> {
                    const std::optional<ecs::entity> entity =
                        lifecycle.entityFromId(id);
                    if (!entity || *entity == candidate.entity) return std::nullopt;
                    const OwnerComponent* owner =
                        ecs::try_get<OwnerComponent>(registry, *entity);
                    const ObjectKindOfComponent* kinds =
                        ecs::try_get<ObjectKindOfComponent>(registry, *entity);
                    const ObjectHealthComponent* health =
                        ecs::try_get<ObjectHealthComponent>(registry, *entity);
                    const ObjectMapStatusComponent* map =
                        ecs::try_get<ObjectMapStatusComponent>(registry, *entity);
                    const ObjectFixedTransformComponent* transform =
                        ecs::try_get<ObjectFixedTransformComponent>(
                            registry, *entity);
                    if (!pilotOwner || !owner || owner->player != pilotOwner->player ||
                        !hasKind(kinds, game::ObjectKindOf::Vehicle) || !health ||
                        health->effectivelyDead || health->maximumFixed <= math::q32_32{} ||
                        health->currentFixed <
                            health->maximumFixed * rule.minimumHealthFraction ||
                        (map && map->offMap) || !transform ||
                        !transform->authoritative ||
                        !pilotWouldCollide(kinds)) {
                        return std::nullopt;
                    }
                    return std::pair{*entity, transform->position};
                };

                std::optional<std::pair<ecs::entity, LogicFixedVec3>> target;
                if (state.target) target = validVehicle(state.target);
                if (!target &&
                    (state.nextTick == 0 || confirmedTick >= state.nextTick)) {
                    state.target = INVALID_OBJECT_ID;
                    math::q32_32 bestDistanceSquared =
                        rule.scanRange * rule.scanRange;
                    const auto identities =
                        ecs::view<const ObjectIdentityComponent>(registry);
                    for (const ecs::entity vehicleEntity : identities) {
                        const ObjectId id = identities
                            .template get<const ObjectIdentityComponent>(vehicleEntity).id;
                        const auto candidateVehicle = validVehicle(id);
                        if (!candidateVehicle) continue;
                        const LogicFixedVec3& position = candidateVehicle->second;
                        const math::q32_32 dx = position.x - pilotPosition.x;
                        const math::q32_32 dy = position.y - pilotPosition.y;
                        const math::q32_32 distanceSquared =
                            dx * dx + dy * dy;
                        if (distanceSquared > bestDistanceSquared ||
                            (distanceSquared == bestDistanceSquared &&
                             state.target && id >= state.target)) continue;
                        bestDistanceSquared = distanceSquared;
                        state.target = id;
                        target = candidateVehicle;
                    }
                    state.nextTick = saturatingAddTicks(
                        confirmedTick,
                        millisecondsToTicks(rule.scanRateMilliseconds, fps));
                }

                if (target && state.target) {
                    state.pilotReturnToBaseIssued = false;
                    const LogicFixedVec3& targetPosition = target->second;
                    const math::q32_32 dx = targetPosition.x - pilotPosition.x;
                    const math::q32_32 dy = targetPosition.y - pilotPosition.y;
                    const ObjectGeometryComponent* targetGeometry =
                        ecs::try_get<ObjectGeometryComponent>(registry,
                                                              target->first);
                    const ObjectGeometryComponent* pilotGeometry =
                        ecs::try_get<ObjectGeometryComponent>(registry,
                                                              candidate.entity);
                    const math::q32_32 arrivalFixed = math::q32_32::max(
                        math::q32_32{int32_t{1}},
                        (targetGeometry
                             ? math::q32_32::max(
                                   math::q32_32{},
                                   targetGeometry->majorRadiusFixed)
                             : math::q32_32{int32_t{1}}) +
                        (pilotGeometry
                             ? math::q32_32::max(
                                   math::q32_32{},
                                   pilotGeometry->majorRadiusFixed)
                             : math::q32_32{int32_t{1}}));
                    if (dx * dx + dy * dy <=
                        arrivalFixed * arrivalFixed) {
                        // VeterancyCrateCollide owns the actual contact
                        // transaction. Keeping the Move intent stable until
                        // that phase runs reproduces aiEnter without adding a
                        // second containment path for pilot crates.
                    } else {
                        setSystemMoveOrder(
                            registry, candidate.entity, state.target,
                            targetPosition,
                            ObjectOrderSystemPurpose::PilotFindVehicle,
                            rule.authoredOrder, confirmedTick);
                    }
                } else if (!state.pilotReturnToBaseIssued && pilotPlayer &&
                           terrain && pilotPlayer->startPosition >= 0) {
                    const auto starts = terrain->multiplayerStartPositions();
                    const auto base = std::find_if(
                        starts.begin(), starts.end(),
                        [&](const game::terrain::MultiplayerStartPosition& start) {
                            return start.index == pilotPlayer->startPosition;
                        });
                    if (base != starts.end()) {
                        const LogicFixedVec3 baseCenter{
                            math::q32_32::from_raw(base->positionRaw[0]),
                            math::q32_32::from_raw(base->positionRaw[1]),
                            math::q32_32::from_raw(base->positionRaw[2]),
                        };
                        setSystemMoveOrder(
                            registry, candidate.entity, INVALID_OBJECT_ID,
                            baseCenter,
                            ObjectOrderSystemPurpose::PilotFindVehicle,
                            rule.authoredOrder, confirmedTick);
                        // RefCode latches only after getAiBaseCenter succeeds
                        // and aiMoveToPosition has actually been submitted.
                        state.pilotReturnToBaseIssued = true;
                    }
                }
                continue;
            }

            if (rule.kind == ObjectTransportBehaviorKind::AssaultTransportAI) {
                ObjectOrderQueueComponent* hostQueue =
                    ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                            candidate.entity);
                const ObjectOrderIntent* order = hostQueue &&
                        !hostQueue->orders.empty()
                    ? &hostQueue->orders.front() : nullptr;
                const bool external = order &&
                    (order->source == ObjectOrderSource::Player ||
                     order->source == ObjectOrderSource::Script);
                const bool newExternalHead = external &&
                    (!state.hasObservedAssaultOrder ||
                     order->issuedTick !=
                         state.observedAssaultOrderIssuedTick ||
                     order->sourceSequence !=
                         state.observedAssaultOrderSourceSequence ||
                     static_cast<uint8_t>(order->source) !=
                         state.observedAssaultOrderSource);
                const bool externalRevisionChanged = hostQueue &&
                    hostQueue->externalRevision !=
                        state.observedExternalRevision;
                if (externalRevisionChanged) {
                    state.observedExternalRevision =
                        hostQueue->externalRevision;
                }
                if (newExternalHead) {
                    state.hasObservedAssaultOrder = true;
                    state.observedAssaultOrderIssuedTick = order->issuedTick;
                    state.observedAssaultOrderSourceSequence =
                        order->sourceSequence;
                    state.observedAssaultOrderSource =
                        static_cast<uint8_t>(order->source);
                    const bool attackObject = external &&
                        order->kind == ObjectOrderKind::Attack &&
                        static_cast<bool>(order->targetObject);
                    const bool attackMove = external &&
                        order->kind == ObjectOrderKind::Move &&
                        order->attackMove && order->hasTargetPosition;
                    if (attackObject || attackMove) {
                        // aiDoCommand only arms AssaultTransport.  Healthy
                        // passengers stay aboard until a DEPLOY weapon hit
                        // invokes AssaultTransportUpdate/beginAssault.
                        state.phase = ObjectTransportBehaviorPhase::Idle;
                        state.target = attackObject
                            ? order->targetObject : INVALID_OBJECT_ID;
                        state.assaultGoalX = order->targetX;
                        state.assaultGoalY = order->targetY;
                        state.assaultGoalZ = order->targetZ;
                        state.assaultAttackMove = attackMove;
                        state.assaultOrderArmed = true;
                        for (ObjectAssaultTransportMemberState& member :
                             state.assaultMembers)
                            member.newlyObserved = false;
                    } else {
                        // Stop, Move and every other external command cancel
                        // the old assault and keep recalling previously
                        // managed fighters until they are back inside.
                        state.phase =
                            ObjectTransportBehaviorPhase::AssaultRecalling;
                        state.target = INVALID_OBJECT_ID;
                        state.assaultAttackMove = false;
                        state.assaultOrderArmed = false;
                    }
                } else if (externalRevisionChanged &&
                           hostQueue->replacementExternalRevision ==
                               hostQueue->externalRevision &&
                           !external) {
                    // A replacement Stop may leave no queue head at all. An
                    // appended waypoint or deletion advances externalRevision
                    // too, but must not restart/cancel the currently executing
                    // assault until that external order actually reaches the
                    // front of the queue.
                    state.phase =
                        ObjectTransportBehaviorPhase::AssaultRecalling;
                    state.target = INVALID_OBJECT_ID;
                    state.assaultAttackMove = false;
                    state.assaultOrderArmed = false;
                }

                for (const ObjectContainedObjectRecord& record :
                     contents.objects) {
                    const auto found = std::lower_bound(
                        state.assaultMembers.begin(),
                        state.assaultMembers.end(), record.object,
                        [](const ObjectAssaultTransportMemberState& member,
                           ObjectId id) { return member.object < id; });
                    if (found == state.assaultMembers.end() ||
                        found->object != record.object) {
                        state.assaultMembers.insert(found, {
                            .object = record.object,
                            .newlyObserved = state.assaultRosterInitialized,
                        });
                    }
                }
                state.assaultRosterInitialized = true;
                state.assaultMembers.erase(
                    std::remove_if(
                        state.assaultMembers.begin(),
                        state.assaultMembers.end(),
                        [&](const ObjectAssaultTransportMemberState& member) {
                            const std::optional<ecs::entity> entity =
                                lifecycle.entityFromId(member.object);
                            if (!entity) return true;
                            const ObjectHealthComponent* health =
                                ecs::try_get<ObjectHealthComponent>(
                                    registry, *entity);
                            if (lifecycle.isPendingDestroy(member.object) ||
                                (health && (health->effectivelyDead ||
                                            health->terminalDeathIssued))) {
                                return true;
                            }
                            const ObjectOrderQueueComponent* queue =
                                ecs::try_get<ObjectOrderQueueComponent>(
                                    registry, *entity);
                            return queue && !queue->orders.empty() &&
                                (queue->orders.front().source ==
                                     ObjectOrderSource::Player ||
                                 queue->orders.front().source ==
                                     ObjectOrderSource::Script);
                        }),
                    state.assaultMembers.end());

                const ObjectStatusComponent* hostStatus =
                    ecs::try_get<ObjectStatusComponent>(registry,
                                                        candidate.entity);
                const bool hostAttacking = hostStatus && hostStatus->hasAny(
                    game::objectStatusBit(game::ObjectStatusFlag::IsAttacking));
                const bool onlyNewMembers = std::all_of(
                    state.assaultMembers.begin(), state.assaultMembers.end(),
                    [](const ObjectAssaultTransportMemberState& member) {
                        return member.newlyObserved;
                    });
                if (hostAttacking && onlyNewMembers) {
                    // AssaultTransportAIUpdate::isAttackPointless idles the
                    // host when its DEPLOY attack cannot release any member.
                    // This is an AI-owned cancellation, not a new external
                    // command, so externalRevision must remain untouched.
                    if (hostQueue && !hostQueue->orders.empty()) {
                        hostQueue->orders.clear();
                        ++hostQueue->revision;
                    }
                    state.phase = ObjectTransportBehaviorPhase::Idle;
                    state.assaultOrderArmed = false;
                    continue;
                }

                const bool recalling = state.phase ==
                    ObjectTransportBehaviorPhase::AssaultRecalling;
                if (state.phase !=
                        ObjectTransportBehaviorPhase::AssaultActive &&
                    !recalling) continue;
                bool targetAlive = !state.target;
                if (state.target) {
                    const std::optional<ecs::entity> targetEntity =
                        lifecycle.entityFromId(state.target);
                    const ObjectHealthComponent* targetHealth = targetEntity
                        ? ecs::try_get<ObjectHealthComponent>(
                              registry, *targetEntity)
                        : nullptr;
                    targetAlive = targetEntity &&
                        !lifecycle.isPendingDestroy(state.target) &&
                        (!targetHealth || (!targetHealth->effectivelyDead &&
                                           !targetHealth->terminalDeathIssued));
                }
                const ObjectHealthComponent* hostHealth =
                    ecs::try_get<ObjectHealthComponent>(registry,
                                                        candidate.entity);
                const bool hostDead = lifecycle.isPendingDestroy(
                    candidate.container) ||
                    (hostHealth && hostHealth->effectivelyDead);
                const ObjectFixedTransformComponent* hostTransform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, candidate.entity);
                const LogicFixedVec3 hostPosition =
                    hostTransform && hostTransform->authoritative
                    ? hostTransform->position
                    : LogicFixedVec3{};

                for (ObjectAssaultTransportMemberState& member :
                     state.assaultMembers) {
                    const std::optional<ecs::entity> memberEntity =
                        lifecycle.entityFromId(member.object);
                    if (!memberEntity) continue;
                    const ObjectHealthComponent* health =
                        ecs::try_get<ObjectHealthComponent>(registry,
                                                            *memberEntity);
                    if (!health || health->effectivelyDead ||
                        health->maximumFixed <= math::q32_32{}) continue;
                    const bool injured = health->currentFixed <
                        health->maximumFixed *
                            rule.healMembersAtLifeFraction;
                    const bool fullyHealthy = health->currentFixed >=
                        health->maximumFixed;
                    const ObjectContainedByComponent* containedBy =
                        ecs::try_get<ObjectContainedByComponent>(
                            registry, *memberEntity);
                    const bool containedHere = containedBy &&
                        containedBy->container == candidate.container;

                    if (containedHere) {
                        if (recalling || (member.newlyObserved && !hostDead))
                            continue;
                        if ((fullyHealthy && targetAlive) || hostDead) {
                            container::Vector<ObjectContainmentEvent> discarded;
                            container::Vector<ObjectContainmentEvent>& detachEvents =
                                containmentEvents ? *containmentEvents : discarded;
                            if (system.requestDetach(
                                    registry, lifecycle, {
                                        .kind = ObjectContainmentRequestKind::Detach,
                                        .container = candidate.container,
                                        .object = member.object,
                                        .confirmedTick = confirmedTick,
                                        .force = true,
                                    }, detachEvents)) {
                                setSystemAttackOrderAfterContainmentExit(
                                    registry, *memberEntity, state.target,
                                    {state.assaultGoalX, state.assaultGoalY,
                                     state.assaultGoalZ},
                                    state.assaultAttackMove || !state.target,
                                    state.assaultAttackMove,
                                    ObjectOrderSystemPurpose::AssaultTransport,
                                    rule.authoredOrder, confirmedTick);
                            }
                        }
                        continue;
                    }

                    if (recalling || injured ||
                        (!targetAlive && !state.assaultAttackMove)) {
                        const ObjectFixedTransformComponent* memberTransform =
                            ecs::try_get<ObjectFixedTransformComponent>(
                                registry, *memberEntity);
                        if (!memberTransform ||
                            !memberTransform->authoritative ||
                            !hostTransform ||
                            !hostTransform->authoritative) continue;
                        const LogicFixedVec3 memberPosition =
                            memberTransform->position;
                        const math::q32_32 dx = hostPosition.x - memberPosition.x;
                        const math::q32_32 dy = hostPosition.y - memberPosition.y;
                        const math::q32_32 arrival{3};
                        if (dx * dx + dy * dy <= arrival * arrival) {
                            container::Vector<ObjectContainmentEvent> discarded;
                            container::Vector<ObjectContainmentEvent>& attachEvents =
                                containmentEvents ? *containmentEvents : discarded;
                            static_cast<void>(system.requestAttach(
                                registry, lifecycle, {
                                    .kind = ObjectContainmentRequestKind::Attach,
                                    .container = candidate.container,
                                    .object = member.object,
                                    .confirmedTick = confirmedTick,
                                }, attachEvents));
                        } else {
                            setSystemMoveOrder(
                                registry, *memberEntity, candidate.container,
                                hostPosition,
                                ObjectOrderSystemPurpose::AssaultTransport,
                                rule.authoredOrder, confirmedTick);
                        }
                    } else {
                        setSystemAttackOrderAfterContainmentExit(
                            registry, *memberEntity, state.target,
                            {state.assaultGoalX, state.assaultGoalY,
                             state.assaultGoalZ},
                            state.assaultAttackMove || !state.target,
                            state.assaultAttackMove,
                            ObjectOrderSystemPurpose::AssaultTransport,
                            rule.authoredOrder, confirmedTick);
                    }
                }
                if (hostDead) {
                    state.phase = ObjectTransportBehaviorPhase::Idle;
                } else if (recalling) {
                    const bool allContained = std::all_of(
                        state.assaultMembers.begin(),
                        state.assaultMembers.end(),
                        [&](const ObjectAssaultTransportMemberState& member) {
                            const std::optional<ecs::entity> entity =
                                lifecycle.entityFromId(member.object);
                            const ObjectContainedByComponent* contained = entity
                                ? ecs::try_get<ObjectContainedByComponent>(
                                      registry, *entity)
                                : nullptr;
                            return !entity || (contained &&
                                contained->container == candidate.container);
                        });
                    if (allContained)
                        state.phase = ObjectTransportBehaviorPhase::Idle;
                }
                continue;
            }

            if (rule.kind == ObjectTransportBehaviorKind::DeliverPayloadAI &&
                state.phase == ObjectTransportBehaviorPhase::
                    DeliveryRecoveringOffMap) {
                if (confirmedTick < state.nextTick || !terrain) continue;
                const ObjectFixedTransformComponent* transform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, candidate.entity);
                if (!transform || !transform->authoritative) continue;
                const game::terrain::TerrainExtentRaw extent =
                    terrain->map().extentIncludingBorderRaw();
                const LogicFixedVec3 currentPosition = transform->position;
                const math::q32_32 fixedX = math::q32_32::clamp(
                    currentPosition.x,
                    math::q32_32::from_raw(extent.minimumX),
                    math::q32_32::from_raw(extent.maximumX));
                const math::q32_32 fixedY = math::q32_32::clamp(
                    currentPosition.y,
                    math::q32_32::from_raw(extent.minimumY),
                    math::q32_32::from_raw(extent.maximumY));
                const math::q32_32 yaw = math::fixed_atan2(
                    state.deliveryRouteY - fixedY,
                    state.deliveryRouteX - fixedX);
                writeAuthoritativeObjectTransform(
                    registry, candidate.entity,
                    LogicFixedVec3{
                        fixedX, fixedY, currentPosition.z},
                    yaw);
                if (ObjectPhysicsComponent* physics =
                        ecs::try_get<ObjectPhysicsComponent>(
                            registry, candidate.entity)) {
                    physics->position.x = fixedX;
                    physics->position.y = fixedY;
                    physics->lastPublishedPosition = physics->position;
                    physics->velocityUnitsPerSecond = {};
                    physics->yaw = yaw;
                    physics->ownsAttitude = true;
                    physics->sleeping = false;
                }
                if (ObjectMapStatusComponent* mapStatus =
                        ecs::try_get<ObjectMapStatusComponent>(
                            registry, candidate.entity)) {
                    if (state.hadMapStatus)
                        mapStatus->offMap = state.previousOffMap;
                    else
                        ecs::remove<ObjectMapStatusComponent>(
                            registry, candidate.entity);
                }
                if (RenderModelComponent* render =
                        ecs::try_get<RenderModelComponent>(
                            registry, candidate.entity))
                    render->hidden = false;
                state.phase =
                    ObjectTransportBehaviorPhase::DeliveryApproach;
                state.hasPreviousDeliveryDistance = false;
            }

            if (rule.kind == ObjectTransportBehaviorKind::DeliverPayloadAI &&
                state.phase == ObjectTransportBehaviorPhase::
                    DeliveryReapproach) {
                const ObjectFixedTransformComponent* transform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, candidate.entity);
                if (!transform || !transform->authoritative) continue;
                const LogicFixedVec3 position = transform->position;
                if (terrain) {
                    const game::terrain::TerrainExtentRaw extent =
                        terrain->map().extentIncludingBorderRaw();
                    const bool outside =
                        position.x.raw() < extent.minimumX ||
                        position.x.raw() > extent.maximumX ||
                        position.y.raw() < extent.minimumY ||
                        position.y.raw() > extent.maximumY;
                    if (outside) {
                        const ObjectLocomotionComponent* locomotion =
                            ecs::try_get<ObjectLocomotionComponent>(
                                registry, candidate.entity);
                        const uint64_t recoveryTicks =
                            recoveryTicksForTurnRate(
                                fps, locomotion
                                    ? locomotion->maximumTurnRate
                                    : math::q32_32{});
                        state.phase = ObjectTransportBehaviorPhase::
                            DeliveryRecoveringOffMap;
                        state.nextTick = saturatingAddTicks(
                            confirmedTick, recoveryTicks);
                        clearSystemOrder(
                            registry, candidate.entity,
                            ObjectOrderSystemPurpose::DeliverPayload,
                            rule.authoredOrder);
                        if (ObjectPhysicsComponent* physics =
                                ecs::try_get<ObjectPhysicsComponent>(
                                    registry, candidate.entity)) {
                            physics->velocityUnitsPerSecond = {};
                            physics->sleeping = false;
                        }
                        if (ObjectLocomotionComponent* mutableLocomotion =
                                ecs::try_get<ObjectLocomotionComponent>(
                                    registry, candidate.entity))
                            mutableLocomotion->usePreciseZPosition = false;
                        ObjectMapStatusComponent* mapStatus =
                            ecs::try_get<ObjectMapStatusComponent>(
                                registry, candidate.entity);
                        state.hadMapStatus = mapStatus != nullptr;
                        state.previousOffMap = mapStatus && mapStatus->offMap;
                        if (!mapStatus)
                            mapStatus = &ecs::emplace<
                                ObjectMapStatusComponent>(
                                registry, candidate.entity);
                        mapStatus->offMap = true;
                        if (RenderModelComponent* render =
                                ecs::try_get<RenderModelComponent>(
                                    registry, candidate.entity))
                            render->hidden = true;
                        continue;
                    }
                }
                const math::q32_32 dx =
                    state.deliveryManeuverX - position.x;
                const math::q32_32 dy =
                    state.deliveryManeuverY - position.y;
                const ObjectLocomotionComponent* locomotion =
                    ecs::try_get<ObjectLocomotionComponent>(
                        registry, candidate.entity);
                const math::q32_32 close = locomotion
                    ? math::q32_32::max(math::q32_32{int32_t{1}},
                                        locomotion->closeEnough)
                    : math::q32_32{int32_t{3}};
                if (dx * dx + dy * dy > close * close) {
                    setSystemMoveOrder(
                        registry, candidate.entity, INVALID_OBJECT_ID,
                        {state.deliveryManeuverX,
                         state.deliveryManeuverY,
                         state.deliveryManeuverZ},
                        ObjectOrderSystemPurpose::DeliverPayload,
                        rule.authoredOrder, confirmedTick);
                    continue;
                }
                state.phase =
                    ObjectTransportBehaviorPhase::DeliveryApproach;
                state.hasPreviousDeliveryDistance = false;
                setSystemMoveOrder(
                    registry, candidate.entity, INVALID_OBJECT_ID,
                    {state.deliveryRouteX, state.deliveryRouteY,
                     state.deliveryRouteZ},
                    ObjectOrderSystemPurpose::DeliverPayload,
                    rule.authoredOrder, confirmedTick);
                continue;
            }

            if (rule.kind == ObjectTransportBehaviorKind::DeliverPayloadAI &&
                state.phase == ObjectTransportBehaviorPhase::
                    DeliveryHeadingOffMap) {
                const ObjectFixedTransformComponent* transform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, candidate.entity);
                if (!transform || !transform->authoritative) continue;
                const LogicFixedVec3 position = transform->position;
                if (terrain) {
                    const game::terrain::TerrainExtentRaw extent =
                        terrain->map().extentIncludingBorderRaw();
                    const bool outside =
                        position.x.raw() < extent.minimumX ||
                        position.x.raw() > extent.maximumX ||
                        position.y.raw() < extent.minimumY ||
                        position.y.raw() > extent.maximumY;
                    if (outside) {
                        if (ObjectLocomotionComponent* locomotion =
                                ecs::try_get<ObjectLocomotionComponent>(
                                    registry, candidate.entity))
                            locomotion->usePreciseZPosition = false;
                        static_cast<void>(lifecycle.requestDestroy(
                            candidate.container, ObjectDestroyReason::System,
                            confirmedTick));
                        state = {};
                        continue;
                    }
                }
                if (state.deliveryExitHeadingArmed) {
                    math::q32_32 currentYaw = transform->yawRadians;
                    if (const ObjectPhysicsComponent* physics =
                            ecs::try_get<ObjectPhysicsComponent>(
                                registry, candidate.entity);
                        physics && physics->ownsAttitude)
                        currentYaw = physics->yaw;
                    const math::q32_32_sincos initial =
                        math::fixed_sincos(state.deliveryExitHeadingYaw);
                    const math::q32_32_sincos current =
                        math::fixed_sincos(currentYaw);
                    const math::q32_32 dot =
                        initial.cosine * current.cosine +
                        initial.sine * current.sine;
                    if (dot < math::q32_32::from_fraction(3, 10)) {
                        static_cast<void>(lifecycle.requestDestroy(
                            candidate.container, ObjectDestroyReason::System,
                            confirmedTick));
                        state = {};
                        continue;
                    }
                }
                setSystemMoveOrder(
                    registry, candidate.entity, INVALID_OBJECT_ID,
                    {state.deliveryManeuverX, state.deliveryManeuverY,
                     state.deliveryManeuverZ},
                    ObjectOrderSystemPurpose::DeliverPayload,
                    rule.authoredOrder, confirmedTick);
                continue;
            }

            if (rule.kind == ObjectTransportBehaviorKind::DeliverPayloadAI &&
                state.phase == ObjectTransportBehaviorPhase::DeliveryApproach) {
                const ObjectFixedTransformComponent* transform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, candidate.entity);
                if (!transform || !transform->authoritative) continue;
                const LogicFixedVec3 position = transform->position;
                const game::terrain::TerrainExtentRaw physicalExtent = terrain
                    ? terrain->map().extentIncludingBorderRaw()
                    : game::terrain::TerrainExtentRaw{};
                if (terrain &&
                    (position.x.raw() < physicalExtent.minimumX ||
                     position.x.raw() > physicalExtent.maximumX ||
                     position.y.raw() < physicalExtent.minimumY ||
                     position.y.raw() > physicalExtent.maximumY)) {
                    const ObjectLocomotionComponent* locomotion =
                        ecs::try_get<ObjectLocomotionComponent>(
                            registry, candidate.entity);
                    const uint64_t recoveryTicks = recoveryTicksForTurnRate(
                        fps, locomotion ? locomotion->maximumTurnRate
                                        : math::q32_32{});
                    state.phase = ObjectTransportBehaviorPhase::
                        DeliveryRecoveringOffMap;
                    state.nextTick = saturatingAddTicks(
                        confirmedTick, recoveryTicks);
                    clearSystemOrder(
                        registry, candidate.entity,
                        ObjectOrderSystemPurpose::DeliverPayload,
                        rule.authoredOrder);
                    if (ObjectPhysicsComponent* physics =
                            ecs::try_get<ObjectPhysicsComponent>(
                                registry, candidate.entity)) {
                        physics->velocityUnitsPerSecond = {};
                        physics->sleeping = false;
                    }
                    if (ObjectLocomotionComponent* mutableLocomotion =
                            ecs::try_get<ObjectLocomotionComponent>(
                                registry, candidate.entity))
                        mutableLocomotion->usePreciseZPosition = false;
                    ObjectMapStatusComponent* mapStatus =
                        ecs::try_get<ObjectMapStatusComponent>(
                            registry, candidate.entity);
                    state.hadMapStatus = mapStatus != nullptr;
                    state.previousOffMap = mapStatus && mapStatus->offMap;
                    if (!mapStatus)
                        mapStatus = &ecs::emplace<ObjectMapStatusComponent>(
                            registry, candidate.entity);
                    mapStatus->offMap = true;
                    if (RenderModelComponent* render =
                            ecs::try_get<RenderModelComponent>(
                                registry, candidate.entity))
                        render->hidden = true;
                    continue;
                }
                const LogicFixedVec3 targetPosition{
                    state.deliveryTargetX,
                    state.deliveryTargetY,
                    state.deliveryTargetZ};
                const math::q32_32 dx = targetPosition.x - position.x;
                const math::q32_32 dy = targetPosition.y - position.y;
                const math::q32_32 distanceSquared = dx * dx + dy * dy;
                if (state.deliveryDiveState == 0 &&
                    state.diveStartDistance > math::q32_32{} &&
                    distanceSquared <= state.diveStartDistance *
                        state.diveStartDistance) {
                    state.deliveryDiveState = 1;
                    // DeliverPayloadAIUpdate enters DIVESTATE_DIVING on this
                    // exact confirmed distance edge and emits StartDive from
                    // the transport's UnitSpecificSounds.  Route it through
                    // the existing transport presentation stream rather than
                    // asking animation/model-condition code to reconstruct it.
                    if (behaviorEvents) {
                        const ThingTemplateComponent* type =
                            ecs::try_get<ThingTemplateComponent>(
                                registry, candidate.entity);
                        const container::StringView cue = type &&
                                type->archetype
                            ? type->archetype->templateData.perUnitSound(
                                  "StartDive")
                            : container::StringView{};
                        if (!cue.empty()) {
                            pushTransportPresentationEvent(
                                *behaviorEvents,
                                ObjectTransportAudioPresentation{
                                    .object = candidate.container,
                                    .eventName = container::String{cue},
                                    .x = position.x,
                                    .y = position.y,
                                    .z = position.z,
                                });
                        }
                    }
                    if (ObjectLocomotionComponent* locomotion =
                            ecs::try_get<ObjectLocomotionComponent>(
                                registry, candidate.entity))
                        locomotion->usePreciseZPosition = true;
                }
                if (state.deliveryDiveState == 1) {
                    const math::q32_32 dz =
                        targetPosition.z - position.z;
                    const math::q32_32 distance3dSquared =
                        distanceSquared + dz * dz;
                    if (state.diveEndDistance > math::q32_32{} &&
                        distance3dSquared <= state.diveEndDistance *
                            state.diveEndDistance) {
                        state.deliveryDiveState = 2;
                        if (ObjectLocomotionComponent* locomotion =
                                ecs::try_get<ObjectLocomotionComponent>(
                                    registry, candidate.entity))
                            locomotion->usePreciseZPosition = false;
                    } else if (!state.strafingWeaponSlot.empty()) {
                        const ObjectPhysicsComponent* physics =
                            ecs::try_get<ObjectPhysicsComponent>(
                                registry, candidate.entity);
                        if (physics &&
                            physics->velocityUnitsPerSecond.z <
                                math::q32_32{5} *
                                    math::q32_32{static_cast<int32_t>(fps)}) {
                            math::q32_32 forwardX =
                                physics->velocityUnitsPerSecond.x;
                            math::q32_32 forwardY =
                                physics->velocityUnitsPerSecond.y;
                            const math::q32_32 horizontal =
                                math::q32_32::sqrt(
                                    forwardX * forwardX +
                                    forwardY * forwardY);
                            if (horizontal > math::q32_32{}) {
                                forwardX /= horizontal;
                                forwardY /= horizontal;
                            } else {
                                const math::q32_32_sincos facing =
                                    math::fixed_sincos(
                                        transform->yawRadians);
                                forwardX = facing.cosine;
                                forwardY = facing.sine;
                            }
                            const math::q32_32 currentDistance =
                                math::q32_32::sqrt(distance3dSquared);
                            const math::q32_32 denominator =
                                state.diveStartDistance -
                                state.diveEndDistance;
                            math::q32_32 ratio = denominator >
                                    math::q32_32{}
                                ? (state.diveStartDistance -
                                   currentDistance) / denominator
                                : math::q32_32{};
                            ratio = math::q32_32::max(
                                math::q32_32{}, math::q32_32::min(
                                    math::q32_32{int32_t{1}}, ratio));
                            const math::q32_32 lead =
                                ratio * math::q32_32{67};
                            if (behaviorEvents) {
                                pushTransportGameplayTransaction(
                                    *behaviorEvents,
                                    ObjectTransportPayloadStrafeTransaction{
                                        .transport = candidate.container,
                                        .weaponSlot = state.strafingWeaponSlot,
                                        .fxList = state.strafeWeaponFx,
                                        .x = targetPosition.x +
                                            forwardX * lead,
                                        .y = targetPosition.y +
                                            forwardY * lead,
                                        .z = terrain
                                            ? math::q32_32::from_raw(
                                                  terrain->groundHeightRaw(
                                                      (targetPosition.x +
                                                       forwardX * lead).raw(),
                                                      (targetPosition.y +
                                                       forwardY * lead).raw()))
                                            : targetPosition.z,
                                        .authoredOrder = rule.authoredOrder,
                                        .confirmedTick = confirmedTick,
                                    }, nextGameplaySubmissionOrdinal);
                            }
                        }
                    }
                }
                const math::q32_32 allowed = state.hasDeliveryOverride
                    ? state.deliveryDistance : rule.deliveryDistance;
                const bool inbound = state.hasPreviousDeliveryDistance &&
                    state.previousDeliveryDistanceSquared > distanceSquared;
                state.previousDeliveryDistanceSquared = distanceSquared;
                state.hasPreviousDeliveryDistance = true;
                const math::q32_32 approachWindow = allowed +
                    (inbound ? state.preOpenDistance : math::q32_32{});
                if (distanceSquared >= approachWindow * approachWindow) {
                    setSystemMoveOrder(
                        registry, candidate.entity, INVALID_OBJECT_ID,
                        {state.deliveryRouteX, state.deliveryRouteY,
                         state.deliveryRouteZ},
                        ObjectOrderSystemPurpose::DeliverPayload,
                        rule.authoredOrder, confirmedTick);
                    continue;
                }
                clearSystemOrder(
                    registry, candidate.entity,
                    ObjectOrderSystemPurpose::DeliverPayload,
                    rule.authoredOrder);
                if (ObjectLocomotionComponent* locomotion =
                        ecs::try_get<ObjectLocomotionComponent>(
                            registry, candidate.entity))
                    locomotion->usePreciseZPosition = false;
                state.phase = ObjectTransportBehaviorPhase::AwaitingDelivery;
                state.nextTick = saturatingAddTicks(
                    confirmedTick,
                    millisecondsToTicks(rule.doorDelayMilliseconds, fps));
                projectContainmentDoorTransition(
                    registry, candidate.entity, true, confirmedTick);
                state.deliveryDoorOpen = true;
                state.deliveryFreeToExit = false;
                continue;
            }

            if (rule.kind == ObjectTransportBehaviorKind::DeliverPayloadAI &&
                state.phase == ObjectTransportBehaviorPhase::AwaitingDelivery &&
                confirmedTick >= state.nextTick) {
                const ObjectFixedTransformComponent* transform =
                    ecs::try_get<ObjectFixedTransformComponent>(
                        registry, candidate.entity);
                if (!transform || !transform->authoritative) continue;
                // DeliveringState raises m_freeToExit only after DoorDelay.
                state.deliveryFreeToExit = true;
                const LogicFixedVec3 position = transform->position;
                const math::q32_32 dx = state.deliveryTargetX - position.x;
                const math::q32_32 dy = state.deliveryTargetY - position.y;
                const math::q32_32 distanceSquared = dx * dx + dy * dy;
                const bool inbound = state.hasPreviousDeliveryDistance &&
                    state.previousDeliveryDistanceSquared > distanceSquared;
                state.previousDeliveryDistanceSquared = distanceSquared;
                state.hasPreviousDeliveryDistance = true;
                const math::q32_32 allowed = state.hasDeliveryOverride
                    ? state.deliveryDistance : rule.deliveryDistance;
                const math::q32_32 deliveryWindow = allowed +
                    (inbound ? state.preOpenDistance : math::q32_32{});
                if (distanceSquared >= deliveryWindow * deliveryWindow) {
                    if (state.deliveryDoorOpen) {
                        projectContainmentDoorTransition(
                            registry, candidate.entity, false, confirmedTick);
                        state.deliveryDoorOpen = false;
                    }
                    state.deliveryFreeToExit = false;
                    if (ObjectLocomotionComponent* locomotion =
                            ecs::try_get<ObjectLocomotionComponent>(
                                registry, candidate.entity))
                        locomotion->usePreciseZPosition = false;
                    ++state.attempts;
                    const uint32_t maximumAttempts = state.hasDeliveryOverride
                        ? state.maximumAttempts : rule.maximumAttempts;
                    if (state.attempts > maximumAttempts) {
                        ObjectTransportPayloadFinishedTransaction*
                            finishedEvent = nullptr;
                        if (behaviorEvents) {
                            finishedEvent = &pushTransportGameplayTransaction(
                                *behaviorEvents,
                                ObjectTransportPayloadFinishedTransaction{
                                    .transport = candidate.container,
                                    .confirmedTick = confirmedTick,
                                }, nextGameplaySubmissionOrdinal);
                        }
                        if (state.selfDestructAfterDelivery || !terrain) {
                            if (finishedEvent) {
                                finishedEvent->destroyTransport = true;
                            } else {
                                static_cast<void>(lifecycle.requestDestroy(
                                    candidate.container,
                                    ObjectDestroyReason::System,
                                    confirmedTick));
                            }
                            state = {};
                        } else {
                            const math::q32_32 yaw =
                                transform->yawRadians;
                            const math::q32_32_sincos direction =
                                math::fixed_sincos(yaw);
                            const game::terrain::TerrainExtentRaw extent =
                                terrain->map().extentIncludingBorderRaw();
                            const math::q32_32 width =
                                math::q32_32::from_raw(extent.maximumX) -
                                math::q32_32::from_raw(extent.minimumX);
                            const math::q32_32 height =
                                math::q32_32::from_raw(extent.maximumY) -
                                math::q32_32::from_raw(extent.minimumY);
                            const math::q32_32 distance =
                                math::q32_32::sqrt(width * width +
                                                  height * height) *
                                math::q32_32::from_fraction(6, 5);
                            state.deliveryManeuverX = position.x +
                                direction.cosine * distance;
                            state.deliveryManeuverY = position.y +
                                direction.sine * distance;
                            state.deliveryManeuverZ = position.z;
                            state.deliveryExitHeadingYaw = yaw;
                            state.deliveryExitHeadingArmed = true;
                            state.phase = ObjectTransportBehaviorPhase::
                                DeliveryHeadingOffMap;
                            setSystemMoveOrder(
                                registry, candidate.entity,
                                INVALID_OBJECT_ID,
                                {state.deliveryManeuverX,
                                 state.deliveryManeuverY,
                                 state.deliveryManeuverZ},
                                ObjectOrderSystemPurpose::DeliverPayload,
                                rule.authoredOrder, confirmedTick);
                        }
                    } else {
                        const ObjectLocomotionComponent* locomotion =
                            ecs::try_get<ObjectLocomotionComponent>(
                                registry, candidate.entity);
                        const math::q32_32 speed = locomotion
                            ? math::q32_32::max(math::q32_32{},
                                               locomotion->maximumSpeed)
                            : math::q32_32{};
                        const math::q32_32 turnRate = locomotion
                            ? math::q32_32::max(math::q32_32{},
                                               locomotion->maximumTurnRate)
                            : math::q32_32{};
                        const math::q32_32 radius = turnRate >
                                math::q32_32{}
                            ? speed / turnRate
                            : math::q32_32{999999};
                        const math::q32_32 yaw = transform->yawRadians;
                        const math::q32_32_sincos direction =
                            math::fixed_sincos(yaw);
                        const math::q32_32 maneuverDistance =
                            radius * math::q32_32::from_fraction(11, 5);
                        state.deliveryManeuverX = position.x +
                            direction.cosine * maneuverDistance;
                        state.deliveryManeuverY = position.y +
                            direction.sine * maneuverDistance;
                        state.deliveryManeuverZ = position.z;
                        state.phase = ObjectTransportBehaviorPhase::
                            DeliveryReapproach;
                        setSystemMoveOrder(
                            registry, candidate.entity, INVALID_OBJECT_ID,
                            {state.deliveryManeuverX,
                             state.deliveryManeuverY,
                             state.deliveryManeuverZ},
                            ObjectOrderSystemPurpose::DeliverPayload,
                            rule.authoredOrder, confirmedTick);
                    }
                    continue;
                }

                // Success is observed only on the interval after the final
                // payload was dropped.  This retains the authored DropDelay
                // after the last item instead of closing the doors in the
                // same tick as the final detach/visible spawn.
                if (contents.objects.empty() &&
                    state.visiblePayloadsDelivered >=
                        state.visiblePayloadCount) {
                    if (state.deliveryDoorOpen) {
                        projectContainmentDoorTransition(
                            registry, candidate.entity, false, confirmedTick);
                        state.deliveryDoorOpen = false;
                    }
                    state.deliveryFreeToExit = false;
                    if (ObjectLocomotionComponent* locomotion =
                            ecs::try_get<ObjectLocomotionComponent>(
                                registry, candidate.entity))
                        locomotion->usePreciseZPosition = false;
                    ObjectTransportPayloadFinishedTransaction* finishedEvent =
                        nullptr;
                    if (behaviorEvents) {
                        finishedEvent = &pushTransportGameplayTransaction(
                            *behaviorEvents,
                            ObjectTransportPayloadFinishedTransaction{
                                .transport = candidate.container,
                                .confirmedTick = confirmedTick,
                            }, nextGameplaySubmissionOrdinal);
                    }
                    if (state.selfDestructAfterDelivery || !terrain) {
                        if (finishedEvent) {
                            finishedEvent->destroyTransport = true;
                        } else {
                            static_cast<void>(lifecycle.requestDestroy(
                                candidate.container,
                                ObjectDestroyReason::System,
                                confirmedTick));
                        }
                        state = {};
                    } else {
                        const math::q32_32 yaw = transform->yawRadians;
                        const math::q32_32_sincos direction =
                            math::fixed_sincos(yaw);
                        const game::terrain::TerrainExtentRaw extent =
                            terrain->map().extentIncludingBorderRaw();
                        const math::q32_32 width =
                            math::q32_32::from_raw(extent.maximumX) -
                            math::q32_32::from_raw(extent.minimumX);
                        const math::q32_32 height =
                            math::q32_32::from_raw(extent.maximumY) -
                            math::q32_32::from_raw(extent.minimumY);
                        const math::q32_32 distance =
                            math::q32_32::sqrt(width * width +
                                              height * height) *
                            math::q32_32::from_fraction(6, 5);
                        state.deliveryManeuverX = position.x +
                            direction.cosine * distance;
                        state.deliveryManeuverY = position.y +
                            direction.sine * distance;
                        state.deliveryManeuverZ = position.z;
                        state.deliveryExitHeadingYaw = yaw;
                        state.deliveryExitHeadingArmed = true;
                        state.phase = ObjectTransportBehaviorPhase::
                            DeliveryHeadingOffMap;
                        setSystemMoveOrder(
                            registry, candidate.entity, INVALID_OBJECT_ID,
                            {state.deliveryManeuverX,
                             state.deliveryManeuverY,
                             state.deliveryManeuverZ},
                            ObjectOrderSystemPurpose::DeliverPayload,
                            rule.authoredOrder, confirmedTick);
                    }
                    continue;
                }

                const math::q32_32 baseX = state.hasDeliveryOverride
                    ? state.dropOffsetX : rule.dropOffsetX;
                const math::q32_32 baseY = state.hasDeliveryOverride
                    ? state.dropOffsetY : rule.dropOffsetY;
                const math::q32_32 baseZ = state.hasDeliveryOverride
                    ? state.dropOffsetZ : rule.dropOffsetZ;
                if (!contents.objects.empty()) {
                    const auto payloadRecord = std::min_element(
                        contents.objects.begin(), contents.objects.end(),
                        [](const ObjectContainedObjectRecord& left,
                           const ObjectContainedObjectRecord& right) {
                            if (left.entryOrdinal != right.entryOrdinal) {
                                return left.entryOrdinal < right.entryOrdinal;
                            }
                            return left.object < right.object;
                        });
                    const ObjectId payloadObject = payloadRecord->object;
                    if (state.fireWeaponPayload ||
                        !state.deliveryPayloadWeapon.empty()) {
                    if (behaviorEvents) {
                        pushTransportGameplayTransaction(
                            *behaviorEvents,
                            ObjectTransportPayloadWeaponTransaction{
                                .transport = candidate.container,
                                .payloadObject = payloadObject,
                                .weaponTemplate =
                                    state.deliveryPayloadWeapon,
                                .x = state.deliveryTargetX + baseX,
                                .y = state.deliveryTargetY + baseY,
                                .z = state.deliveryTargetZ + baseZ,
                                .authoredOrder = rule.authoredOrder,
                                .confirmedTick = confirmedTick,
                            }, nextGameplaySubmissionOrdinal);
                    }
                    ++state.deliveredCount;
                    } else {
                        const uint32_t deliveryAttempt =
                            state.deliveredCount + 1u;
                        if (behaviorEvents) {
                            const uint64_t varianceSeed =
                                (static_cast<uint64_t>(candidate.container.value) << 32u) ^
                                static_cast<uint64_t>(payloadObject.value) ^
                                (static_cast<uint64_t>(deliveryAttempt) << 48u) ^
                                confirmedTick;
                            ObjectTransportPayloadDropTransaction dropped;
                            dropped.object = candidate.container;
                            dropped.target = payloadObject;
                            dropped.payload = state.hasDeliveryOverride
                                ? state.deliveryContainerTemplate
                                : rule.putInContainer ? rule.payloadTemplate
                                                     : container::String{};
                            dropped.x = baseX + deterministicVariance(
                                state.hasDeliveryOverride ? state.dropVarianceX
                                                          : rule.dropVarianceX,
                                varianceSeed + 0u);
                            dropped.y = baseY + deterministicVariance(
                                state.hasDeliveryOverride ? state.dropVarianceY
                                                          : rule.dropVarianceY,
                                varianceSeed + 1u);
                            dropped.z = baseZ + deterministicVariance(
                                state.hasDeliveryOverride ? state.dropVarianceZ
                                                          : rule.dropVarianceZ,
                                varianceSeed + 2u);
                            dropped.directLanding =
                                state.parachuteDirectly;
                            dropped.targetX =
                                state.deliveryTargetX;
                            dropped.targetY =
                                state.deliveryTargetY;
                            dropped.targetZ =
                                state.deliveryTargetZ;
                            dropped.routeX =
                                state.deliveryRouteX;
                            dropped.routeY =
                                state.deliveryRouteY;
                            dropped.routeZ =
                                state.deliveryRouteZ;
                            dropped.authoredOrder = rule.authoredOrder;
                            dropped.ruleIndex =
                                static_cast<uint32_t>(ruleIndex);
                            dropped.attempt = deliveryAttempt;
                            dropped.confirmedTick = confirmedTick;
                            dropped.inheritTransportVelocity =
                                state.inheritTransportVelocity;
                            pushTransportGameplayTransaction(
                                *behaviorEvents, std::move(dropped),
                                nextGameplaySubmissionOrdinal);
                        } else {
                            // Non-session callers have no detached gameplay
                            // stream to finish the parent transaction. Preserve
                            // the low-level structural fallback without
                            // changing the production path's authored order.
                            container::Vector<ObjectContainmentEvent>
                                discardedEvents;
                            container::Vector<ObjectContainmentEvent>&
                                detachEvents = containmentEvents
                                ? *containmentEvents : discardedEvents;
                            if (system.requestDetach(
                                registry, lifecycle,
                                {.kind = ObjectContainmentRequestKind::Detach,
                                 .container = candidate.container,
                                 .object = payloadObject,
                                 .confirmedTick = confirmedTick,
                                 .force = true},
                                detachEvents)) {
                                state.deliveredCount = deliveryAttempt;
                            }
                        }
                    }
                }

                // DeliverPayloadAIUpdate processes the real Contain front
                // first, then materializes visible payloads for this same
                // DropDelay occurrence. Their common ordinals preserve that
                // authored order across the detached session boundary.
                const uint32_t visiblePerInterval =
                    state.visibleItemsDroppedPerInterval;
                for (uint32_t visibleIndex = 0;
                     visibleIndex < visiblePerInterval &&
                     state.visiblePayloadsDelivered < state.visiblePayloadCount;
                     ++visibleIndex) {
                    ++state.visiblePayloadsDelivered;
                    if (behaviorEvents) {
                        ObjectTransportVisiblePayloadDropTransaction visible;
                        visible.object = candidate.container;
                        visible.payload = state.visiblePayloadTemplate;
                        visible.auxiliaryPayload =
                            state.visiblePayloadWeapon;
                        visible.attachmentBone =
                            state.visibleDropBoneBaseName;
                        visible.subObject = state.visibleSubObjectBaseName;
                        visible.targetX = state.deliveryTargetX;
                        visible.targetY = state.deliveryTargetY;
                        visible.targetZ = state.deliveryTargetZ;
                        visible.routeX = state.deliveryRouteX;
                        visible.routeY = state.deliveryRouteY;
                        visible.routeZ = state.deliveryRouteZ;
                        visible.pitchRate = state.exitPitchRate;
                        visible.authoredOrder = rule.authoredOrder;
                        visible.attempt = state.visiblePayloadsDelivered;
                        visible.confirmedTick = confirmedTick;
                        visible.inheritTransportVelocity =
                            state.inheritTransportVelocity;
                        visible.directLanding = state.parachuteDirectly;
                        pushTransportGameplayTransaction(
                            *behaviorEvents, std::move(visible),
                            nextGameplaySubmissionOrdinal);
                    }
                }

                state.nextTick = saturatingAddTicks(
                    confirmedTick,
                    millisecondsToTicks(
                        state.hasDeliveryOverride
                            ? state.dropDelayMilliseconds
                            : rule.dropDelayMilliseconds,
                        fps));
            }
        }
}

} // namespace object_containment_detail
} // namespace engine
