#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionObjectTargetRemapTransactions.h"
#include "game/session/transaction/GameSessionAIMoveOrderTransactions.h"
#include "game/session/transaction/GameSessionAIShadowTransactions.h"
#include "game/session/query/GameSessionAIOrderPolicy.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/spatial/ObjectSpatialIndex.h"

#include "debug/debug.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/runtime/ObjectAIAttackTargetPolicy.h"
#include "game/object/simulation/runtime/ObjectAIInsignificantBuildingPolicy.h"
#include "game/object/simulation/runtime/ObjectAIOpportunityTargetPolicy.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/combat/ObjectCombatTargetability.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/ai/definition/ObjectAIBehaviorPlan.h"
#include "game/terrain/MapVisibilityAuthority.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace engine {

namespace {

// RefCode keeps the aggressor in ActiveBody as a *clearable* last attacker:
// BodyModule::getClearableLastAttacker() returns the last DamageInfo source
// until hasAttackedMeAndICanReturnFire() consumes it with clearLastAttacker(),
// and every fresh hit re-arms it. That consumption happens on the first guard
// machine update after the hit, so the memory is at most one update long --
// "if he stops attacking us, then we want our timer to kick us off of him"
// (AIGuard.cpp). The AI boundary here is value-only and cannot clear an ECS
// flag, so project the equivalent bounded recency window instead. The damage
// phase resolves before this shadow phase inside one tick, so a hit is visible
// on the tick it lands; also accepting the previous tick covers damage
// published after the AI phase and mirrors ActiveBody's own "this or previous
// frame" preference window (rememberPreferredBodyDamageInfo).
constexpr uint64_t kGuardAggressorMemoryTicks = 1;

// This is the same distinction used by AIUpdateInterface::
// isDoingGroundMovement().  The current locomotor's legal AIR surface, not
// the object's temporary altitude, decides whether it is an air mover.  A
// ground truck knocked above terrain must still pathfind; a jet keeps the
// airborne QuickPath even while taxiing.  ObjectLocomotionComponent::surfaces
// is the frozen selected locomotor profile, rather than the whole authored
// locomotor-set union.
void projectLocomotionFacts(ai::ObjectAIReadOnlyFact& fact,
                            const ObjectLocomotionComponent* locomotion) noexcept {
    const game::LocomotorSurfaceMask airSurface =
        game::locomotorSurfaceBit(game::LocomotorSurface::Air);
    fact.hasCurrentLocomotor = locomotion ? uint8_t{1} : uint8_t{0};
    fact.groundMovement = locomotion &&
            (locomotion->surfaces & airSurface) == 0
        ? uint8_t{1}
        : uint8_t{0};
    fact.mobile = locomotion && fact.disabledMask == 0 && !fact.containedBy
        ? uint8_t{1}
        : uint8_t{0};
}

[[nodiscard]] uint64_t projectedObjectAIOrderRevision(
    const ecs::registry& registry, ecs::entity entity,
    const ObjectOrderQueueComponent* orders) noexcept {
    if (!orders) return 0;

    // privateFollowPathAppend mutates the active goal vector; it does not
    // replace the running AI command. The ECS queue still advances its
    // revision so replay/presentation can observe the appended node, but the
    // ObjectAI fact must retain the admitted head revision. Publishing the
    // mutable tail revision wakes consumers as though a new order replaced
    // FollowPath, which can terminate movement when a third point is added.
    const ObjectSystemPathSequenceComponent* path =
        ecs::try_get<ObjectSystemPathSequenceComponent>(registry, entity);
    if (path && path->activeQueueRevision != 0 &&
        path->routeSubtype == ObjectMoveRouteSubtype::FollowPath &&
        path->source == ObjectOrderSource::Player &&
        path->systemPurpose == ObjectOrderSystemPurpose::Generic &&
        !orders->orders.empty()) {
        const ObjectOrderIntent& head = orders->orders.front();
        if (head.kind == ObjectOrderKind::Move &&
            head.source == path->source &&
            head.systemPurpose == path->systemPurpose &&
            head.moveRouteSubtype == path->routeSubtype &&
            head.issuedTick == path->issuedTick &&
            head.sourceSequence == path->firstSourceSequence) {
            return path->activeQueueRevision;
        }
    }
    return orders->revision;
}

[[nodiscard]] bool guardStateRetaliatesAgainstAggressor(
    ai::AIStateId state) noexcept {
    return state == ai::AIStateId::Guard ||
        state == ai::AIStateId::GuardRetaliate ||
        state == ai::AIStateId::GuardTunnelNetwork;
}

// Value-only equivalent of hasAttackedMeAndICanReturnFire(): a live, hostile,
// attackable last-damage source that this actor is currently able to shoot
// back at. Returning INVALID_OBJECT_ID leaves the guard kernel's aggressor
// branch inert exactly as the original condition returning FALSE did.
[[nodiscard]] ObjectId recentGuardAggressor(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ecs::entity entity, ObjectId subject,
    ai::AIStateId state, bool ableToAttack, uint64_t confirmedTick) {
    if (!guardStateRetaliatesAgainstAggressor(state) || !ableToAttack)
        return INVALID_OBJECT_ID;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    if (!health || !health->hasLastDamageInfo ||
        health->lastDamageType == game::DamageType::HEALING ||
        !health->lastDamageSource ||
        health->lastDamageSource == subject ||
        health->lastDamageTick > confirmedTick ||
        confirmedTick - health->lastDamageTick >
            kGuardAggressorMemoryTicks) {
        return INVALID_OBJECT_ID;
    }
    // PendingDestroy is excluded by entityFromId(), matching the original
    // TheGameLogic->findObjectByID() rejection of a dead aggressor.
    const std::optional<ecs::entity> aggressor =
        lifecycle.entityFromId(health->lastDamageSource);
    if (!aggressor) return INVALID_OBJECT_ID;
    const ObjectHealthComponent* aggressorHealth =
        ecs::try_get<ObjectHealthComponent>(registry, *aggressor);
    if (aggressorHealth && aggressorHealth->effectivelyDead)
        return INVALID_OBJECT_ID;
    const ObjectContainedByComponent* aggressorContained =
        ecs::try_get<ObjectContainedByComponent>(registry, *aggressor);
    const ObjectStatusComponent* aggressorStatus =
        ecs::try_get<ObjectStatusComponent>(registry, *aggressor);
    const bool hiddenStealth = aggressorStatus &&
        aggressorStatus->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::Stealthed)) &&
        !aggressorStatus->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::Detected)) &&
        !aggressorStatus->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::Disguised));
    const ObjectKindOfComponent* aggressorKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, *aggressor);
    if (!objectAIAttackTargetPolicyAllowed({
            .containedPassenger =
                aggressorContained && aggressorContained->enclosing,
            .hiddenStealth = hiddenStealth,
            .allied = relationshipBetweenObjects(
                          registry, players, entity, *aggressor) !=
                      PlayerRelationship::Enemies,
            .unattackable = aggressorKinds && game::objectHasKind(
                aggressorKinds->mask, game::ObjectKindOf::Unattackable),
        })) {
        return INVALID_OBJECT_ID;
    }
    return health->lastDamageSource;
}

