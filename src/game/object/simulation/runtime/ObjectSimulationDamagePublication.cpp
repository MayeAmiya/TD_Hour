#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/base/SimulationRandom.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/definition/LocomotorTemplate.h"
#include "game/object/simulation/combat/ObjectCombatProfileRuntime.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/plan/movement/ObjectPhysicsPlanTypes.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <utility>
#include "game/object/simulation/runtime/ObjectSimulationDetail.h"
#include "game/object/simulation/runtime/ObjectSimulationDamageDetail.h"

namespace engine {

namespace {

using PhysicsScalar = math::q32_32;
using HealthScalar = ObjectHealthComponent::Scalar;

const HealthScalar kHealthZero{};

[[nodiscard]] LogicFixedVec3 scaleFixed(
    const LogicFixedVec3& value, PhysicsScalar amount) noexcept {
    return {value.x * amount, value.y * amount, value.z * amount};
}

[[nodiscard]] bool queuedDamageOrder(
    const object_simulation_detail::QueuedDamageRequest& lhs,
    const object_simulation_detail::QueuedDamageRequest& rhs) noexcept {
    const ObjectDamageRequest& a = lhs.request;
    const ObjectDamageRequest& b = rhs.request;
    if (a.confirmedTick != b.confirmedTick)
        return a.confirmedTick < b.confirmedTick;
    const ObjectId aOrderGroup = a.causalGroup ? a.causalGroup : a.source;
    const ObjectId bOrderGroup = b.causalGroup ? b.causalGroup : b.source;
    if (aOrderGroup != bOrderGroup) return aOrderGroup < bOrderGroup;
    if (a.resolutionPhase != b.resolutionPhase) {
        return static_cast<uint8_t>(a.resolutionPhase) <
               static_cast<uint8_t>(b.resolutionPhase);
    }
    if (a.source != b.source) return a.source < b.source;
    if (a.sourceSequence != b.sourceSequence)
        return a.sourceSequence < b.sourceSequence;
    if (a.target != b.target) return a.target < b.target;
    return lhs.submissionOrdinal < rhs.submissionOrdinal;
}

} // namespace

using namespace object_simulation_detail;

namespace object_simulation_detail {

ObjectBodyReactionExecutor::ObjectBodyReactionExecutor(
    ObjectSimulation& simulation, ecs::registry& registry,
    ObjectLifecycle& lifecycle, ObjectDamageRequest request,
    ObjectUpgradeExecutionContext context, PlayerId victimPlayer,
    PlayerId sourcePlayer, int32_t experienceValue,
    bool experienceEligibleKiller, uint64_t confirmedTick) noexcept
    : m_simulation(simulation),
      m_registry(registry),
      m_lifecycle(lifecycle),
      m_request(std::move(request)),
      m_context(context),
      m_victimPlayer(victimPlayer),
      m_sourcePlayer(sourcePlayer),
      m_experienceValue(experienceValue),
      m_experienceEligibleKiller(experienceEligibleKiller),
      m_confirmedTick(confirmedTick) {}

void ObjectBodyReactionExecutor::dispatchDamage(
    const ObjectHealthEvent& event) {
    if (!m_context.content || !m_context.random) return;
    ObjectSimulationState& simulationState = state(m_simulation);
    simulationState.m_fireWeaponBehaviors.onHealthEvent(
        m_registry, m_lifecycle, event, *m_context.content,
        *m_context.random, simulationState.m_rules.logicFramesPerSecond,
        simulationState.m_nextGameplaySubmissionOrdinal,
        simulationState.m_systemWeaponFireCommands);
}

void ObjectBodyReactionExecutor::dispatchDamageStateChange(
    const ObjectHealthEvent& event) {
    ObjectSimulationState& simulationState = state(m_simulation);
    const size_t firstEvent =
        simulationState.m_transitionDamageFxEvents.size();
    const uint64_t firstSequence = simulationState.m_nextGameplaySubmissionOrdinal;
    simulationState.m_transitionDamageFx.onHealthEvent(
        m_registry, m_lifecycle, event, m_context.content,
        simulationState.m_sessionSeed,
        simulationState.m_nextGameplaySubmissionOrdinal,
        simulationState.m_transitionDamageFxEvents);
    simulationState.m_boneFx.onHealthEvent(
        m_registry, m_lifecycle, event, simulationState.m_rules,
        simulationState.m_sessionSeed,
        simulationState.m_nextGameplaySubmissionOrdinal,
        simulationState.m_transitionDamageFxEvents);
    auto begin = simulationState.m_transitionDamageFxEvents.begin() +
        static_cast<std::ptrdiff_t>(firstEvent);
    std::stable_sort(
        begin, simulationState.m_transitionDamageFxEvents.end(),
        [](const ObjectTransitionDamageFxEvent& left,
           const ObjectTransitionDamageFxEvent& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    uint64_t sequence = firstSequence;
    for (auto current = begin;
         current != simulationState.m_transitionDamageFxEvents.end();
         ++current) {
        current->emissionSequence = sequence++;
    }
}

void ObjectBodyReactionExecutor::awardLethalExperience() {
    if (!m_context.players) return;
    ObjectSimulationState& simulationState = state(m_simulation);
    const PlayerState* sourcePlayer =
        m_context.players->get(m_sourcePlayer);
    const PlayerState* victimPlayer =
        m_context.players->get(m_victimPlayer);
    const std::optional<ecs::entity> sourceEntity =
        m_lifecycle.entityFromIdIncludingPending(m_request.source);
    const std::optional<ecs::entity> victimEntity =
        m_lifecycle.entityFromId(m_request.target);
    const ThingTemplateComponent* victimType = victimEntity
        ? ecs::try_get<ThingTemplateComponent>(m_registry, *victimEntity)
        : nullptr;

    // ActiveBody::scoreTheKill runs before Object::onDie.  Keep the score
    // keeper side effects at this same Body edge; the later Died publication
    // is presentation/diagnostic fan-out only and must not be the gameplay
    // consumer that mutates player score state.
    if (sourceEntity && sourcePlayer && victimPlayer && victimType &&
        victimPlayer->playableSide &&
        !hasKind(ecs::try_get<ObjectKindOfComponent>(m_registry,
                                                     *victimEntity),
                 game::ObjectKindOf::IgnoredInGui)) {
        const ObjectKindOfComponent* victimKinds =
            ecs::try_get<ObjectKindOfComponent>(m_registry, *victimEntity);
        const ObjectStatusComponent* victimStatus =
            ecs::try_get<ObjectStatusComponent>(m_registry, *victimEntity);
        const bool underConstruction = victimStatus && victimStatus->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction));
        std::optional<PlayerScoredObjectKind> scoreKind;
        if (hasKind(victimKinds, game::ObjectKindOf::Structure) &&
            (hasKind(victimKinds, game::ObjectKindOf::Score) ||
             hasKind(victimKinds, game::ObjectKindOf::ScoreDestroy))) {
            scoreKind = PlayerScoredObjectKind::Building;
        } else if (hasKind(victimKinds, game::ObjectKindOf::Infantry) ||
                   hasKind(victimKinds, game::ObjectKindOf::Vehicle)) {
            if (hasKind(victimKinds, game::ObjectKindOf::Score) ||
                hasKind(victimKinds, game::ObjectKindOf::ScoreDestroy)) {
                scoreKind = PlayerScoredObjectKind::Unit;
            }
        }
        if (scoreKind && !underConstruction &&
            !victimType->name.empty()) {
            static_cast<void>(m_context.players->recordObjectLost(
                m_victimPlayer, victimType->name, *scoreKind));
            const PlayerRelationship relationship =
                relationshipBetweenObjects(
                    m_registry, *m_context.players, *sourceEntity,
                    *victimEntity);
            if (m_sourcePlayer != m_victimPlayer &&
                relationship == PlayerRelationship::Enemies) {
                static_cast<void>(m_context.players->recordObjectDestroyed(
                    m_sourcePlayer, m_victimPlayer, victimType->name,
                    *scoreKind));
            }
        }
    }

    // Bounty, skill/experience remain the stricter original killer path:
    // hostile source, playable victim, and no UnderConstruction credit.
    if (!m_experienceEligibleKiller) return;
    if (sourcePlayer && victimPlayer && victimType &&
        victimType->archetype &&
        sourcePlayer->cashBountyPercent > math::q32_32{}) {
        const int64_t buildCost = std::clamp<int64_t>(
            calculateObjectBuildCost(
                *victimType->archetype, *victimPlayer, m_registry,
                m_lifecycle),
            0, std::numeric_limits<int32_t>::max());
        if (buildCost > 0) {
            const uint64_t rawPercent = static_cast<uint64_t>(
                sourcePlayer->cashBountyPercent.raw());
            const uint64_t wholePercent = rawPercent >> 32u;
            const uint64_t fractionalPercent =
                rawPercent & UINT64_C(0xffffffff);
            const uint64_t fractionalProduct =
                static_cast<uint64_t>(buildCost) * fractionalPercent;
            const uint64_t fractionalReward =
                (fractionalProduct >> 32u) +
                ((fractionalProduct & UINT64_C(0xffffffff)) != 0u
                     ? 1u : 0u);
            const uint64_t reward = std::min<uint64_t>(
                static_cast<uint64_t>(std::numeric_limits<int32_t>::max()),
                static_cast<uint64_t>(buildCost) * wholePercent +
                    fractionalReward);
            if (reward != 0u && m_context.players->adjustCash(
                    m_sourcePlayer, static_cast<int64_t>(reward))) {
                static_cast<void>(m_context.players->recordMoneyEarned(
                    m_sourcePlayer, reward, m_confirmedTick));
            }
        }
    }
    const UpgradeMask sourceUpgrades = sourcePlayer
        ? sourcePlayer->upgrades.completed : UpgradeMask{};
    const ObjectExperienceMutation mutation =
        simulationState.m_experience.addPointsIncludingPendingDestroy(
            m_registry, m_lifecycle, m_request.source,
            m_experienceValue, true, m_confirmedTick);
    m_simulation.finalizeExperienceMutation(
        m_registry, m_lifecycle, mutation, m_request.target,
        sourceUpgrades, m_confirmedTick, m_context);
}

} // namespace object_simulation_detail

container::Vector<ObjectDamageTransactionIngress>
ObjectSimulation::takeReadyDamageTransactions(uint64_t confirmedTick) {
    auto leased = leaseReadyDamageTransactions(confirmedTick);
    container::Vector<ObjectDamageTransactionIngress> output;
    output.reserve(leased.size());
    for (ObjectDamageTransactionIngress& ingress : leased) {
        output.push_back(std::move(ingress));
    }
    return output;
}

ObjectSimulationEventLease<ObjectDamageTransactionIngress>
ObjectSimulation::leaseReadyDamageTransactions(uint64_t confirmedTick) {
    auto& state = object_simulation_detail::state(*this);
    auto& pending = state.m_damageRequests;
    auto& ready = state.m_readyDamageScratch;
    auto& output = state.m_readyDamageIngressScratch;
    ready.clear();
    ready.reserve(pending.size());

    // Preserve deferred encounter order while retaining the producer queue's
    // allocation. A later confirmed barrier sees exactly the same order as the
    // former deferred Vector, without pending = move(deferred).
    size_t deferredCount = 0;
    for (size_t index = 0; index < pending.size(); ++index) {
        object_simulation_detail::QueuedDamageRequest& queued = pending[index];
        if (queued.request.confirmedTick == 0)
            queued.request.confirmedTick = confirmedTick;
        if (queued.request.confirmedTick > confirmedTick) {
            if (deferredCount != index) {
                pending[deferredCount] = std::move(queued);
            }
            ++deferredCount;
        } else {
            ready.push_back(std::move(queued));
        }
    }
    pending.resize(deferredCount);
    std::sort(ready.begin(), ready.end(), queuedDamageOrder);

    output.clear();
    output.reserve(ready.size());
    for (object_simulation_detail::QueuedDamageRequest& queued : ready) {
        output.push_back({
            .request = std::move(queued.request),
            .submissionOrdinal = queued.submissionOrdinal,
        });
    }
    ready.clear();
    return ObjectSimulationEventLease<ObjectDamageTransactionIngress>{output};
}

void ObjectSimulation::discardQueuedDamageTransactions() noexcept {
    auto& state = object_simulation_detail::state(*this);
    state.m_damageRequests.clear();
    state.m_readyDamageScratch.clear();
    state.m_readyDamageIngressScratch.clear();
}

ObjectDamageTransactionResult ObjectSimulation::executeDamageTransaction(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectDamageRequest request, uint64_t confirmedTick,
    ObjectUpgradeExecutionContext context) {
    ObjectDamageTransactionResult result;
    auto& state = object_simulation_detail::state(*this);
    container::Vector<object_simulation_detail::QueuedDamageRequest> preserved =
        std::move(state.m_damageRequests);
    state.m_damageRequests.clear();
    queueDamage(std::move(request));
    const bool previousMode = state.m_resolvingSingleDamageTransaction;
    state.m_resolvingSingleDamageTransaction = true;
    resolveQueuedDamage(registry, lifecycle, confirmedTick, context, &result);
    state.m_resolvingSingleDamageTransaction = previousMode;
    container::Vector<object_simulation_detail::QueuedDamageRequest> generated =
        std::move(state.m_damageRequests);
    state.m_damageRequests = std::move(preserved);
    state.m_damageRequests.insert(
        state.m_damageRequests.end(),
        std::make_move_iterator(generated.begin()),
        std::make_move_iterator(generated.end()));
    return result;
}

bool ObjectSimulation::resumeDamageTransaction(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectBodyResumeState& bodyResume,
    ObjectUpgradeExecutionContext context) {
    auto& simulationState = object_simulation_detail::state(*this);
    if (bodyResume.healthEventStart > simulationState.m_healthEvents.size() ||
        bodyResume.bodyTransactionOrdinal == 0) {
        return false;
    }

    const ObjectDamageRequest& request = bodyResume.damage;
    const std::optional<ecs::entity> victimEntity =
        lifecycle.entityFromIdIncludingPending(request.target);
    const ObjectKindOfComponent* victimKinds = victimEntity
        ? ecs::try_get<ObjectKindOfComponent>(registry, *victimEntity)
        : nullptr;
    if (victimEntity && request.shockWaveAmount > PhysicsScalar{} &&
        request.shockWaveRadius > PhysicsScalar{}) {
        ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, *victimEntity);
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(registry, *victimEntity);
        const bool projectile =
            ecs::try_get<ObjectProjectileComponent>(registry, *victimEntity) !=
                nullptr ||
            hasKind(victimKinds, game::ObjectKindOf::Projectile);
        const bool airborneTarget = hasKind(
                                        victimKinds,
                                        game::ObjectKindOf::Aircraft) ||
            (airborne && airborne->isAirborne);
        if (physics && !projectile && !airborneTarget) {
            LogicFixedVec3 direction{
                request.shockWaveVectorX,
                request.shockWaveVectorY,
                request.shockWaveVectorZ,
            };
            const PhysicsScalar lengthSquared = direction.x * direction.x +
                direction.y * direction.y + direction.z * direction.z;
            if (lengthSquared > PhysicsScalar{}) {
                const PhysicsScalar length = PhysicsScalar::sqrt(lengthSquared);
                const PhysicsScalar distanceFromCenter = std::min(
                    PhysicsScalar{int32_t{1}},
                    length / request.shockWaveRadius);
                const PhysicsScalar distanceTaper = distanceFromCenter *
                    (PhysicsScalar{int32_t{1}} - request.shockWaveTaperOff);
                const PhysicsScalar magnitude = request.shockWaveAmount *
                    (PhysicsScalar{int32_t{1}} - distanceTaper);
                direction = scaleFixed({
                    direction.x / length,
                    direction.y / length,
                    direction.z / length,
                }, magnitude);
                direction.z = PhysicsScalar::sqrt(
                    direction.x * direction.x + direction.y * direction.y +
                    direction.z * direction.z);
                queuePhysicsRequest({
                    .target = request.target,
                    .source = request.source,
                    .sourceSequence = request.sourceSequence,
                    .linear = direction,
                    .kind = ObjectPhysicsRequestKind::ApplyShock,
                    .confirmedTick = request.confirmedTick,
                });

                const auto randomModifier = [&]() {
                    return context.random
                        ? context.random->fixedInclusive(
                              PhysicsScalar{-1}, PhysicsScalar{1})
                        : PhysicsScalar{};
                };
                queuePhysicsRequest({
                    .target = request.target,
                    .source = request.source,
                    .sourceSequence = request.sourceSequence,
                    .yawRate = physics->shockMaxYaw * randomModifier(),
                    .pitchRate = physics->shockMaxPitch * randomModifier(),
                    .rollRate = physics->shockMaxRoll * randomModifier(),
                    .kind = ObjectPhysicsRequestKind::AddAngularRates,
                    .confirmedTick = request.confirmedTick,
                });
                queuePhysicsRequest({
                    .target = request.target,
                    .source = request.source,
                    .sourceSequence = request.sourceSequence,
                    .enabled = true,
                    .kind = ObjectPhysicsRequestKind::SetStunned,
                    .confirmedTick = request.confirmedTick,
                });
            }
        }
    }