// Value-only equivalent of Object::isAbleToAttack() as this boundary already
// approximates it for recentGuardAggressor(): a weapon exists, the object is
// finished, on the field in its own right, and not disabled.
[[nodiscard]] bool objectAbleToAttackForRepulsor(
    const ecs::registry& registry, ecs::entity entity,
    uint64_t confirmedTick) {
    if (!ecs::try_get<ObjectWeaponComponent>(registry, entity) ||
        ecs::try_get<ObjectConstructionSiteComponent>(registry, entity)) {
        return false;
    }
    const ObjectContainedByComponent* contained =
        ecs::try_get<ObjectContainedByComponent>(registry, entity);
    if (contained && contained->enclosing) return false;
    return objectDisabledMask(registry, entity, confirmedTick) == 0;
}

// Value-only equivalent of AI::findClosestRepulsor() (AI.cpp:789):
// getClosestObject() inside vision range under PartitionFilterRepulsor
// (PartitionManager.cpp:5094) plus the stealth rejection filter. The flagged
// OBJECT_STATUS_REPULSOR branch is deliberately evaluated before relationship
// and kind, because a *friendly* damaged civilian is what scares the rest of
// the crowd (ActiveBody.cpp:682). Everything else only repulses while it is a
// live, attack-capable enemy that is neither inert nor a passive structure.
// Ties break on the lower ObjectId so the result never depends on spatial
// iteration order.
[[nodiscard]] ObjectId closestRepulsorInVision(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players,
    const container::Vector<ObjectSpatialRecord>& records, ecs::entity entity,
    ObjectId subject, const ai::AIFixedPosition& position,
    uint64_t confirmedTick) {
    const math::q32_32 range =
        effectiveObjectVisionRangeFixed(registry, entity);
    if (range <= math::q32_32{}) return INVALID_OBJECT_ID;
    const math::q32_32 rangeSquared = range * range;
    const math::q32_32 originX = math::q32_32::from_raw(position.xRaw);
    const math::q32_32 originY = math::q32_32::from_raw(position.yRaw);
    ObjectId closest = INVALID_OBJECT_ID;
    math::q32_32 bestDistanceSquared{};
    for (const ObjectSpatialRecord& record : records) {
        if (!record.object || record.object == subject) continue;
        const math::q32_32 dx = record.position.x - originX;
        const math::q32_32 dy = record.position.y - originY;
        const math::q32_32 distanceSquared = dx * dx + dy * dy;
        if (distanceSquared > rangeSquared) continue;
        if (closest &&
            (distanceSquared > bestDistanceSquared ||
             (distanceSquared == bestDistanceSquared &&
              closest < record.object))) {
            continue;
        }
        // PendingDestroy is excluded by entityFromId(), matching the original
        // partition query's rejection of an already removed object.
        const std::optional<ecs::entity> candidate =
            lifecycle.entityFromId(record.object);
        if (!candidate) continue;
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, *candidate);
        // PartitionFilterRejectByObjectStatus(STEALTHED, {DETECTED,DISGUISED})
        // wraps the whole query, flagged repulsors included.
        if (status &&
            status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::Stealthed)) &&
            !status->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::Detected) |
                game::objectStatusBit(game::ObjectStatusFlag::Disguised))) {
            continue;
        }
        const bool flagged = status &&
            status->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::Repulsor));
        if (!flagged) {
            const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(registry, *candidate);
            if (health && health->effectivelyDead) continue;
            if (relationshipBetweenObjects(registry, players, entity,
                                           *candidate) !=
                PlayerRelationship::Enemies) {
                continue;
            }
            // A structure is judged purely on whether it can shoot back;
            // otherwise KINDOF_INERT is rejected outright and every remaining
            // candidate must still be able to attack.
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(registry, *candidate);
            const bool structure = kinds &&
                game::objectHasKind(kinds->mask,
                                    game::ObjectKindOf::Structure);
            if (!structure && kinds &&
                game::objectHasKind(kinds->mask, game::ObjectKindOf::Inert)) {
                continue;
            }
            if (!objectAbleToAttackForRepulsor(registry, *candidate,
                                               confirmedTick)) {
                continue;
            }
        }
        closest = record.object;
        bestDistanceSquared = distanceSquared;
    }
    return closest;
}

} // namespace