    for (size_t index = bodyResume.healthEventStart;
         index < simulationState.m_healthEvents.size(); ++index) {
        ObjectHealthEvent& event = simulationState.m_healthEvents[index];
        if (event.bodyTransactionOrdinal != 0) continue;
        event.bodyTransactionOrdinal = bodyResume.bodyTransactionOrdinal;
        if (event.kind == ObjectHealthEventKind::SecondLifeStarted) {
            container::Vector<ObjectDamageRequest> battleBusDamage;
            if (simulationState.m_containment.requestBehavior(
                    registry, lifecycle, simulationState.m_rules,
                    {.kind = ObjectTransportBehaviorRequestKind::BattleBusStartUndeath,
                     .object = event.object,
                     .target = event.source,
                     .confirmedTick = event.confirmedTick},
                    battleBusDamage, simulationState.m_containmentEvents,
                    simulationState.m_transportEvents,
                    simulationState.m_nextGameplaySubmissionOrdinal)) {
                for (ObjectDamageRequest& child : battleBusDamage) {
                    queueDamage(std::move(child));
                }
            }
        }
        if (event.kind == ObjectHealthEventKind::Died) {
            detachDeadAircraftReservations(
                simulationState.m_airfield, registry, lifecycle, event.object,
                event.confirmedTick, simulationState.m_airfieldEvents);
        }
        simulationState.m_poisoned.onHealthEvent(
            registry, lifecycle, event,
            simulationState.m_rules.logicFramesPerSecond);
        simulationState.m_fireUpdates.onHealthEvent(
            registry, lifecycle, event, context.random,
            simulationState.m_rules.logicFramesPerSecond,
            simulationState.m_objectFireAudioCommands);
        simulationState.m_supplyWarehouseCrippling.onHealthEvent(
            registry, lifecycle, event);
        if (!event.healthDecreased) continue;
        simulationState.m_baseRegenerate.onHealthDecreased(
            registry, lifecycle, event.object, event.confirmedTick,
            simulationState.m_rules);
        simulationState.m_autoHeal.onHealthDecreased(
            registry, lifecycle, event.object, event.confirmedTick,
            simulationState.m_rules);
    }
    updateBodyDamageVisuals(registry);
    return true;
}