void GameSessionAIShadowTransactions::run() {
    m_ai.m_objectAI.setPathfindCellSizeRaw(
        m_content.m_navigation.isInitialized()
            ? m_content.m_navigation.grid().transform().cellSizeRaw
            : 0);
    for (const ai::MovementFeedback& feedback :
         m_ai.m_objectAI.transients().movementFeedback()) {
        const bool alreadyPending = std::any_of(
            m_ai.m_objectAIMoveCompletions.begin(),
            m_ai.m_objectAIMoveCompletions.end(),
            [&feedback](const ai::PathCorrelation& pending) {
                return pending == feedback.correlation;
            });
        if (!alreadyPending && m_ai.m_objectAI.acceptsMoveToCompletion(feedback))
            m_ai.m_objectAIMoveCompletions.push_back(feedback.correlation);
    }
    auto& previousFacts =
        m_ai.m_objectAIShadowFacts;
    auto& nextFacts =
        m_ai.m_objectAIShadowNextFacts;
    nextFacts.clear();
    const auto actors = m_ai.m_objectAI.orderedSubjects();
    container::Vector<ObjectSpatialRecord> spatialRecords;
    bool spatialRecordsLoaded = false;
    const GameSessionAIOrderPolicy aiOrderPolicy{
        m_content, m_world, m_presentation};
    const container::SharedPtr<const game::terrain::MapVisibilitySnapshot>
        visibilitySnapshot = m_world.m_mapVisibility.snapshot();
    size_t previousCursor = 0;
    for (const ai::AIStateSoASubjectSlot& actor : actors) {
        ai::ObjectAIReadOnlyFact fact;
        fact.subject = actor.subject;
        while (previousCursor < previousFacts.size() &&
               previousFacts[previousCursor].subject < actor.subject) {
            ++previousCursor;
        }
        const ai::ObjectAIReadOnlyFact* previous =
            previousCursor < previousFacts.size() &&
                    previousFacts[previousCursor].subject == actor.subject
                ? &previousFacts[previousCursor]
                : nullptr;

        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(actor.subject);
        if (!entity) {
            // PendingDestroy is deliberately excluded by entityFromId(). A
            // stale lifecycle membership can therefore only enter terminal
            // Dead and can never emit a useful gameplay command.
            fact.effectivelyDead = 1;
            nextFacts.push_back(fact);
            continue;
        }
        const std::optional<ai::ObjectAIRecipeActorView> recipeBinding =
            m_ai.m_objectAI.recipeBinding(actor.subject);
        if (recipeBinding && recipeBinding->state ==
                ai::ObjectAIRecipeBindingState::ContentUnavailable) {
            // Missing/ambiguous mod content is a deterministic no-op, not a
            // partially executable generic actor.
            fact.effectivelyDead = 1;
            nextFacts.push_back(fact);
            continue;
        }

        const std::optional<ai::ObjectAIActorStateView> actorState =
            m_ai.m_objectAI.actorState(actor.subject);
        if (previous && actorState && actorState->sleepingAt(
                m_presentation.m_confirmedTick)) {
            // Cheap wake signature. Expensive tunnel/crate/idle-target
            // spatial queries remain copied from the previous immutable fact
            // until the authored wake deadline or a local semantic change.
            ai::ObjectAIReadOnlyFact probe = *previous;
            const TransformComponent* transform =
                ecs::try_get<TransformComponent>(
                    m_world.m_registry, *entity);
            probe.position = {};
            probe.positionValid = 0;
            if (transform) {
                const LogicFixedVec3 position =
                    readAuthoritativeObjectPosition(
                        m_world.m_registry, *entity,
                        *transform);
                probe.position = {
                    .xRaw = position.x.raw(),
                    .yRaw = position.y.raw(),
                    .zRaw = position.z.raw(),
                };
                probe.positionValid = 1;
            }
            const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(
                    m_world.m_registry, *entity);
            probe.effectivelyDead =
                health && health->effectivelyDead ? 1 : 0;
            probe.disabledMask = objectDisabledMask(
                m_world.m_registry, *entity,
                m_presentation.m_confirmedTick);
            const ObjectContainedByComponent* contained =
                ecs::try_get<ObjectContainedByComponent>(
                    m_world.m_registry, *entity);
            probe.containedBy = contained && contained->enclosing
                ? contained->container : INVALID_OBJECT_ID;

            const ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(
                    m_world.m_registry, *entity);
            projectLocomotionFacts(probe, locomotion);
            const ObjectProjectileComponent* projectile =
                ecs::try_get<ObjectProjectileComponent>(
                    m_world.m_registry, *entity);
            probe.projectile = projectile ? 1 : 0;
            const ObjectAirfieldComponent* airfield =
                ecs::try_get<ObjectAirfieldComponent>(
                    m_world.m_registry, *entity);
            probe.jetAI = airfield &&
                    ((!airfield->jetAi.empty()) ||
                     (airfield->plan && !airfield->plan->jetAi.empty()))
                ? 1 : 0;

            const ObjectOrderQueueComponent* orders =
                ecs::try_get<ObjectOrderQueueComponent>(
                    m_world.m_registry, *entity);
            probe.orderRevision = projectedObjectAIOrderRevision(
                m_world.m_registry, *entity, orders);
            probe.attackExitConditionSatisfied = 0;
            if (orders && !orders->orders.empty()) {
                const ObjectOrderIntent& head = orders->orders.front();
                probe.attackExitConditionSatisfied =
                    head.kind == ObjectOrderKind::Attack &&
                    head.maximumShots.has_value() &&
                    head.shotsFired >= *head.maximumShots ? 1 : 0;
            }
            const bool constructionComplete =
                ecs::try_get<ObjectConstructionSiteComponent>(
                    m_world.m_registry, *entity) ==
                nullptr;
            const ObjectWeaponComponent* weapons =
                ecs::try_get<ObjectWeaponComponent>(
                    m_world.m_registry, *entity);
            probe.weaponRevision = weapons
                ? static_cast<uint64_t>(weapons->weaponSetGeneration) : 0;
            const bool hasContainmentEdge =
                contained && contained->container;
            const bool passengerCanFire = hasContainmentEdge &&
                objectPassengerAllowedToFire(
                    m_world.m_registry, m_world.m_objects, *entity,
                    m_presentation.m_confirmedTick);
            const bool disabledAllowsAttackUpdate =
                probe.disabledMask == 0 ||
                (passengerCanFire &&
                 (probe.disabledMask & ~objectDisabledBit(
                     ObjectDisabledReason::Held)) == 0);
            const bool ownWeaponsAbleToAttack =
                objectOwnWeaponsAbleToAttack(
                    m_world.m_registry, m_world.m_objects,
                    m_content.m_contentSnapshot, *entity,
                    m_presentation.m_confirmedTick);
            if (const ObjectAITargetScanWakeComponent* wake =
                    ecs::try_get<ObjectAITargetScanWakeComponent>(
                        m_world.m_registry, *entity)) {
                probe.targetScanWakeRevision = wake->revision;
            }
            probe.capabilityMask = 0;
            if (locomotion) {
                probe.capabilityMask |= ai::objectAICapabilityBit(
                    ai::ObjectAICapability::GroundMovement);
                if (locomotion->minimumSpeed <= math::q32_32{}) {
                    probe.capabilityMask |= ai::objectAICapabilityBit(
                        ai::ObjectAICapability::CanTurnInPlace);
                }
            }
            if (projectile) {
                probe.capabilityMask |= ai::objectAICapabilityBit(
                    ai::ObjectAICapability::Projectile);
            }
            if (constructionComplete) {
                probe.capabilityMask |= ai::objectAICapabilityBit(
                    ai::ObjectAICapability::ConstructionComplete);
            }
            if (!objectIsOutOfAmmo(
                    m_world.m_registry, *entity,
                    m_content.m_contentSnapshot)) {
                probe.capabilityMask |= ai::objectAICapabilityBit(
                    ai::ObjectAICapability::HasAmmo);
            }
            if (ownWeaponsAbleToAttack && disabledAllowsAttackUpdate) {
                probe.capabilityMask |= ai::objectAICapabilityBit(
                    ai::ObjectAICapability::AttackMoodAllowed);
            }

            const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(
                    m_world.m_registry, *entity);
            const game::ObjectAIBehaviorPlan* behavior =
                type && type->archetype &&
                        type->archetype->aiBehaviorPlan
                    ? type->archetype->aiBehaviorPlan.get() : nullptr;
            if (behavior) {
                const uint64_t intervalNumerator =
                    static_cast<uint64_t>(
                        behavior->moodAttackCheckMilliseconds) *
                    static_cast<uint64_t>(std::max(
                        1, m_content.m_startInfo
                               .gameSpeedFPS));
                probe.idleTargetScanIntervalTicks =
                    static_cast<uint32_t>(std::min<uint64_t>(
                        std::numeric_limits<uint32_t>::max(),
                        std::max<uint64_t>(
                            1u, (intervalNumerator + 999u) / 1000u)));
            }
            const uint32_t acquireMask = behavior
                ? behavior->autoAcquireEnemiesWhenIdle : 0;
            const ObjectStatusComponent* sourceStatus =
                ecs::try_get<ObjectStatusComponent>(
                    m_world.m_registry, *entity);
            const bool sourceStealthed = sourceStatus &&
                sourceStatus->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::Stealthed));
            const ObjectStealthComponent* sourceStealth =
                ecs::try_get<ObjectStealthComponent>(
                    m_world.m_registry, *entity);
            const bool stealthGrantedBySpecialPower = sourceStealth &&
                sourceStealth->plan &&
                sourceStealth->plan->grantedBySpecialPower;
            const bool usingAbility = sourceStatus &&
                sourceStatus->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::IsUsingAbility));
            const bool attacking = sourceStatus &&
                sourceStatus->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::IsAttacking));
            const bool retargetWhileAttackingBlocked = attacking &&
                (acquireMask &
                 game::ObjectAIAutoAcquireNotWhileAttacking) != 0;
            probe.idleAutoAcquireEnabled = behavior && actorState->idle &&
                    !probe.effectivelyDead && ownWeaponsAbleToAttack &&
                    disabledAllowsAttackUpdate &&
                    (!probe.containedBy || passengerCanFire) &&
                    !usingAbility && !retargetWhileAttackingBlocked &&
                    (acquireMask & game::ObjectAIAutoAcquireYes) != 0 &&
                    (!sourceStealthed ||
                     stealthGrantedBySpecialPower || passengerCanFire ||
                     (acquireMask &
                      game::ObjectAIAutoAcquireWhileStealthed) != 0)
                ? 1 : 0;
            // A fresh aggressor must be part of the cheap wake signature:
            // a sleeping guard would otherwise never observe the hit and the
            // retaliation branch would stay dead until its next scan.
            probe.lastAggressor = recentGuardAggressor(
                m_world.m_registry, m_world.m_objects, m_content.m_players,
                *entity, actor.subject, actorState->state,
                ownWeaponsAbleToAttack && disabledAllowsAttackUpdate &&
                    constructionComplete,
                m_presentation.m_confirmedTick);
            // The repulsor reactor inputs must be part of the cheap wake
            // signature for the same reason the aggressor is: a sleeping
            // repulsable actor would otherwise never observe the tick a
            // repulsor appeared. KINDOF_CAN_BE_REPULSED and the locomotor
            // wander values are single-component reads, and the only spatial
            // query is gated on that authored kind, so the probe stays cheap.
            const ObjectKindOfComponent* probeKinds =
                ecs::try_get<ObjectKindOfComponent>(
                    m_world.m_registry, *entity);
            probe.canBeRepulsed = probeKinds &&
                    game::objectHasKind(probeKinds->mask,
                                        game::ObjectKindOf::CanBeRepulsed)
                ? 1 : 0;
            probe.wanderAboutPointRadiusRaw = locomotion
                ? locomotion->wanderAboutPointRadius.raw() : 0;
            probe.wanderWidthFactorRaw = locomotion
                ? locomotion->wanderWidthFactor.raw() : 0;
            probe.closestRepulsor = INVALID_OBJECT_ID;
            if (m_content.m_objectSimulationRules.ai.enableRepulsors &&
                probe.canBeRepulsed && probe.positionValid &&
                !probe.effectivelyDead) {
                if (!spatialRecordsLoaded) {
                    spatialRecords = m_world.m_spatialIndex.records();
                    spatialRecordsLoaded = true;
                }
                probe.closestRepulsor = closestRepulsorInVision(
                    m_world.m_registry, m_world.m_objects,
                    m_content.m_players, spatialRecords, *entity,
                    actor.subject, probe.position,
                    m_presentation.m_confirmedTick);
            }
            if (probe == *previous) {
                nextFacts.push_back(*previous);
                continue;
            }
        }

        fact = {};
        fact.subject = actor.subject;

        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
        if (transform) {
            const LogicFixedVec3 position = readAuthoritativeObjectPosition(
                m_world.m_registry, *entity, *transform);
            fact.position = {
                .xRaw = position.x.raw(),
                .yRaw = position.y.raw(),
                .zRaw = position.z.raw(),
            };
            fact.positionValid = 1;
        }

        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
        fact.effectivelyDead = health && health->effectivelyDead ? 1 : 0;
        fact.disabledMask = objectDisabledMask(
            m_world.m_registry, *entity, m_presentation.m_confirmedTick);

        const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(m_world.m_registry, *entity);
        // AI's `contained` column means an enclosed/off-world passenger, not
        // every structural host edge. Portable add-ons and station garrisons
        // keep independent attack/scan capability while transform ownership
        // remains in ObjectContainment.
        fact.containedBy = contained && contained->enclosing
            ? contained->container : INVALID_OBJECT_ID;

        // GuardTunnelNetwork needs a value-only nearest entrance at the
        // ECS→AI boundary. Resolve only this actor's controlling-player
        // network, like RefCode Player::getTunnelSystem(), and keep the
        // closest completed entrance deterministically.
        if (actorState &&
            actorState->state == ai::AIStateId::GuardTunnelNetwork) {
            if (!spatialRecordsLoaded) {
                spatialRecords =
                    m_world.m_spatialIndex.records();
                spatialRecordsLoaded = true;
            }
            const OwnerComponent* subjectOwner =
                ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
            const PlayerId tunnelOwner = subjectOwner
                ? subjectOwner->player : INVALID_PLAYER_ID;
            // AITNGuardMachine::lookForInnerTarget checks the authored Team
            // victim before TunnelTracker. Keep that priority in the same
            // detached value consumed by the guard kernel so a Team target
            // can replace the network nemesis without an ECS callback.
            const std::optional<ObjectTeamId> subjectTeam =
                m_world.m_objectTeams.teamOf(fact.subject);
            const ObjectTeamRecord* subjectTeamRecord = subjectTeam
                ? m_world.m_objectTeams.find(*subjectTeam) : nullptr;
            const scenario::ScriptTeamDefinition* subjectTeamDefinition =
                subjectTeamRecord && subjectTeamRecord->scenarioDefinition &&
                    m_presentation.m_scenarioDefinition
                ? m_presentation.m_scenarioDefinition->findScriptTeam(
                      subjectTeamRecord->scenarioDefinition)
                : nullptr;
            const ObjectId commonTarget = subjectTeamDefinition &&
                    subjectTeamDefinition->plan.attackCommonTarget
                ? subjectTeamRecord->commonTarget : INVALID_OBJECT_ID;
            const std::optional<ecs::entity> commonTargetEntity = commonTarget
                ? m_world.m_objects.entityFromId(commonTarget) : std::nullopt;
            const ObjectHealthComponent* commonTargetHealth =
                commonTargetEntity
                ? ecs::try_get<ObjectHealthComponent>(
                      m_world.m_registry, *commonTargetEntity)
                : nullptr;
            if (commonTargetEntity &&
                (!commonTargetHealth ||
                 !commonTargetHealth->effectivelyDead) &&
                relationshipBetweenObjects(
                    m_world.m_registry, m_content.m_players,
                    *entity, *commonTargetEntity) ==
                    PlayerRelationship::Enemies &&
                !objectHiddenFromObserverForAcquisition(
                    m_world.m_registry, m_world.m_objects,
                    m_content.m_players, *entity,
                    *commonTargetEntity)) {
                fact.priorityNemesis = commonTarget;
            }
            bool fixedToContainingEntrance = false;
            if (fact.containedBy) {
                const std::optional<ecs::entity> enclosing =
                    m_world.m_objects.entityFromId(fact.containedBy);
                const ObjectContainmentRuntimeComponent* enclosingRuntime =
                    enclosing ? ecs::try_get<ObjectContainmentRuntimeComponent>(
                        m_world.m_registry, *enclosing) : nullptr;
                const OwnerComponent* enclosingOwner = enclosing
                    ? ecs::try_get<OwnerComponent>(m_world.m_registry, *enclosing)
                    : nullptr;
                const ObjectMapStatusComponent* enclosingMapStatus = enclosing
                    ? ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry,
                                                              *enclosing)
                    : nullptr;
                const ObjectStatusComponent* enclosingStatus = enclosing
                    ? ecs::try_get<ObjectStatusComponent>(m_world.m_registry,
                                                           *enclosing)
                    : nullptr;
                const ObjectHealthComponent* enclosingHealth = enclosing
                    ? ecs::try_get<ObjectHealthComponent>(m_world.m_registry,
                                                           *enclosing)
                    : nullptr;
                if (enclosingRuntime && enclosingRuntime->plan &&
                    tunnelOwner && enclosingOwner &&
                    enclosingOwner->player == tunnelOwner &&
                    (!enclosingMapStatus || !enclosingMapStatus->offMap) &&
                    (!enclosingStatus || !enclosingStatus->hasAny(
                        game::objectStatusBit(
                            game::ObjectStatusFlag::UnderConstruction) |
                        game::objectStatusBit(game::ObjectStatusFlag::Sold))) &&
                    (!enclosingHealth || !enclosingHealth->effectivelyDead) &&
                    !ecs::try_get<ObjectConstructionSiteComponent>(
                        m_world.m_registry, *enclosing) &&
                    std::any_of(enclosingRuntime->plan->rules.begin(),
                                enclosingRuntime->plan->rules.end(),
                                [](const ObjectContainmentRule& rule) {
                                    return rule.kind == ObjectContainmentKind::Tunnel;
                                })) {
                    fact.nearestTunnel = fact.containedBy;
                    fixedToContainingEntrance = true;
                }
            }
            math::q32_32 bestDistanceSquared{};
            bool foundNearestEntrance = false;
            for (const ObjectSpatialRecord& record : spatialRecords) {
                if (fixedToContainingEntrance || !tunnelOwner ||
                    !fact.positionValid) break;
                if (!record.object || record.object == fact.subject) continue;
                const std::optional<ecs::entity> candidate =
                    m_world.m_objects.entityFromId(record.object);
                if (!candidate) continue;
                const OwnerComponent* candidateOwner =
                    ecs::try_get<OwnerComponent>(m_world.m_registry, *candidate);
                if (!candidateOwner || candidateOwner->player != tunnelOwner)
                    continue;
                const ObjectContainmentRuntimeComponent* runtime =
                    ecs::try_get<ObjectContainmentRuntimeComponent>(
                        m_world.m_registry, *candidate);
                if (!runtime || !runtime->plan) continue;
                const bool tunnel = std::any_of(
                    runtime->plan->rules.begin(), runtime->plan->rules.end(),
                    [](const ObjectContainmentRule& rule) {
                        return rule.kind == ObjectContainmentKind::Tunnel;
                    });
                if (!tunnel) continue;
                const ObjectMapStatusComponent* mapStatus =
                    ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, *candidate);
                if (mapStatus && mapStatus->offMap) continue;
                const ObjectStatusComponent* candidateStatus =
                    ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *candidate);
                if (candidateStatus && candidateStatus->hasAny(
                        game::objectStatusBit(
                            game::ObjectStatusFlag::UnderConstruction) |
                        game::objectStatusBit(game::ObjectStatusFlag::Sold)))
                    continue;
                const ObjectHealthComponent* health =
                    ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *candidate);
                if (health && health->effectivelyDead)
                    continue;
                if (ecs::try_get<ObjectConstructionSiteComponent>(
                        m_world.m_registry, *candidate))
                    continue;
                const math::q32_32 dx =
                    record.position.x -
                    math::q32_32::from_raw(fact.position.xRaw);
                const math::q32_32 dy =
                    record.position.y -
                    math::q32_32::from_raw(fact.position.yRaw);
                const math::q32_32 distanceSquared = dx * dx + dy * dy;
                if (!foundNearestEntrance ||
                    distanceSquared < bestDistanceSquared ||
                    (distanceSquared == bestDistanceSquared &&
                     (!fact.nearestTunnel || record.object < fact.nearestTunnel))) {
                    fact.nearestTunnel = record.object;
                    bestDistanceSquared = distanceSquared;
                    foundNearestEntrance = true;
                }
            }
            if (!fact.priorityNemesis && fact.nearestTunnel) {
                fact.priorityNemesis =
                    m_world.m_objectSimulation.recentTunnelNetworkNemesis(
                        m_world.m_registry, m_world.m_objects, fact.nearestTunnel,
                        m_presentation.m_confirmedTick);
            }
        }

        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *entity);
        projectLocomotionFacts(fact, locomotion);

        const bool guardCanSeekCrates = actorState && fact.mobile &&
            (actorState->state == ai::AIStateId::Guard ||
             actorState->state == ai::AIStateId::GuardRetaliate ||
             actorState->state == ai::AIStateId::GuardTunnelNetwork);
        if (guardCanSeekCrates && fact.positionValid) {
            if (!spatialRecordsLoaded) {
                spatialRecords =
                    m_world.m_spatialIndex.records();
                spatialRecordsLoaded = true;
            }
            math::q32_32 bestDistanceSquared{};
            bool foundCrate = false;
            for (const ObjectSpatialRecord& record : spatialRecords) {
                if (!record.object || record.object == fact.subject ||
                    !canObjectAIAutonomouslyPickUpCrate(
                        m_world.m_registry,
                        m_world.m_objects,
                        m_content.m_terrain,
                        m_content.m_players,
                        m_content.m_objectSimulationRules,
                        fact.subject, record.object)) {
                    continue;
                }
                const math::q32_32 x = record.position.x;
                const math::q32_32 y = record.position.y;
                const math::q32_32 dx =
                    x - math::q32_32::from_raw(fact.position.xRaw);
                const math::q32_32 dy =
                    y - math::q32_32::from_raw(fact.position.yRaw);
                const math::q32_32 distanceSquared = dx * dx + dy * dy;
                if (!foundCrate || distanceSquared < bestDistanceSquared ||
                    (distanceSquared == bestDistanceSquared &&
                     record.object < fact.pickupCrate)) {
                    fact.pickupCrate = record.object;
                    fact.pickupCratePosition = {
                        .xRaw = x.raw(),
                        .yRaw = y.raw(),
                        .zRaw = record.position.z.raw(),
                    };
                    fact.pickupCratePositionValid = 1;
                    bestDistanceSquared = distanceSquared;
                    foundCrate = true;
                }
            }
        }

        const ObjectProjectileComponent* projectile =
            ecs::try_get<ObjectProjectileComponent>(m_world.m_registry, *entity);
        fact.projectile = projectile ? 1 : 0;
        const ObjectAirfieldComponent* airfield =
            ecs::try_get<ObjectAirfieldComponent>(m_world.m_registry, *entity);
        fact.jetAI = airfield &&
                ((!airfield->jetAi.empty()) ||
                 (airfield->plan && !airfield->plan->jetAi.empty()))
            ? 1 : 0;

        const ObjectOrderQueueComponent* orders =
            ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity);
        fact.orderRevision = projectedObjectAIOrderRevision(
            m_world.m_registry, *entity, orders);
        if (orders && !orders->orders.empty()) {
            const ObjectOrderIntent& head = orders->orders.front();
            fact.attackExitConditionSatisfied =
                head.kind == ObjectOrderKind::Attack &&
                head.maximumShots.has_value() &&
                head.shotsFired >= *head.maximumShots
                    ? uint8_t{1}
                    : uint8_t{0};
        }

        const bool constructionComplete =
            ecs::try_get<ObjectConstructionSiteComponent>(m_world.m_registry, *entity) == nullptr;
        const ObjectWeaponComponent* weapons =
            ecs::try_get<ObjectWeaponComponent>(m_world.m_registry, *entity);
        fact.weaponRevision = weapons
            ? static_cast<uint64_t>(weapons->weaponSetGeneration)
            : 0;
        const bool hasContainmentEdge = contained && contained->container;
        const bool passengerCanFire = hasContainmentEdge &&
            objectPassengerAllowedToFire(
                m_world.m_registry, m_world.m_objects, *entity,
                m_presentation.m_confirmedTick);
        const bool disabledAllowsAttackUpdate = fact.disabledMask == 0 ||
            (passengerCanFire &&
             (fact.disabledMask & ~objectDisabledBit(
                 ObjectDisabledReason::Held)) == 0);
        const bool ownWeaponsAbleToAttack = objectOwnWeaponsAbleToAttack(
            m_world.m_registry, m_world.m_objects,
            m_content.m_contentSnapshot, *entity,
            m_presentation.m_confirmedTick);
        if (locomotion) {
            fact.capabilityMask |= ai::objectAICapabilityBit(
                ai::ObjectAICapability::GroundMovement);
            // RefCode AIFaceState derives this from the current
            // Locomotor::getMinSpeed(), not merely from locomotor presence.
            if (locomotion->minimumSpeed <= math::q32_32{}) {
                fact.capabilityMask |= ai::objectAICapabilityBit(
                    ai::ObjectAICapability::CanTurnInPlace);
            }
        }
        if (projectile)
            fact.capabilityMask |= ai::objectAICapabilityBit(
                ai::ObjectAICapability::Projectile);
        if (constructionComplete)
            fact.capabilityMask |= ai::objectAICapabilityBit(
                ai::ObjectAICapability::ConstructionComplete);
        if (!objectIsOutOfAmmo(
                m_world.m_registry, *entity,
                m_content.m_contentSnapshot))
            fact.capabilityMask |= ai::objectAICapabilityBit(
                ai::ObjectAICapability::HasAmmo);
        if (ownWeaponsAbleToAttack && disabledAllowsAttackUpdate)
            fact.capabilityMask |= ai::objectAICapabilityBit(
                ai::ObjectAICapability::AttackMoodAllowed);

        fact.lastAggressor = recentGuardAggressor(
            m_world.m_registry, m_world.m_objects, m_content.m_players,
            *entity, fact.subject,
            actorState ? actorState->state : ai::AIStateId::Invalid,
            ownWeaponsAbleToAttack && disabledAllowsAttackUpdate &&
                constructionComplete,
            m_presentation.m_confirmedTick);

        // Repulsor reactor half. Every legacy repulsor branch tests
        // KINDOF_CAN_BE_REPULSED before it looks for a repulsor at all
        // (AIStates.cpp:1403, 4594, 4719, 4833), and Object::Object() only
        // builds the repulsor helper for that kind (Object.cpp:353), so the
        // vision-range scan below is confined to authored civilians.
        const ObjectKindOfComponent* subjectKinds =
            ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity);
        fact.canBeRepulsed = subjectKinds &&
                game::objectHasKind(subjectKinds->mask,
                                    game::ObjectKindOf::CanBeRepulsed)
            ? 1 : 0;
        if (locomotion) {
            // AIWanderInPlaceState uses WanderAboutPointRadius; AIWanderState
            // and AIPanicState use WanderWidthFactor. Both stay authored raw
            // Q32.32 here; the AI runtime converts to pathfind cells because
            // Navigation owns that scalar.
            fact.wanderAboutPointRadiusRaw =
                locomotion->wanderAboutPointRadius.raw();
            fact.wanderWidthFactorRaw = locomotion->wanderWidthFactor.raw();
        }
        if (m_content.m_objectSimulationRules.ai.enableRepulsors &&
            fact.canBeRepulsed && fact.positionValid &&
            !fact.effectivelyDead) {
            if (!spatialRecordsLoaded) {
                spatialRecords = m_world.m_spatialIndex.records();
                spatialRecordsLoaded = true;
            }
            fact.closestRepulsor = closestRepulsorInVision(
                m_world.m_registry, m_world.m_objects, m_content.m_players,
                spatialRecords, *entity, fact.subject, fact.position,
                m_presentation.m_confirmedTick);
        }

        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(
                m_world.m_registry, *entity);
        const game::ObjectAIBehaviorPlan* behavior =
            type && type->archetype && type->archetype->aiBehaviorPlan
            ? type->archetype->aiBehaviorPlan.get() : nullptr;
        if (const ObjectAITargetScanWakeComponent* wake =
                ecs::try_get<ObjectAITargetScanWakeComponent>(
                    m_world.m_registry, *entity)) {
            fact.targetScanWakeRevision = wake->revision;
        }
        if (behavior) {
            const uint64_t intervalNumerator =
                static_cast<uint64_t>(behavior->moodAttackCheckMilliseconds) *
                static_cast<uint64_t>(std::max(
                    1, m_content.m_startInfo.gameSpeedFPS));
            fact.idleTargetScanIntervalTicks = static_cast<uint32_t>(
                std::min<uint64_t>(
                    std::numeric_limits<uint32_t>::max(),
                    std::max<uint64_t>(1u,
                        (intervalNumerator + 999u) / 1000u)));
        }
        const uint32_t acquireMask = behavior
            ? behavior->autoAcquireEnemiesWhenIdle : 0;
        const ObjectStatusComponent* sourceStatus =
            ecs::try_get<ObjectStatusComponent>(
                m_world.m_registry, *entity);
        const ObjectMapStatusComponent* sourceMapStatus =
            ecs::try_get<ObjectMapStatusComponent>(
                m_world.m_registry, *entity);
        const bool sourceOffMap = sourceMapStatus && sourceMapStatus->offMap;
        const bool sourceStealthed = sourceStatus && sourceStatus->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Stealthed));
        const ObjectStealthComponent* sourceStealth =
            ecs::try_get<ObjectStealthComponent>(
                m_world.m_registry, *entity);
        const bool stealthGrantedBySpecialPower = sourceStealth &&
            sourceStealth->plan &&
            sourceStealth->plan->grantedBySpecialPower;
        const bool usingAbility = sourceStatus && sourceStatus->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::IsUsingAbility));
        const bool attacking = sourceStatus && sourceStatus->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::IsAttacking));
        const bool retargetWhileAttackingBlocked = attacking &&
            (acquireMask &
             game::ObjectAIAutoAcquireNotWhileAttacking) != 0;
        const bool autoAcquireEnabled = behavior && actorState &&
            actorState->state == ai::AIStateId::Idle &&
            !fact.effectivelyDead && ownWeaponsAbleToAttack &&
            disabledAllowsAttackUpdate &&
            (!fact.containedBy || passengerCanFire) &&
            !usingAbility && !retargetWhileAttackingBlocked &&
            (acquireMask & game::ObjectAIAutoAcquireYes) != 0 &&
            (!sourceStealthed ||
             stealthGrantedBySpecialPower || passengerCanFire ||
             (acquireMask &
              game::ObjectAIAutoAcquireWhileStealthed) != 0);
        fact.idleAutoAcquireEnabled = autoAcquireEnabled ? 1u : 0u;
        if (autoAcquireEnabled && fact.positionValid) {
            const OwnerComponent* sourceOwner =
                ecs::try_get<OwnerComponent>(
                    m_world.m_registry, *entity);
            const PlayerState* sourcePlayer = sourceOwner
                ? m_content.m_players.get(sourceOwner->player)
                : nullptr;
            const bool humanControlled = sourcePlayer &&
                sourcePlayer->controller == PlayerControllerKind::Human;
            const bool computerControlled = sourcePlayer &&
                sourcePlayer->controller == PlayerControllerKind::Ai;
            const game::terrain::MapVisibilitySnapshot* visibility =
                humanControlled ? visibilitySnapshot.get() : nullptr;
            ObjectId commonTeamTarget = INVALID_OBJECT_ID;
            const std::optional<ObjectTeamId> objectTeam =
                m_world.m_objectTeams.teamOf(fact.subject);
            const ObjectTeamRecord* teamRecord = objectTeam
                ? m_world.m_objectTeams.find(*objectTeam) : nullptr;
            const scenario::ScriptTeamDefinition* teamDefinition =
                teamRecord && teamRecord->scenarioDefinition &&
                    m_presentation.m_scenarioDefinition
                ? m_presentation.m_scenarioDefinition->findScriptTeam(
                      teamRecord->scenarioDefinition)
                : nullptr;
            const ObjectAIBehaviorPolicyComponent* sourcePolicy =
                ecs::try_get<ObjectAIBehaviorPolicyComponent>(
                    m_world.m_registry, *entity);
            if (teamDefinition &&
                teamDefinition->plan.attackCommonTarget &&
                (!sourcePolicy || sourcePolicy->attitude >=
                    ObjectAIAttitude::Normal)) {
                commonTeamTarget = teamRecord->commonTarget;
            }
            math::q32_32 range = effectiveObjectVisionRangeFixed(
                m_world.m_registry, *entity);
            range *= m_content.m_objectSimulationRules.ai
                .guardOuterModifier(humanControlled);
            if (computerControlled && sourcePolicy) {
                if (sourcePolicy->attitude == ObjectAIAttitude::Sleep) {
                    range = {};
                } else if (sourcePolicy->attitude ==
                           ObjectAIAttitude::Alert) {
                    range *= m_content.m_objectSimulationRules.ai
                        .alertRangeModifier;
                } else if (sourcePolicy->attitude ==
                           ObjectAIAttitude::Aggressive) {
                    range *= m_content.m_objectSimulationRules.ai
                        .aggressiveRangeModifier;
                }
            }
            // RefCode extends a contained actor's search radius by its host's
            // bounding circle. It does not grant an unbounded scan.
            if (hasContainmentEdge) {
                const std::optional<ecs::entity> host =
                    m_world.m_objects.entityFromId(contained->container);
                const ObjectGeometryComponent* hostGeometry = host
                    ? ecs::try_get<ObjectGeometryComponent>(
                          m_world.m_registry, *host)
                    : nullptr;
                if (hostGeometry) {
                    range += math::q32_32::max(
                        math::q32_32{},
                        hostGeometry->boundingCircleRadiusFixed);
                }
            }
            if (range > math::q32_32{}) {
                if (!spatialRecordsLoaded) {
                    spatialRecords =
                        m_world.m_spatialIndex.records();
                    spatialRecordsLoaded = true;
                }
                bool passive = false;
                const std::optional<ObjectId> retaliationTarget =
                    aiOrderPolicy.passiveRetaliationTarget(
                        fact.subject, passive);
                ObjectAIOpportunityTargetSelection targetSelection;
                for (const ObjectSpatialRecord& record : spatialRecords) {
                    if (!record.object || record.object == fact.subject)
                        continue;
                    if (passive && (!retaliationTarget ||
                                    record.object != *retaliationTarget)) {
                        continue;
                    }
                    const bool preferredTeamTarget =
                        record.object == commonTeamTarget;
                    const std::optional<ecs::entity> candidate =
                        m_world.m_objects.entityFromId(
                            record.object);
                    if (!candidate) continue;
                    const ObjectLifecycleComponent* candidateLifecycle =
                        ecs::try_get<ObjectLifecycleComponent>(
                            m_world.m_registry, *candidate);
                    const ObjectMapStatusComponent* candidateMapStatus =
                        ecs::try_get<ObjectMapStatusComponent>(
                            m_world.m_registry, *candidate);
                    if (!candidateLifecycle ||
                        candidateLifecycle->phase !=
                            ObjectLifecyclePhase::Alive ||
                        m_world.m_objects.isPendingDestroy(record.object) ||
                        ((candidateMapStatus && candidateMapStatus->offMap) !=
                         sourceOffMap)) {
                        continue;
                    }
                    const ObjectHealthComponent* candidateHealth =
                        ecs::try_get<ObjectHealthComponent>(
                            m_world.m_registry,
                            *candidate);
                    if (candidateHealth && candidateHealth->effectivelyDead)
                        continue;
                    const ObjectContainedByComponent* candidateContained =
                        ecs::try_get<ObjectContainedByComponent>(
                            m_world.m_registry,
                            *candidate);
                    if (candidateContained && candidateContained->enclosing)
                        continue;
                    const ObjectStatusComponent* candidateStatus =
                        ecs::try_get<ObjectStatusComponent>(
                            m_world.m_registry,
                            *candidate);
                    const bool hiddenStealth = candidateStatus &&
                        candidateStatus->hasAny(game::objectStatusBit(
                            game::ObjectStatusFlag::Stealthed)) &&
                        !candidateStatus->hasAny(game::objectStatusBit(
                            game::ObjectStatusFlag::Detected)) &&
                        !candidateStatus->hasAny(game::objectStatusBit(
                            game::ObjectStatusFlag::Disguised));
                    const ObjectKindOfComponent* candidateKinds =
                        ecs::try_get<ObjectKindOfComponent>(
                            m_world.m_registry,
                            *candidate);
                    const ObjectCombatTargetability targetability =
                        queryObjectCombatTargetability(
                            m_world.m_registry, m_world.m_objects,
                            m_content.m_contentSnapshot, fact.subject,
                            record.object,
                            m_presentation.m_confirmedTick);
                    if (!targetability.canAttack) {
                        continue;
                    }
                    if (!objectAIAttackTargetPolicyAllowed({
                            .hiddenStealth = hiddenStealth,
                            .allied = relationshipBetweenObjects(
                                m_world.m_registry,
                                m_content.m_players,
                                *entity, *candidate) !=
                                PlayerRelationship::Enemies,
                            .unattackable = candidateKinds &&
                                game::objectHasKind(
                                    candidateKinds->mask,
                                    game::ObjectKindOf::Unattackable),
                        })) {
                        continue;
                    }
                    const bool building = candidateKinds &&
                        game::objectHasKind(
                            candidateKinds->mask,
                            game::ObjectKindOf::Structure);
                    if (m_content.m_objectSimulationRules.ai
                            .attackIgnoreInsignificantBuildings &&
                        !preferredTeamTarget &&
                        isObjectAIIgnoredInsignificantBuilding(
                            m_world.m_registry, *candidate,
                            candidateKinds)) {
                        continue;
                    }
                    const bool buildingAbleToAttack =
                        objectOwnWeaponsAbleToAttack(
                            m_world.m_registry, m_world.m_objects,
                            m_content.m_contentSnapshot, *candidate,
                            m_presentation.m_confirmedTick) ||
                        (candidateStatus && candidateStatus->hasAny(
                            game::objectStatusBit(
                                game::ObjectStatusFlag::CanAttack)));
                    if (building && !preferredTeamTarget &&
                        !buildingAbleToAttack &&
                        (acquireMask &
                         game::ObjectAIAutoAcquireAttackBuildings) == 0) {
                        continue;
                    }
                    if (humanControlled &&
                        !targetability.withinAnyWeaponRange) {
                        continue;
                    }
                    if (humanControlled) {
                        const ObjectGeometryComponent* candidateGeometry =
                            ecs::try_get<ObjectGeometryComponent>(
                                m_world.m_registry, *candidate);
                        const math::q32_32 candidateRadius = candidateGeometry
                            ? math::q32_32::max(
                                  math::q32_32{},
                                  candidateGeometry->boundingCircleRadiusFixed)
                            : math::q32_32{};
                        if (visibility && visibility->renderingActive &&
                            (!sourceOwner ||
                             !visibility->footprintHasClearCellRaw(
                                 sourceOwner->player,
                                 record.position.x.raw(),
                                 record.position.y.raw(),
                                 candidateRadius.raw()))) {
                            continue;
                        }
                    }
                    if (preferredTeamTarget) {
                        fact.idleAutoAcquireTarget = record.object;
                        break;
                    }
                    if (passive) {
                        fact.idleAutoAcquireTarget = record.object;
                        break;
                    }
                    considerObjectAIOpportunityTarget(
                        ai::AIOpportunityAttackMoveQueryCommandKind::
                            FindMoodTarget,
                        fact.position,
                        ObjectAIOpportunityTargetCandidate{
                            .target = record.object,
                            .position = {
                                .xRaw = record.position.x.raw(),
                                .yRaw = record.position.y.raw(),
                                .zRaw = record.position.z.raw(),
                            },
                            .attackable = true,
                            .enemy = true,
                            .attackPriority =
                                aiOrderPolicy.attackPriorityForTarget(
                                    fact.subject, *candidate),
                            .attackPriorityDistanceModifierRaw =
                                m_content.m_objectSimulationRules.ai
                                    .attackPriorityDistanceModifier.raw(),
                            .maximumAcquisitionDistanceRaw = range.raw(),
                        },
                        targetSelection);
                }
                if (!fact.idleAutoAcquireTarget)
                    fact.idleAutoAcquireTarget = targetSelection.target;
            }
        }

        nextFacts.push_back(fact);
    }

    previousFacts.swap(nextFacts);

    // RefCode AIIdleState reacts to a visible repulsor even though no
    // external order exists. Stage the same internal transition through this
    // transaction owner before the shadow wave; ObjectAIRuntime mints the
    // state-machine correlation used by Safe-path feedback.
    if (m_content.m_objectSimulationRules.ai.enableRepulsors) {
        for (const ai::ObjectAIReadOnlyFact& fact : previousFacts) {
            if (!fact.canBeRepulsed || !fact.mobile ||
                fact.effectivelyDead || !fact.closestRepulsor) {
                continue;
            }
            const std::optional<ai::ObjectAIActorStateView> state =
                m_ai.m_objectAI.actorState(fact.subject);
            if (state && state->state == ai::AIStateId::Idle) {
                static_cast<void>(m_ai.m_objectAI.activateRepulsorEscape(
                    fact.subject, m_presentation.m_confirmedTick));
            }
        }
    }

    const ai::ObjectAIShadowTickReport report = m_ai.m_objectAI.runShadow(
        m_presentation.m_confirmedTick,
        static_cast<uint32_t>(std::max(1, m_content.m_startInfo.gameSpeedFPS)),
        m_ai.m_objectAIShadowFacts);
    if (!report.succeeded()) {
        const bool capacityFailure =
            report.outputOverflows != 0 ||
            report.transitionCapacityRejections != 0 ||
            report.feedbackOverflows != 0 ||
            report.status ==
                ai::ObjectAIShadowTickStatus::WakeProjectionRejected;
        static_cast<void>(m_publication.raiseSimulationFault({
            .domain = SimulationFaultDomain::ObjectAI,
            .code = report.status ==
                        ai::ObjectAIShadowTickStatus::NonMonotonicTick
                ? SimulationFaultCode::InvalidConfirmedTick
                : capacityFailure ? SimulationFaultCode::CapacityExceeded
                                  : SimulationFaultCode::InvalidEvent,
            .confirmedTick =
                m_presentation.m_confirmedTick,
        }));
        TD_LOG_ERROR(
            "[GameSession] Object AI shadow phase rejected tick {}: status={} "
            "spanRejects={} transitionCapacityRejects={} blockedExits={} "
            "outputOverflows={} feedbackOverflows={} feedbackRejected={} "
            "stagingRejects={} actorsScheduled={} batchesRun={}",
            m_presentation.m_confirmedTick, static_cast<uint32_t>(report.status),
            report.spanRejections, report.transitionCapacityRejections,
            report.blockedExits, report.outputOverflows,
            report.feedbackOverflows, report.feedbackRejected,
            report.outputStagingFailures, report.actorsScheduled,
            report.batchesRun);
    }
    if (report.succeeded() && report.outputStagingFailures != 0) {
        TD_LOG_DEBUG(
            "[GameSession] Object AI discarded {} stale/unsupported shadow outputs at tick {}",
            report.outputStagingFailures,
            m_presentation.m_confirmedTick);
    }
    if (report.succeeded()) {
        GameSessionAIMoveOrderTransactions moveOrders{
            m_content, m_world,
            m_ai, m_presentation,
            m_publication,
            m_frame};
        moveOrders.commitWaypointCompletions();
        moveOrders.commitMoveCompletions();
    }
}