void ObjectSimulation::resolveQueuedDamage(ecs::registry& registry, ObjectLifecycle& lifecycle,
                                           uint64_t confirmedTick,
                                           ObjectUpgradeExecutionContext context,
                                           ObjectDamageTransactionResult* transactionResult) {
    // Topple is a standard pre-Body object transaction, not a property of
    // NeutronMissile/WaveGuide/SpawnSlave scheduling. Any producer may append
    // typed Topple ingress; every Body barrier commits that ingress before it
    // considers damage. This mirrors Object::topple(); attemptDamage()
    // without making ObjectSimulation remember individual producers.
    object_simulation_detail::state(*this).m_tactical.consumeToppleRequests(
        registry, lifecycle, confirmedTick,
        object_simulation_detail::state(*this)
            .m_nextGameplaySubmissionOrdinal);
    // Damage submitted by scripts/earlier systems is applied before the
    // object's locomotor pass, so a same-tick damage threshold can select the
    // damaged movement rules exactly at the object-update boundary.  This is
    // intentionally callable without movement: ScriptBridge uses it after a
    // stamped DAMAGE effect so a following structural effect cannot erase the
    // already-authored hit before Body processes it.
    if (object_simulation_detail::state(*this).m_damageRequests.empty()) {
        return;
    }
    container::Vector<object_simulation_detail::QueuedDamageRequest> ready;
    container::Vector<object_simulation_detail::QueuedDamageRequest> deferred;
    ready.reserve(object_simulation_detail::state(*this).m_damageRequests.size());
    deferred.reserve(object_simulation_detail::state(*this).m_damageRequests.size());
    for (object_simulation_detail::QueuedDamageRequest& queued : object_simulation_detail::state(*this).m_damageRequests) {
        ObjectDamageRequest& request = queued.request;
        if (request.confirmedTick == 0) request.confirmedTick = confirmedTick;
        // Public submission may intentionally target a later confirmed frame
        // (for example a deferred ScriptEffect). Never consume it early just
        // because another object system happened to update first.
        if (request.confirmedTick > confirmedTick) {
            deferred.push_back(std::move(queued));
            continue;
        }
        ready.push_back(std::move(queued));
    }
    object_simulation_detail::state(*this).m_damageRequests.clear();
    // No EnTT view order or producer container order is allowed to decide
    // which same-frame hit claims a kill. Source ObjectId and its authored
    // sequence form the visible key for ordinary requests; a nonzero causal
    // group keeps a projectile's warhead and its DetonateCallsKill Body hit
    // adjacent, matching the original per-projectile detonation transaction.
    // The private ingress ordinal only breaks malformed/legacy ties
    // deterministically within one session.
    std::sort(ready.begin(), ready.end(), queuedDamageOrder);
    bool suspendedBody = false;
    for (size_t readyIndex = 0; readyIndex < ready.size(); ++readyIndex) {
        // Keep a value while callbacks append to `ready`; vector growth must
        // never invalidate the currently resolving Body transaction.
        object_simulation_detail::QueuedDamageRequest queued = std::move(ready[readyIndex]);
        bool generatedImmediateDamage = false;
        if (object_simulation_detail::state(*this).m_spawnSlave.routeHiveDamage(
                registry, lifecycle, queued.request) ==
            ObjectHiveDamageRoute::Swallowed) {
            continue;
        }
        uint64_t& nextBodyOrdinal = object_simulation_detail::state(*this)
                                        .m_nextBodyTransactionOrdinal;
        const uint64_t bodyTransactionOrdinal = nextBodyOrdinal++;
        if (nextBodyOrdinal == 0) ++nextBodyOrdinal;
        const std::optional<ecs::entity> victimEntity =
            lifecycle.entityFromId(queued.request.target);
        const OwnerComponent* victimOwner = victimEntity
            ? ecs::try_get<OwnerComponent>(registry, *victimEntity)
            : nullptr;
        const PlayerId victimPlayerId = victimOwner
            ? victimOwner->player : INVALID_PLAYER_ID;
        const PlayerState* victimPlayer =
            context.players && victimPlayerId
                ? context.players->get(victimPlayerId)
                : nullptr;
        const ObjectStatusComponent* victimStatus = victimEntity
            ? ecs::try_get<ObjectStatusComponent>(registry, *victimEntity)
            : nullptr;
        const ObjectKindOfComponent* victimKinds = victimEntity
            ? ecs::try_get<ObjectKindOfComponent>(registry, *victimEntity)
            : nullptr;
        const ThingTemplateComponent* victimType = victimEntity
            ? ecs::try_get<ThingTemplateComponent>(registry, *victimEntity)
            : nullptr;
        const bool experienceEligibleVictim = victimEntity && victimOwner && victimPlayer &&
            victimPlayer->isPlayableSide() &&
            !hasKind(victimKinds, game::ObjectKindOf::IgnoredInGui) &&
            !(victimStatus && victimStatus->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction)));
        const int32_t experienceValue = victimEntity
            ? object_simulation_detail::state(*this).m_experience.experienceValue(registry, *victimEntity)
            : 0;
        const std::optional<ecs::entity> sourceEntity =
            lifecycle.entityFromIdIncludingPending(queued.request.source);
        const OwnerComponent* sourceOwner = sourceEntity
            ? ecs::try_get<OwnerComponent>(registry, *sourceEntity)
            : nullptr;
        const PlayerId sourcePlayerId = sourceOwner
            ? sourceOwner->player : INVALID_PLAYER_ID;
        const bool experienceEligibleKiller = experienceEligibleVictim &&
            context.players && sourcePlayerId && queued.request.source &&
            sourcePlayerId != victimPlayerId &&
            sourceEntity && victimEntity &&
            relationshipBetweenObjects(
                registry, *context.players, *sourceEntity, *victimEntity) ==
                PlayerRelationship::Enemies;
        ObjectBodyReactionExecutor reactions{
            *this, registry, lifecycle, queued.request, context,
            victimPlayerId, sourcePlayerId, experienceValue,
            experienceEligibleKiller, confirmedTick};
        bool activeBodySpecialHandled = false;
        if (victimEntity &&
            queued.request.damageType == game::DamageType::KILL_GARRISONED) {
            activeBodySpecialHandled = true;
            const ObjectContainmentRuntimeComponent* containmentRuntime =
                ecs::try_get<ObjectContainmentRuntimeComponent>(
                    registry, *victimEntity);
            const ObjectContainmentComponent* contents =
                ecs::try_get<ObjectContainmentComponent>(registry,
                                                          *victimEntity);
            uint64_t killsRemaining = queued.amount > kHealthZero
                ? static_cast<uint64_t>(queued.amount.raw()) >> 32u : 0u;
            if (containmentRuntime && containmentRuntime->plan && contents &&
                killsRemaining != 0) {
                // ZH consumes ContainedItemsList in admission order. The ECS
                // reverse roster is ObjectId-sorted for lookup, so recover the
                // best available authored order from the frozen enter tick;
                // ObjectId is the deterministic tie-break for same-tick bulk
                // admission.
                container::Vector<ObjectContainedObjectRecord> occupants =
                    contents->objects;
                std::stable_sort(
                    occupants.begin(), occupants.end(),
                    [](const ObjectContainedObjectRecord& left,
                       const ObjectContainedObjectRecord& right) {
                        if (left.confirmedEnteredTick !=
                            right.confirmedEnteredTick) {
                            return left.confirmedEnteredTick <
                                right.confirmedEnteredTick;
                        }
                        return left.object < right.object;
                    });
                for (const ObjectContainedObjectRecord& record : occupants) {
                    if (killsRemaining == 0) break;
                    const std::optional<ecs::entity> occupant =
                        lifecycle.entityFromId(record.object);
                    if (!occupant) continue;
                    const ObjectHealthComponent* occupantHealth =
                        ecs::try_get<ObjectHealthComponent>(registry,
                                                            *occupant);
                    const ObjectContainedByComponent* edge =
                        ecs::try_get<ObjectContainedByComponent>(registry,
                                                                 *occupant);
                    if (!occupantHealth || occupantHealth->effectivelyDead ||
                        !edge || edge->container != queued.request.target ||
                        edge->containmentRuleIndex >=
                            containmentRuntime->plan->rules.size()) {
                        continue;
                    }
                    const ObjectContainmentRule& rule =
                        containmentRuntime->plan->rules[
                            edge->containmentRuleIndex];
                    if (rule.kind != ObjectContainmentKind::Garrison ||
                        rule.immuneToClearBuildingAttacks) {
                        continue;
                    }
                    queueDamage({
                        .target = record.object,
                        .source = queued.request.source,
                        .sourceSequence = queued.request.sourceSequence,
                        .causalGroup = queued.request.target,
                        .damageType = game::DamageType::UNRESISTABLE,
                        .deathType = game::DeathType::NORMAL,
                        .forceKill = true,
                        .confirmedTick = queued.request.confirmedTick,
                    });
                    if (context.players) {
                        const OwnerComponent* occupantOwner =
                            ecs::try_get<OwnerComponent>(registry,
                                                         *occupant);
                        if (occupantOwner && occupantOwner->player) {
                            static_cast<void>(context.players->
                                recordClearedGarrisonedBuilding(
                                    occupantOwner->player));
                        }
                    }
                    --killsRemaining;
                    generatedImmediateDamage = true;
                }
            }
        } else if (victimEntity &&
                   queued.request.damageType ==
                       game::DamageType::KILL_PILOT) {
            activeBodySpecialHandled = true;
            if (hasKind(victimKinds, game::ObjectKindOf::Vehicle)) {
                const ObjectContainmentRuntimeComponent* containmentRuntime =
                    ecs::try_get<ObjectContainmentRuntimeComponent>(
                        registry, *victimEntity);
                const ObjectContainmentComponent* contents =
                    ecs::try_get<ObjectContainmentComponent>(registry,
                                                              *victimEntity);
                ObjectId rider = INVALID_OBJECT_ID;
                const bool riderChangeContain = containmentRuntime &&
                    containmentRuntime->plan && std::any_of(
                        containmentRuntime->plan->rules.begin(),
                        containmentRuntime->plan->rules.end(),
                        [](const ObjectContainmentRule& rule) {
                            return rule.kind ==
                                ObjectContainmentKind::RiderChange;
                        });
                if (riderChangeContain && contents) {
                    for (const ObjectContainedObjectRecord& record :
                         contents->objects) {
                        const std::optional<ecs::entity> occupant =
                            lifecycle.entityFromId(record.object);
                        const ObjectContainedByComponent* edge = occupant
                            ? ecs::try_get<ObjectContainedByComponent>(
                                  registry, *occupant)
                            : nullptr;
                        if (edge && edge->container == queued.request.target &&
                            edge->containmentRuleIndex <
                                containmentRuntime->plan->rules.size() &&
                            containmentRuntime->plan->rules[
                                edge->containmentRuleIndex].kind ==
                                ObjectContainmentKind::RiderChange) {
                            rider = record.object;
                            break;
                        }
                    }
                }
                const ObjectLocomotionComponent* locomotion =
                    ecs::try_get<ObjectLocomotionComponent>(registry,
                                                             *victimEntity);
                const bool moving = locomotion &&
                    locomotion->state == ObjectLocomotionState::Moving;
                if (rider && moving) {
                    queueDamage({
                        .target = queued.request.target,
                        .source = queued.request.source,
                        .sourceSequence = queued.request.sourceSequence,
                        .causalGroup = queued.request.target,
                        .damageType = game::DamageType::UNRESISTABLE,
                        .deathType = game::DeathType::NORMAL,
                        .forceKill = true,
                        .confirmedTick = queued.request.confirmedTick,
                    });
                    generatedImmediateDamage = true;
                } else if (rider) {
                    static_cast<void>(object_simulation_detail::state(*this).m_containment.requestDetach(
                        registry, lifecycle,
                        {.kind = ObjectContainmentRequestKind::Detach,
                         .container = queued.request.target,
                         .object = rider,
                         .confirmedTick = queued.request.confirmedTick,
                         .force = true},
                        object_simulation_detail::state(*this).m_containmentEvents));
                    queueDamage({
                        .target = rider,
                        .source = queued.request.source,
                        .sourceSequence = queued.request.sourceSequence,
                        .causalGroup = queued.request.target,
                        .damageType = game::DamageType::UNRESISTABLE,
                        .deathType = game::DeathType::NORMAL,
                        .forceKill = true,
                        .confirmedTick = queued.request.confirmedTick,
                    });
                    generatedImmediateDamage = true;
                } else if (!riderChangeContain) {
                    if (ObjectOrderQueueComponent* queue =
                            ecs::try_get<ObjectOrderQueueComponent>(
                                registry, *victimEntity);
                        queue && !queue->orders.empty()) {
                        queue->orders.clear();
                        ++queue->revision;
                    }
                    static_cast<void>(ObjectDisabledSystem::setUntil(
                        registry, *victimEntity,
                        ObjectDisabledReason::Unmanned,
                        OBJECT_DISABLED_FOREVER_TICK,
                        queued.request.confirmedTick));
                    // GameSession consumes this existing stable-ID neutralize
                    // transaction; NeutronBlast and KILL_PILOT intentionally
                    // share the same academy/ownership consequence.
                    object_simulation_detail::state(*this)
                        .m_vehicleNeutralizationRequests.push_back({
                        .source = queued.request.source,
                        .target = queued.request.target,
                        .authoredOrder = queued.request.sourceSequence,
                        .confirmedTick = queued.request.confirmedTick,
                    });
                }
            }
        }

        if (activeBodySpecialHandled) {
            object_simulation_detail::state(*this).m_healthEvents.push_back({
                .kind = ObjectHealthEventKind::SpecialDamageApplied,
                .object = queued.request.target,
                .source = queued.request.source,
                .damageType = queued.request.damageType,
                .damageFxType = queued.request.damageFxOverride.value_or(
                    queued.request.damageType),
                .deathType = queued.request.deathType,
                .requestedAmount = queued.request.amount,
                .previousHealth = victimEntity &&
                        ecs::try_get<ObjectHealthComponent>(registry,
                                                            *victimEntity)
                    ? ecs::get<ObjectHealthComponent>(registry,
                                                       *victimEntity).currentFixed
                    : HealthScalar{},
                .currentHealth = victimEntity &&
                        ecs::try_get<ObjectHealthComponent>(registry,
                                                            *victimEntity)
                    ? ecs::get<ObjectHealthComponent>(registry,
                                                       *victimEntity).currentFixed
                    : HealthScalar{},
                .confirmedTick = queued.request.confirmedTick,
                .bodyTransactionOrdinal = bodyTransactionOrdinal,
            });
            if (generatedImmediateDamage &&
                !object_simulation_detail::state(*this)
                     .m_resolvingSingleDamageTransaction) {
                container::Vector<object_simulation_detail::QueuedDamageRequest> generated =
                    std::move(object_simulation_detail::state(*this).m_damageRequests);
                object_simulation_detail::state(*this).m_damageRequests.clear();
                ready.insert(
                    ready.begin() +
                        static_cast<std::ptrdiff_t>(readyIndex + 1u),
                    std::make_move_iterator(generated.begin()),
                    std::make_move_iterator(generated.end()));
            }
            continue;
        }

        // Bridge and BridgeTower callbacks fan the incoming authored health
        // percentage through the same Body queue.  They never write sibling
        // HP directly, and propagated requests identify their bridge/tower
        // source so the callback lane can reject recursive fan-out.
        container::Vector<ObjectDamageRequest> bridgePropagation;
        object_simulation_detail::state(*this).m_bridge.propagateHealthRequest(
            registry, lifecycle, queued.request, queued.amount,
            bridgePropagation);
        if (!bridgePropagation.empty()) {
            generatedImmediateDamage = true;
            for (ObjectDamageRequest& request : bridgePropagation) {
                queueDamage(std::move(request));
            }
        }

        const size_t healthEventStart = object_simulation_detail::state(*this).m_healthEvents.size();
        applyDamageRequest(*this, registry, lifecycle, queued.request, queued.amount,
                           object_simulation_detail::state(*this).m_rules, context, object_simulation_detail::state(*this).m_sessionSeed,
                           object_simulation_detail::state(*this).m_healthEvents,
                           reactions,
                           transactionResult);
        // Authored onDie handlers now submit child Damage directly through
        // ObjectSimulation instead of mutating an outer callback flag.
        generatedImmediateDamage = generatedImmediateDamage ||
            !object_simulation_detail::state(*this).m_damageRequests.empty();
        if (transactionResult && transactionResult->deathWalk) {
            TD_ASSERT(!transactionResult->bodyResume.has_value());
            transactionResult->bodyResume = ObjectBodyResumeState{
                .damage = queued.request,
                .healthEventStart = healthEventStart,
                .bodyTransactionOrdinal = bodyTransactionOrdinal,
            };
            suspendedBody = true;
            break;
        }
        if (!resumeDamageTransaction(
                registry, lifecycle,
                {.damage = queued.request,
                 .healthEventStart = healthEventStart,
                 .bodyTransactionOrdinal = bodyTransactionOrdinal},
                context)) {
            TD_ASSERT(false);
            return;
        }
        generatedImmediateDamage = generatedImmediateDamage ||
            !object_simulation_detail::state(*this).m_damageRequests.empty();
        if (generatedImmediateDamage &&
            !object_simulation_detail::state(*this)
                 .m_resolvingSingleDamageTransaction) {
            // Object::kill inside NeutronBlastBehavior dispatches nested Die
            // callbacks before the outer radius iterator advances. Insert the
            // stable ObjectId-ordered requests directly behind this request so
            // an unrelated same-tick hit cannot steal the death transition.
            container::Vector<object_simulation_detail::QueuedDamageRequest> generated =
                std::move(object_simulation_detail::state(*this).m_damageRequests);
            object_simulation_detail::state(*this).m_damageRequests.clear();
            ready.insert(
                ready.begin() + static_cast<std::ptrdiff_t>(readyIndex + 1u),
                std::make_move_iterator(generated.begin()),
                std::make_move_iterator(generated.end()));
        }
    }
    if (object_simulation_detail::state(*this)
            .m_resolvingSingleDamageTransaction) {
        auto& pending = object_simulation_detail::state(*this).m_damageRequests;
        pending.insert(
            pending.end(), std::make_move_iterator(deferred.begin()),
            std::make_move_iterator(deferred.end()));
    } else {
        object_simulation_detail::state(*this).m_damageRequests =
            std::move(deferred);
    }

    // Body damage is logic state. Copy its three display conditions into the
    // render-intent component at the completed fixed-frame boundary; the
    // renderer only consumes the detached snapshot and never infers HP.
    if (!suspendedBody) updateBodyDamageVisuals(registry);
}

} // namespace engine