void GameSessionAIMoveOrderTransactions::commitMoveCompletions() {
    size_t retainedCount = 0;
    const auto retain = [this, &retainedCount](
                            const ai::PathCorrelation& correlation) {
        m_ai.m_objectAIMoveCompletions[retainedCount++] = correlation;
    };
    for (const ai::PathCorrelation& correlation :
         m_ai.m_objectAIMoveCompletions) {
        const std::optional<ai::ObjectAIActorStateView> actorState =
            m_ai.m_objectAI.actorState(correlation.subject);
        // A direct MoveTo reaches Idle in the same multiwave that consumes
        // Completed. AttackMove may receive movement completion while its
        // opportunity query is still pending; its wrapper records that
        // terminal result and must finish the query/child transaction before
        // the one Move queue identity can be committed.
        if (!actorState || actorState->state != ai::AIStateId::Idle) {
            retain(correlation);
            continue;
        }
        const std::optional<ecs::entity> entity =
            m_world.m_objects.entityFromId(correlation.subject);
        ObjectOrderQueueComponent* queue = entity
            ? ecs::try_get<ObjectOrderQueueComponent>(m_world.m_registry, *entity)
            : nullptr;
        if (!queue || queue->orders.empty())
            continue;
        const ObjectOrderIntent& order = queue->orders.front();
        const ai::AIAsyncOrderIdentity& expected =
            correlation.orderIdentity;
        const bool matches =
            order.kind == ObjectOrderKind::Move &&
            expected.subject == correlation.subject &&
            expected.queueRevision == queue->revision &&
            expected.externalRevision == queue->externalRevision &&
            expected.issuedTick == order.issuedTick &&
            expected.sourceSequence == order.sourceSequence &&
            expected.sourceScriptId == order.sourceScriptId &&
            expected.systemPurposeInstance == order.systemPurposeInstance &&
            expected.source == static_cast<uint8_t>(order.source) &&
            expected.systemPurpose ==
                static_cast<uint8_t>(order.systemPurpose);
        if (!matches)
            continue;

        const ai::ObjectAIOrderAdmissionResult completed =
            m_ai.m_objectAI.completeMoveOrder(
                correlation.subject, expected,
                ai::ObjectAIOrderCompletion::Success);
        if (!completed.succeeded()) {
            TD_LOG_ERROR(
                "[GameSession] Object AI Move completion rejected: "
                "subject={} tick={} status={}",
                correlation.subject.value, m_presentation.m_confirmedTick,
                static_cast<uint32_t>(completed.status));
            retain(correlation);
            continue;
        }
        queue->orders.erase(queue->orders.begin());
        ++queue->revision;
    }
    m_ai.m_objectAIMoveCompletions.resize(retainedCount);
}

} // namespace engine
