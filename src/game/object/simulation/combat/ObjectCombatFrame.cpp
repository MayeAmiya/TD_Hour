#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/combat/ObjectWeaponDamage.h"
#include "game/navigation/runtime/NavigationSystem.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>
#include "game/object/simulation/combat/ObjectCombatDetail.h"
#include "game/object/component/ObjectDirty.h"

namespace engine {

using namespace object_combat_detail;

namespace {

[[nodiscard]] uint64_t jetLockonDurationTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0 || framesPerSecond == 0) return 0;
    const uint64_t scaled = static_cast<uint64_t>(milliseconds) *
        static_cast<uint64_t>(framesPerSecond);
    return std::max<uint64_t>(1u, (scaled + 999u) / 1000u);
}

struct JetLockonOwner final {
    ObjectJetAiRuntime* runtime = nullptr;
    const game::ObjectJetAiRule* rule = nullptr;
};

[[nodiscard]] JetLockonOwner jetLockonOwner(
    ecs::registry& registry, ecs::entity entity) noexcept {
    ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, entity);
    if (!component || !component->plan) return {};
    const size_t count = std::min(component->jetAi.size(),
                                  component->plan->jetAi.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectJetAiRule& rule = component->plan->jetAi[index];
        if (rule.lockonMilliseconds != 0)
            return {&component->jetAi[index], &rule};
    }
    return {};
}

void removeJetLockonTargeter(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId target, ObjectId targeter) {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(target);
    if (!entity) return;
    JetLockonOwner owner = jetLockonOwner(registry, *entity);
    if (!owner.runtime) return;
    auto& targeters = owner.runtime->lockonTargeters;
    const auto position = std::lower_bound(
        targeters.begin(), targeters.end(), targeter);
    if (position != targeters.end() && *position == targeter)
        targeters.erase(position);
    if (targeters.empty()) owner.runtime->lockonReadyTick = 0;
}

void reconcileJetLockonTargeter(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId targeter, ObjectWeaponComponent& weapons,
    ObjectId desiredTarget, uint64_t confirmedTick,
    uint32_t framesPerSecond) {
    if (weapons.jetLockonTarget == desiredTarget) return;
    if (weapons.jetLockonTarget)
        removeJetLockonTargeter(
            registry, lifecycle, weapons.jetLockonTarget, targeter);
    weapons.jetLockonTarget = INVALID_OBJECT_ID;
    if (!desiredTarget) return;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(desiredTarget);
    if (!entity) return;
    JetLockonOwner owner = jetLockonOwner(registry, *entity);
    if (!owner.runtime || !owner.rule) return;
    auto& targeters = owner.runtime->lockonTargeters;
    const auto position = std::lower_bound(
        targeters.begin(), targeters.end(), targeter);
    if (position == targeters.end() || *position != targeter) {
        const bool first = targeters.empty();
        targeters.insert(position, targeter);
        if (first) {
            const uint64_t duration = jetLockonDurationTicks(
                owner.rule->lockonMilliseconds, framesPerSecond);
            owner.runtime->lockonReadyTick =
                confirmedTick > std::numeric_limits<uint64_t>::max() - duration
                    ? std::numeric_limits<uint64_t>::max()
                    : confirmedTick + duration;
        }
    }
    weapons.jetLockonTarget = desiredTarget;
}

[[nodiscard]] bool jetAimTemporarilyPrevented(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId target, ObjectId targeter, uint64_t confirmedTick) noexcept {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(target);
    if (!entity) return false;
    JetLockonOwner owner = jetLockonOwner(registry, *entity);
    if (!owner.runtime || owner.runtime->lockonReadyTick == 0 ||
        confirmedTick >= owner.runtime->lockonReadyTick)
        return false;
    return std::binary_search(
        owner.runtime->lockonTargeters.begin(),
        owner.runtime->lockonTargeters.end(), targeter);
}

[[nodiscard]] uint64_t weaponPresentationFingerprint(
    const ObjectWeaponComponent& weapons) noexcept {
    uint64_t value = 14695981039346656037ull;
    const auto mix = [&value](uint64_t part) noexcept {
        value ^= part;
        value *= 1099511628211ull;
    };
    mix(weapons.target.value);
    mix(weapons.jetLockonTarget.value);
    mix(weapons.activeOrderTick);
    mix(weapons.activeOrderSequence);
    mix(weapons.activeSourceScriptId);
    mix(weapons.nextShotSequence);
    mix(weapons.weaponSetGeneration);
    mix(weapons.activeWeaponSetIndex.value_or(UINT32_MAX));
    mix(weapons.currentSlot
            ? static_cast<uint8_t>(*weapons.currentSlot) : UINT8_MAX);
    mix(weapons.lockedSlot
            ? static_cast<uint8_t>(*weapons.lockedSlot) : UINT8_MAX);
    mix(static_cast<uint8_t>(weapons.lockType));
    mix(static_cast<uint8_t>(weapons.state));
    mix(weapons.consecutiveShots);
    mix(weapons.consecutiveShotVictim.value);
    mix(weapons.continuousFireCooldownTick);
    mix(weapons.forceReloadTick);
    mix(weapons.loopingFireSoundStopTick);
    mix(weapons.loopingFireSoundWeapon.value);
    mix(static_cast<uint8_t>(weapons.continuousFireStage));
    for (const ObjectWeaponSetRuntime& set : weapons.sets) {
        mix(set.conditions);
        mix(set.shareWeaponReloadTime);
        mix(set.weaponLockSharedAcrossSets);
        mix(set.sharedReloadCompleteTick);
        for (const ObjectWeaponSlotRuntime& slot : set.slots) {
            mix(slot.content.value);
            mix(slot.ammoInClip);
            mix(slot.nextReadyTick);
            mix(slot.reloadCompleteTick);
            mix(slot.preAttackCompleteTick);
            mix(slot.lastFireTick);
            mix(slot.suspendFxUntilTick);
            mix(slot.lastFireSequence);
            mix(slot.currentBarrel);
            mix(slot.shotsRemainingForCurrentBarrel);
            mix(slot.previousFireTick);
            mix(slot.previousFireSequence);
            mix(slot.preAttackOrderTick);
            mix(slot.preAttackOrderSequence);
            mix(slot.preAttackSourceScriptId);
            mix(slot.preAttackTarget.value);
            mix(slot.clipGeneration);
            mix(slot.preAttackClipGeneration);
            mix(slot.preAttackArmed);
            mix(slot.reloadReplenishesClip);
        }
    }
    for (const ObjectTurretRuntime& turret : weapons.turrets) {
        mix(static_cast<uint64_t>(turret.yawRadians.raw()));
        mix(static_cast<uint64_t>(turret.pitchRadians.raw()));
        mix(static_cast<uint64_t>(turret.idleScanTargetYawRadians.raw()));
        mix(turret.recenterAtTick);
        mix(turret.nextIdleScanTick);
        mix(turret.sweepEnabledUntilTick);
        mix(turret.continuousFireSoundUntilTick);
        mix(static_cast<uint8_t>(turret.idlePhase));
        mix(turret.positiveSweep);
        mix(turret.enabled);
        mix(turret.forcedRecentering);
        // TURRET_ROTATE is selected from this bit alone. The final snap can
        // leave the angle unchanged, so yawRadians is not a sufficient proxy
        // for the rotating -> aligned transition.
        mix(turret.rotating);
    }
    return value;
}

[[nodiscard]] bool weaponPresentationDeadlineDue(
    const ObjectWeaponComponent& weapons, uint64_t confirmedTick) noexcept {
    for (const ObjectWeaponSetRuntime& set : weapons.sets) {
        if (set.sharedReloadCompleteTick == confirmedTick) return true;
        for (const ObjectWeaponSlotRuntime& slot : set.slots) {
            const bool firingPulseExpires = slot.lastFireSequence != 0 &&
                slot.lastFireTick != std::numeric_limits<uint64_t>::max() &&
                slot.lastFireTick + 1u == confirmedTick;
            if (firingPulseExpires ||
                slot.nextReadyTick == confirmedTick ||
                slot.reloadCompleteTick == confirmedTick ||
                slot.preAttackCompleteTick == confirmedTick ||
                slot.suspendFxUntilTick == confirmedTick) {
                return true;
            }
        }
    }
    return false;
}

class WeaponPresentationDirtyGuard final {
public:
    WeaponPresentationDirtyGuard(
        ecs::registry& registry, ecs::entity entity,
        ObjectWeaponComponent& weapons, uint64_t confirmedTick) noexcept
        : m_registry(registry), m_entity(entity), m_weapons(weapons),
          m_before(weaponPresentationFingerprint(weapons)),
          m_deadlineDue(weaponPresentationDeadlineDue(
              weapons, confirmedTick)) {}

    ~WeaponPresentationDirtyGuard() {
        if (!m_registry.valid(m_entity)) return;
        if (!m_deadlineDue &&
            m_before == weaponPresentationFingerprint(m_weapons)) {
            return;
        }
        markObjectDirty(
            m_registry, m_entity,
            objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
                objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
    }

private:
    ecs::registry& m_registry;
    ecs::entity m_entity = ecs::null;
    ObjectWeaponComponent& m_weapons;
    uint64_t m_before = 0;
    bool m_deadlineDue = false;
};

} // namespace

void gatherObjectSeeThroughObstacles(
    const ecs::registry& registry, container::Vector<uint64_t>& output) {
    output.clear();
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectKindOfComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectKindOfComponent& kinds =
            view.template get<const ObjectKindOfComponent>(entity);
        if (!identity.id ||
            !game::objectHasKind(
                kinds.mask, game::ObjectKindOf::CanSeeThroughStructure)) {
            continue;
        }
        output.push_back(identity.id.value);
    }
    // Sorted and deduplicated so the transparency count is independent of ECS
    // storage order.
    std::sort(output.begin(), output.end());
    output.erase(std::unique(output.begin(), output.end()), output.end());
}

bool objectAttackViewBlockedByObstacle(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const navigation::NavigationSystem* navigation,
    bool attackUsesLineOfSight,
    container::Span<const uint64_t> seeThroughObstacles,
    ecs::entity attackerEntity, ObjectId attacker,
    const LogicFixedVec3& attackerPosition,
    ObjectId victim, const LogicFixedVec3& victimPosition,
    bool contactWeapon) noexcept {
    // Global AIData switch, then the per-object authored requirement. Both are
    // early returns inside Pathfinder::isAttackViewBlockedByObstacle.
    if (!attackUsesLineOfSight || contactWeapon || navigation == nullptr ||
        !navigation->isInitialized() ||
        !navigation->topologyQueriesAvailable()) {
        return false;
    }
    const ObjectKindOfComponent* attackerKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, attackerEntity);
    if (!containsKind(attackerKinds,
                      game::ObjectKindOf::AttackNeedsLineOfSight)) {
        return false;
    }
    // srj sez: at tiny ranges the cell walk reports false positives, which is
    // why RefCode skips contact weapons above and only tests ground attackers
    // here. IMMOBILE and SPAWNS_ARE_THE_WEAPONS are its explicit exceptions
    // for turrets and Stinger soldiers, which have no locomotor.
    const ObjectAirborneComponent* attackerAirborne =
        ecs::try_get<ObjectAirborneComponent>(registry, attackerEntity);
    const ObjectLocomotionComponent* attackerLocomotion =
        ecs::try_get<ObjectLocomotionComponent>(registry, attackerEntity);
    const ObjectContainedByComponent* attackerContained =
        ecs::try_get<ObjectContainedByComponent>(registry, attackerEntity);
    bool attackerOnGround =
        (attackerLocomotion &&
         (!attackerAirborne || !attackerAirborne->isAirborne)) ||
        containsKind(attackerKinds, game::ObjectKindOf::Immobile) ||
        containsKind(attackerKinds, game::ObjectKindOf::SpawnsAreTheWeapons);
    // getSlaverID(): a spawned slave never has its view blocked by the object
    // it is slaved to, and vice versa.
    const auto slaverOf = [&registry](ecs::entity entity) noexcept {
        const ObjectSpawnedByRuntimeComponent* spawned =
            ecs::try_get<ObjectSpawnedByRuntimeComponent>(registry, entity);
        return spawned ? spawned->master : INVALID_OBJECT_ID;
    };
    const ObjectId attackerSlaver = slaverOf(attackerEntity);
    ObjectId attackerContainer = INVALID_OBJECT_ID;
    if (attackerContained && attackerContained->container) {
        attackerContainer = attackerContained->container;
        const std::optional<ecs::entity> host =
            lifecycle.entityFromId(attackerContainer);
        if (!attackerOnGround && host) {
            // Contained objects on the ground -- garrisoned buildings.
            const ObjectAirborneComponent* hostAirborne =
                ecs::try_get<ObjectAirborneComponent>(registry, *host);
            const ObjectKindOfComponent* hostKinds =
                ecs::try_get<ObjectKindOfComponent>(registry, *host);
            attackerOnGround =
                containsKind(hostKinds, game::ObjectKindOf::Structure) ||
                !hostAirborne || !hostAirborne->isAirborne;
        }
    }
    if (!attackerOnGround) return false;

    ObjectId victimContainer = INVALID_OBJECT_ID;
    ObjectId victimSlaver = INVALID_OBJECT_ID;
    if (victim) {
        const std::optional<ecs::entity> victimEntity =
            lifecycle.entityFromId(victim);
        if (!victimEntity) return false;
        // RefCode never checks line of sight to flying objects.
        const ObjectAirborneComponent* victimAirborne =
            ecs::try_get<ObjectAirborneComponent>(registry, *victimEntity);
        if (victimAirborne && victimAirborne->isAirborne) return false;
        if (const ObjectContainedByComponent* victimContained =
                ecs::try_get<ObjectContainedByComponent>(
                    registry, *victimEntity)) {
            victimContainer = victimContained->container;
        }
        victimSlaver = slaverOf(*victimEntity);
    }

    return navigation::attackViewBlockedByObstacle(
        navigation->grid(), navigation->dynamicOverlay(),
        {
            .attacker = {attackerPosition.x.raw(), attackerPosition.y.raw(),
                         attackerPosition.z.raw()},
            .victim = {victimPosition.x.raw(), victimPosition.y.raw(),
                       victimPosition.z.raw()},
            .attackerEntityId = attacker.value,
            .victimEntityId = victim.value,
            .attackerContainerEntityId = attackerContainer.value,
            .victimContainerEntityId = victimContainer.value,
            .attackerSlaverEntityId = attackerSlaver.value,
            .victimSlaverEntityId = victimSlaver.value,
            .seeThroughEntityIds = seeThroughObstacles,
        });
}

bool ObjectCombatSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content,
    const ObjectSpatialIndex* spatialIndex, const PlayerRegistry* players,
    SimulationRandom& random, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick, ObjectCombatWeaponFireSink weaponFireSink,
    ObjectCombatAIInput aiInput) {
    // Local sibling order only. GameSession replaces it with the common
    // owner-thread admission ordinal at the Combat handoff boundary.
    uint64_t nextFrameFireLocalOrdinal = 1;
    const auto reserveFrameFireEmissionSequence = [&]() {
        const uint64_t result = nextFrameFireLocalOrdinal++;
        if (nextFrameFireLocalOrdinal == 0)
            ++nextFrameFireLocalOrdinal;
        return result;
    };
    container::Vector<ObjectSystemWeaponFireCommand> actorWeaponFires;
    bool sinkFailed = false;
    struct ActorWeaponClosure final {
        container::Vector<ObjectSystemWeaponFireCommand>& commands;
        ObjectCombatWeaponFireSink sink;
        bool& failed;

        ~ActorWeaponClosure() {
            if (failed) {
                commands.clear();
                return;
            }
            for (ObjectSystemWeaponFireCommand& command : commands) {
                if (!sink(std::move(command))) {
                    failed = true;
                    break;
                }
            }
            commands.clear();
        }
    };
    struct Candidate final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };

    // PointDefenseLaserUpdate owns private weapons and scans independently
    // from Attack intents. Run these deterministic ObjectId/module-order
    // lanes before ordinary WeaponSet actors, matching the legacy Update
    // boundary while reusing the same damage/projectile/event sinks.
    container::Vector<Candidate> pointDefenseActors;
    const auto pointDefenseView = ecs::view<
        ObjectIdentityComponent, ObjectPointDefenseLaserComponent,
        TransformComponent, OwnerComponent>(registry);
    for (const ecs::entity entity : pointDefenseView) {
        const ObjectIdentityComponent& identity =
            pointDefenseView.template get<ObjectIdentityComponent>(entity);
        if (identity.id && lifecycle.entityFromId(identity.id)) {
            pointDefenseActors.push_back({identity.id, entity});
        }
    }
    std::sort(pointDefenseActors.begin(), pointDefenseActors.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.id < right.id;
        });
    const auto matchesAnyKind = [](const ObjectKindOfComponent* kinds,
                                   const game::ObjectKindOfMask& mask) {
        return kinds && kinds->mask.test_for_any(mask);
    };
    container::Vector<ObjectId> pointDefenseNearby;
    for (const Candidate& actor : pointDefenseActors) {
        if (sinkFailed) return false;
        ActorWeaponClosure actorClosure{
            actorWeaponFires, weaponFireSink, sinkFailed};
        const ObjectHealthComponent* sourceHealth =
            ecs::try_get<ObjectHealthComponent>(registry, actor.entity);
        if (sourceHealth && sourceHealth->effectivelyDead) {
            continue;
        }
        ObjectPointDefenseLaserComponent& component =
            ecs::get<ObjectPointDefenseLaserComponent>(registry, actor.entity);
        if (isObjectDisabled(registry, actor.entity, confirmedTick)) {
            // The legacy scheduler does not call this UpdateModule while any
            // Disabled reason is active. Its private countdowns therefore do
            // not elapse. Keep the modern absolute deadlines one tick ahead
            // for every skipped confirmed frame to preserve that pause.
            for (ObjectPointDefenseLaserRuleRuntime& rule : component.rules) {
                if (rule.nextScanTick != 0) {
                    rule.nextScanTick = saturatingTickAdd(
                        rule.nextScanTick, 1u);
                }
                if (rule.nextShotTick != 0) {
                    rule.nextShotTick = saturatingTickAdd(
                        rule.nextShotTick, 1u);
                }
            }
            continue;
        }
        const TransformComponent& sourceTransform =
            ecs::get<TransformComponent>(registry, actor.entity);
        const LogicFixedVec3 sourceFixedPosition =
            readAuthoritativeObjectPosition(
                registry, actor.entity, sourceTransform);
        const OwnerComponent& sourceOwner =
            ecs::get<OwnerComponent>(registry, actor.entity);
        for (ObjectPointDefenseLaserRuleRuntime& rule : component.rules) {
            const game::WeaponTemplate* weapon = content.findWeapon(rule.weapon);
            if (!weapon || !spatialIndex || !players) continue;
            const game::WeaponBonus privateTimerBonus;
            const math::q32_32 fireRangeFixed = math::q32_32::max(
                kFixedZero,
                privateTimerBonus.scale(
                    weapon->fixed.attackRange,
                    game::WeaponBonusField::Range) -
                    kFixedRationalizedRangeUndersize);

            const auto scan = [&]() -> ObjectId {
                struct Selection final {
                    ObjectId inRange = INVALID_OBJECT_ID;
                    ObjectId outside = INVALID_OBJECT_ID;
                    math::q32_32 inDistanceSquared{};
                    math::q32_32 outsideDistanceSquared{};
                } selected[2];
                spatialIndex->queryRadiusFixed(
                    sourceFixedPosition, rule.scanRange,
                    pointDefenseNearby);
                const math::q32_32 scanRangeSquared =
                    rule.scanRange * rule.scanRange;
                const math::q32_32 fireRangeSquared =
                    fireRangeFixed * fireRangeFixed;
                const math::q32_32 predictionScale =
                    logicFramesPerSecond == 0
                        ? math::q32_32{}
                        : rule.predictTargetVelocityFactor /
                              math::q32_32{static_cast<int32_t>(
                                  logicFramesPerSecond)};
                for (const ObjectId candidate : pointDefenseNearby) {
                    if (!candidate || candidate == actor.id) continue;
                    const std::optional<ecs::entity> target =
                        lifecycle.entityFromId(candidate);
                    if (!target) continue;
                    const ObjectHealthComponent* health =
                        ecs::try_get<ObjectHealthComponent>(registry, *target);
                    if (health && health->effectivelyDead) continue;
                    const ObjectKindOfComponent* kinds =
                        ecs::try_get<ObjectKindOfComponent>(registry, *target);
                    const size_t lane = matchesAnyKind(
                        kinds, rule.primaryTargetKindMask) ? 0u :
                        matchesAnyKind(kinds, rule.secondaryTargetKindMask) ? 1u : 2u;
                    if (lane > 1u) continue;
                    const OwnerComponent* owner =
                        ecs::try_get<OwnerComponent>(registry, *target);
                    if (!owner || relationshipBetweenObjects(
                            registry, *players, actor.entity, *target) !=
                            PlayerRelationship::Enemies) continue;
                    const ObjectStatusComponent* status =
                        ecs::try_get<ObjectStatusComponent>(registry, *target);
                    if (status &&
                        status->hasAny(game::objectStatusBit(
                            game::ObjectStatusFlag::Stealthed)) &&
                        !status->hasAny(game::objectStatusBit(
                            game::ObjectStatusFlag::Detected)) &&
                        !status->hasAny(game::objectStatusBit(
                            game::ObjectStatusFlag::Disguised))) continue;
                    if (!targetMatchesAntiMask(
                            *weapon, kinds,
                            ecs::try_get<ObjectAirborneComponent>(
                                registry, *target),
                            status)) continue;
                    const TransformComponent* transform =
                        ecs::try_get<TransformComponent>(registry, *target);
                    if (!transform) continue;
                    const LogicFixedVec3 targetPosition =
                        readAuthoritativeObjectPosition(
                            registry, *target, *transform);
                    const math::q32_32 dx =
                        targetPosition.x - sourceFixedPosition.x;
                    const math::q32_32 dy =
                        targetPosition.y - sourceFixedPosition.y;
                    const math::q32_32 distanceSquared =
                        dx * dx + dy * dy;
                    // ObjectSpatialIndex includes the candidate footprint in
                    // its broad-phase query. RefCode's FROM_CENTER_2D scan
                    // still applies an exact center-distance radius here.
                    if (distanceSquared > scanRangeSquared) continue;
                    Selection& choice = selected[lane];
                    if (distanceSquared <= fireRangeSquared) {
                        if (!choice.inRange ||
                            distanceSquared < choice.inDistanceSquared ||
                            (distanceSquared == choice.inDistanceSquared &&
                             candidate < choice.inRange)) {
                            choice.inDistanceSquared = distanceSquared;
                            choice.inRange = candidate;
                        }
                    } else if (!choice.inRange) {
                        math::q32_32 predictedX = targetPosition.x;
                        math::q32_32 predictedY = targetPosition.y;
                        if (predictionScale != math::q32_32{} &&
                            !containsKind(kinds, game::ObjectKindOf::Immobile)) {
                            if (const ObjectPhysicsComponent* physics =
                                    ecs::try_get<ObjectPhysicsComponent>(
                                        registry, *target)) {
                                predictedX +=
                                    physics->velocityUnitsPerSecond.x *
                                    predictionScale;
                                predictedY +=
                                    physics->velocityUnitsPerSecond.y *
                                    predictionScale;
                            }
                        }
                        const math::q32_32 predictedDx =
                            predictedX - sourceFixedPosition.x;
                        const math::q32_32 predictedDy =
                            predictedY - sourceFixedPosition.y;
                        const math::q32_32 predictedDistanceSquared =
                            predictedDx * predictedDx +
                            predictedDy * predictedDy;
                        if (!choice.outside ||
                            predictedDistanceSquared <
                                choice.outsideDistanceSquared ||
                            (predictedDistanceSquared ==
                                 choice.outsideDistanceSquared &&
                             candidate < choice.outside)) {
                            choice.outsideDistanceSquared =
                                predictedDistanceSquared;
                            choice.outside = candidate;
                        }
                    }
                }
                if (selected[0].inRange) return selected[0].inRange;
                if (selected[1].inRange) return selected[1].inRange;
                if (selected[0].outside) return selected[0].outside;
                return selected[1].outside;
            };

            const uint64_t scanFrames = millisecondsToFrames(
                rule.scanRateMilliseconds, logicFramesPerSecond);
            if (rule.nextScanTick == 0 || confirmedTick >= rule.nextScanTick) {
                rule.trackedTarget = scan();
                rule.nextScanTick = saturatingTickAdd(
                    confirmedTick, scanFrames + 1u);
                rule.targetWasInRange = false;
            }
            std::optional<ecs::entity> targetEntity =
                lifecycle.entityFromId(rule.trackedTarget);
            const ObjectHealthComponent* trackedHealth = targetEntity
                ? ecs::try_get<ObjectHealthComponent>(registry, *targetEntity)
                : nullptr;
            if (!targetEntity ||
                (trackedHealth && trackedHealth->effectivelyDead)) {
                const bool deadTargetStillExists = targetEntity.has_value();
                rule.trackedTarget = INVALID_OBJECT_ID;
                rule.targetWasInRange = false;
                if (!deadTargetStillExists) continue;
                const uint64_t delay = static_cast<uint64_t>(
                    random.integerInclusive(0, 3));
                rule.nextScanTick = saturatingTickAdd(confirmedTick, delay);
                if (delay != 0) continue;
                rule.trackedTarget = scan();
                rule.nextScanTick = saturatingTickAdd(
                    confirmedTick, scanFrames + 1u);
                targetEntity = lifecycle.entityFromId(rule.trackedTarget);
                if (!targetEntity) continue;
            }
            const TransformComponent* targetTransform = targetEntity
                ? ecs::try_get<TransformComponent>(registry, *targetEntity)
                : nullptr;
            if (!targetTransform) continue;
            const LogicFixedVec3 targetFixedPosition =
                readAuthoritativeObjectPosition(
                    registry, *targetEntity, *targetTransform);
            const math::q32_32 targetDx =
                targetFixedPosition.x - sourceFixedPosition.x;
            const math::q32_32 targetDy =
                targetFixedPosition.y - sourceFixedPosition.y;
            const bool inRange =
                targetDx * targetDx + targetDy * targetDy <
                    fireRangeFixed * fireRangeFixed;
            if (!inRange && rule.targetWasInRange) {
                const uint64_t delay = static_cast<uint64_t>(
                    random.integerInclusive(0, 3));
                rule.trackedTarget = INVALID_OBJECT_ID;
                rule.targetWasInRange = false;
                rule.nextScanTick = saturatingTickAdd(confirmedTick, delay);
                if (delay == 0) {
                    rule.trackedTarget = scan();
                    rule.nextScanTick = saturatingTickAdd(
                        confirmedTick, scanFrames + 1u);
                }
                continue;
            }
            rule.targetWasInRange = inRange;
            if (!inRange || confirmedTick < rule.nextShotTick) continue;

            uint32_t shotSequence = rule.nextShotSequence++;
            if (shotSequence == 0) {
                shotSequence = rule.nextShotSequence++;
            }
            const size_t priorCommandCount = actorWeaponFires.size();
            if (!queueObjectTargetedTransientWeaponFire(
                    rule.weapon, registry, actor.entity, actor.id,
                    *targetEntity, rule.trackedTarget, content, random,
                    shotSequence, rule.authoredOrder,
                    nextFrameFireLocalOrdinal, confirmedTick,
                    actorWeaponFires)) {
                continue;
            }
            // RefCode allocates the behavior's private weapon explicitly in
            // TERTIARY_WEAPON, which selects LASER/LazerSpot draw bones even
            // though it is independent from the object's normal WeaponSet.
            if (actorWeaponFires.size() > priorCommandCount) {
                actorWeaponFires.back().launchSlot =
                    game::WeaponSlot::Tertiary;
                static_cast<void>(reserveFrameFireEmissionSequence());
            }
            // The temporary legacy Weapon also advanced its own (immediately
            // discarded) cooldown before PointDefenseLaserUpdate sampled the
            // behavior-owned timer below. Preserve that RNG draw for modded
            // min/max delays; retail point-defense weapons use a fixed delay.
            const game::WeaponBonus firedWeaponBonus =
                content.resolveWeaponBonus(
                    *weapon,
                    actorWeaponFires.back().bonusConditions);
            static_cast<void>(chooseShotDelayFrames(
                *weapon, logicFramesPerSecond, random, firedWeaponBonus));
            rule.nextShotTick = saturatingTickAdd(
                confirmedTick, saturatingTickAdd(
                    chooseShotDelayFrames(
                        *weapon, logicFramesPerSecond, random,
                        privateTimerBonus), 1u));
        }
    }

    // Detach removes reservations eagerly, but destruction can erase a
    // passenger before it reaches this combat loop. Validate every sparse
    // host-owned reservation against both sides of the containment edge so a
    // dead/moved occupant can never monopolize a FIREPOINT indefinitely.
    const auto garrisonPointView = ecs::view<
        ObjectIdentityComponent, ObjectGarrisonFirePointComponent,
        ObjectContainmentComponent, ObjectContainmentRuntimeComponent>(
            registry);
    for (const ecs::entity host : garrisonPointView) {
        const ObjectId hostId = garrisonPointView
            .template get<ObjectIdentityComponent>(host).id;
        ObjectGarrisonFirePointComponent& firePoints = garrisonPointView
            .template get<ObjectGarrisonFirePointComponent>(host);
        const ObjectContainmentComponent& contents = garrisonPointView
            .template get<ObjectContainmentComponent>(host);
        const ObjectContainmentRuntimeComponent& runtime = garrisonPointView
            .template get<ObjectContainmentRuntimeComponent>(host);
        const auto stale = std::remove_if(
            firePoints.assignments.begin(), firePoints.assignments.end(),
            [&](const ObjectGarrisonFirePointAssignment& assignment) {
                const auto roster = std::lower_bound(
                    contents.objects.begin(), contents.objects.end(),
                    assignment.occupant,
                    [](const ObjectContainedObjectRecord& record,
                       ObjectId id) {
                        return record.object < id;
                    });
                if (!hostId || roster == contents.objects.end() ||
                    roster->object != assignment.occupant)
                    return true;
                const std::optional<ecs::entity> passenger =
                    lifecycle.entityFromId(assignment.occupant);
                const ObjectContainedByComponent* edge = passenger
                    ? ecs::try_get<ObjectContainedByComponent>(
                        registry, *passenger)
                    : nullptr;
                return !edge || edge->container != hostId ||
                    !edge->enclosing || !runtime.plan ||
                    edge->containmentRuleIndex >= runtime.plan->rules.size() ||
                    runtime.plan->rules[edge->containmentRuleIndex].kind !=
                        ObjectContainmentKind::Garrison ||
                    !runtime.plan->rules[edge->containmentRuleIndex]
                         .enclosingContainer;
            });
        if (stale != firePoints.assignments.end()) {
            firePoints.assignments.erase(stale,
                                         firePoints.assignments.end());
            ++firePoints.revision;
        }
    }

    container::Vector<Candidate> actors;
    const auto view = ecs::view<ObjectIdentityComponent, ObjectWeaponComponent, TransformComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity = view.template get<ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id)) continue;
        actors.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(actors.begin(), actors.end(), [](const Candidate& left, const Candidate& right) {
        return left.id < right.id;
    });
    m_aiAttackFeedback.reserve(m_aiAttackFeedback.size() + actors.size());

    // One transparency roster per confirmed tick, shared by every actor's
    // line-of-sight walk below.
    container::Vector<uint64_t> seeThroughObstacles;
    const bool attackLineOfSightEnabled = aiInput.attackUsesLineOfSight &&
        aiInput.navigation != nullptr &&
        aiInput.navigation->isInitialized() &&
        aiInput.navigation->topologyQueriesAvailable();
    if (attackLineOfSightEnabled) {
        gatherObjectSeeThroughObstacles(registry, seeThroughObstacles);
    }

    for (const Candidate& actor : actors) {
        if (sinkFailed) return false;
        ActorWeaponClosure actorClosure{
            actorWeaponFires, weaponFireSink, sinkFailed};
        if (!lifecycle.entityFromId(actor.id)) continue;
        ObjectWeaponComponent& weapons = ecs::get<ObjectWeaponComponent>(registry, actor.entity);
        WeaponPresentationDirtyGuard dirtyGuard{
            registry, actor.entity, weapons, confirmedTick};
        updateObjectFiringTracker(
            registry, actor.entity, actor.id, weapons, content, random,
            logicFramesPerSecond, confirmedTick, m_events);
        const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(registry, actor.entity);
        const auto releaseContainedGarrisonFirePoint = [&]() {
            if (!contained || !contained->enclosing || !contained->container)
                return;
            const std::optional<ecs::entity> host =
                lifecycle.entityFromId(contained->container);
            if (!host) return;
            const ObjectContainmentRuntimeComponent* hostRuntime =
                ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *host);
            if (!hostRuntime || !hostRuntime->plan ||
                contained->containmentRuleIndex >= hostRuntime->plan->rules.size() ||
                hostRuntime->plan->rules[contained->containmentRuleIndex].kind !=
                    ObjectContainmentKind::Garrison ||
                !hostRuntime->plan->rules[contained->containmentRuleIndex].enclosingContainer)
                return;
            releaseGarrisonFirePoint(registry, *host, actor.id);
        };
        ObjectOrderQueueComponent* directQueue =
            !contained || !contained->enclosing
                ? ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                          actor.entity)
                : nullptr;
        const ObjectOrderIntent* directOrder =
            directQueue && !directQueue->orders.empty()
                ? &directQueue->orders.front()
                : nullptr;
        ObjectAICombatOperationComponent* operation =
            ecs::try_get<ObjectAICombatOperationComponent>(registry,
                                                            actor.entity);
        const bool aiOwnsDirectAttack =
            isOrdinaryAIAttackOrder(directOrder) && actor.id &&
            std::binary_search(aiInput.owners.begin(), aiInput.owners.end(),
                               actor.id);
        const bool aiOwnsAttackMoveChild =
            isAIAttackMoveOrder(directOrder) && actor.id &&
            std::binary_search(aiInput.owners.begin(), aiInput.owners.end(),
                               actor.id);
        const bool aiOwnsTacticalChild =
            isAITacticalAttackOrder(directOrder) && actor.id &&
            std::binary_search(aiInput.owners.begin(), aiInput.owners.end(),
                               actor.id);
        const auto tacticalChildStateAllowed =
            [directOrder](ai::AIStateId state) noexcept {
                if (!directOrder ||
                    directOrder->kind != ObjectOrderKind::TacticalAttack) {
                    return false;
                }
                switch (directOrder->tacticalAttackSubtype) {
                case ObjectTacticalAttackSubtype::Hunt:
                case ObjectTacticalAttackSubtype::AttackSquad:
                case ObjectTacticalAttackSubtype::AttackArea:
                case ObjectTacticalAttackSubtype::GuardRetaliate:
                    return state == ai::AIStateId::AttackObject;
                case ObjectTacticalAttackSubtype::Guard:
                case ObjectTacticalAttackSubtype::GuardTunnelNetwork:
                    return state == ai::AIStateId::AttackObject ||
                        state == ai::AIStateId::AttackAndFollowObject;
                default:
                    return false;
                }
            };
        const bool autonomousAttackOwner = actor.id &&
            std::binary_search(
                aiInput.autonomousOwners.begin(),
                aiInput.autonomousOwners.end(), actor.id);
        const bool autonomousOperation = !directOrder && operation &&
            operation->correlation.state == ai::AIStateId::AttackObject &&
            !operation->correlation.orderIdentity.isValid();
        const bool autonomousCommand = !directOrder && std::any_of(
            aiInput.commands.begin(), aiInput.commands.end(),
            [actor](const ai::AIAttackCommand& command) noexcept {
                return command.correlation.subject == actor.id &&
                    command.correlation.state ==
                        ai::AIStateId::AttackObject &&
                    !command.correlation.orderIdentity.isValid() &&
                    command.correlation.isValid();
            });
        const bool aiOwnsAutonomousAttack = autonomousAttackOwner &&
            (autonomousOperation || autonomousCommand);
        const bool aiOwnsCombatOrder =
            aiOwnsDirectAttack || aiOwnsAttackMoveChild ||
            aiOwnsTacticalChild || aiOwnsAutonomousAttack;
        const ai::AIAsyncOrderIdentity currentOrderIdentity =
            !aiOwnsAutonomousAttack && aiOwnsCombatOrder
                ? attackOrderIdentity(actor.id, *directQueue, *directOrder)
                : ai::AIAsyncOrderIdentity{};
        if (operation &&
            (!aiOwnsCombatOrder ||
             !operation->correlation.isValid() ||
             operation->correlation.subject != actor.id ||
             operation->correlation.orderIdentity != currentOrderIdentity ||
             (aiOwnsTacticalChild &&
              !tacticalChildStateAllowed(operation->correlation.state)) ||
             operation->correlation.phase != operation->phase ||
             !isActiveAICombatPhase(operation->phase))) {
            clearAICombatOperation(registry, actor.entity, weapons);
            operation = nullptr;
        }

        if (aiOwnsCombatOrder) {
            for (const ai::AIAttackCommand& command : aiInput.commands) {
                if (command.correlation.subject != actor.id ||
                    command.correlation.orderIdentity != currentOrderIdentity ||
                    !command.correlation.isValid() ||
                    !commandPhaseMatchesKind(command) ||
                    command.confirmedTick > confirmedTick) {
                    continue;
                }

                const bool directCommand = aiOwnsDirectAttack &&
                    command.correlation.state != ai::AIStateId::AttackMoveTo;
                const bool attackMoveChildCommand = aiOwnsAttackMoveChild &&
                    command.correlation.state == ai::AIStateId::AttackObject;
                const bool tacticalChildCommand = aiOwnsTacticalChild &&
                    tacticalChildStateAllowed(command.correlation.state);
                const bool autonomousAttackCommand =
                    aiOwnsAutonomousAttack && !directOrder &&
                    command.correlation.state ==
                        ai::AIStateId::AttackObject &&
                    !command.correlation.orderIdentity.isValid();
                if (!directCommand && !attackMoveChildCommand &&
                    !tacticalChildCommand && !autonomousAttackCommand) {
                    continue;
                }
                if (directCommand) {
                    const bool attacksObject = directOrder->targetObject !=
                        INVALID_OBJECT_ID;
                    if (command.attacksObject != attacksObject ||
                        (attacksObject &&
                         (command.forceAttack != directOrder->forceAttack ||
                          command.target != directOrder->targetObject))) {
                        continue;
                    }
                } else if (!command.attacksObject || command.forceAttack ||
                           !command.target || command.target == actor.id) {
                    continue;
                }

                const auto beginOperation = [&](ai::AIAttackPhase phase) {
                    const bool tracksAttackMoveLimit =
                        aiOwnsAttackMoveChild && directOrder->maximumShots;
                    ObjectAICombatOperationComponent* active =
                        ecs::try_get<ObjectAICombatOperationComponent>(
                            registry, actor.entity);
                    if (active && active->correlation == command.correlation) {
                        active->phase = phase;
                        active->target = command.target;
                        active->targetPosition = command.targetPosition;
                        active->attacksObject = command.attacksObject;
                        active->forceAttack = command.forceAttack;
                        return;
                    }
                    if (active) {
                        clearAICombatOperation(registry, actor.entity,
                                               weapons);
                    }
                    ecs::emplace<ObjectAICombatOperationComponent>(
                        registry, actor.entity,
                        ObjectAICombatOperationComponent{
                            .correlation = command.correlation,
                            .target = command.target,
                            .targetPosition = command.targetPosition,
                            .phase = phase,
                            .attacksObject = command.attacksObject,
                            .forceAttack = command.forceAttack,
                            .fireRequested = false,
                            .maximumShots = tracksAttackMoveLimit
                                ? *directOrder->maximumShots : 0u,
                            .shotsFired = 0u,
                            .hasMaximumShots = tracksAttackMoveLimit,
                            .attackLimitReached = tracksAttackMoveLimit &&
                                *directOrder->maximumShots == 0u,
                        });
                };

                operation = ecs::try_get<ObjectAICombatOperationComponent>(
                    registry, actor.entity);
                switch (command.kind) {
                case ai::AIAttackCommandKind::BeginAim:
                    beginOperation(ai::AIAttackPhase::Aim);
                    break;
                case ai::AIAttackCommandKind::EndAim:
                    if (operation &&
                        operation->correlation == command.correlation) {
                        ecs::remove<ObjectAICombatOperationComponent>(
                            registry, actor.entity);
                    }
                    break;
                case ai::AIAttackCommandKind::BeginFire:
                    beginOperation(ai::AIAttackPhase::Fire);
                    break;
                case ai::AIAttackCommandKind::Fire:
                    if (operation &&
                        operation->correlation == command.correlation &&
                        operation->phase == ai::AIAttackPhase::Fire) {
                        operation->fireRequested = true;
                    }
                    break;
                case ai::AIAttackCommandKind::EndFire:
                    if (operation &&
                        operation->correlation == command.correlation) {
                        clearAICombatOperation(registry, actor.entity,
                                               weapons);
                    }
                    break;
                }
            }
        }
        operation = ecs::try_get<ObjectAICombatOperationComponent>(
            registry, actor.entity);

        ai::AIAttackFeedback aiSnapshot;
        const bool hasActiveAIOperation = aiOwnsCombatOrder && operation &&
            isActiveAICombatPhase(operation->phase);
        const ObjectId desiredJetLockonTarget =
            hasActiveAIOperation &&
                    operation->phase == ai::AIAttackPhase::Aim &&
                    operation->attacksObject
                ? operation->target
                : INVALID_OBJECT_ID;
        reconcileJetLockonTargeter(
            registry, lifecycle, actor.id, weapons,
            desiredJetLockonTarget, confirmedTick,
            logicFramesPerSecond);
        if (hasActiveAIOperation) {
            aiSnapshot.correlation = operation->correlation;
            aiSnapshot.kind = ai::AIAttackFeedbackKind::Snapshot;
            aiSnapshot.target = operation->target;
            aiSnapshot.targetPosition = operation->targetPosition;
            aiSnapshot.confirmedTick = confirmedTick;
            aiSnapshot.targetValid = !operation->attacksObject;
            const ai::AIAttackPolicy policy =
                ai::attackPolicyFor(operation->correlation.state);
            aiSnapshot.chaseAllowed = policy.attacksObject;
            aiSnapshot.shotLimitReached = operation->attackLimitReached;
        }
        ScopedAIAttackSnapshot snapshotScope(
            m_aiAttackFeedback, aiSnapshot, hasActiveAIOperation);

        const ObjectHealthComponent* actorHealth =
            ecs::try_get<ObjectHealthComponent>(registry, actor.entity);
        const ObjectStatusComponent* actorStatus =
            ecs::try_get<ObjectStatusComponent>(registry, actor.entity);
        const bool held = isObjectDisabledBy(
            registry, actor.entity, ObjectDisabledReason::Held,
            confirmedTick);
        const bool heldPassengerCanFire = held && contained &&
            objectPassengerAllowedToFire(
                registry, lifecycle, actor.entity, confirmedTick);
        const bool attackExecutionSuspended =
            (actorHealth && actorHealth->effectivelyDead) ||
            (actorStatus && actorStatus->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::Sold))) ||
            (held && !heldPassengerCanFire);

        // RefCode AIStates.cpp owns four attack statuses. They are written from
        // the attack state machine only, so this pass -- the one place where
        // this project executes an AI-owned attack order -- is the equivalent
        // single writer. All four are recomputed and applied together each tick;
        // ObjectStatusSystem::apply is a no-op when nothing changed, so this
        // cannot dirty an object spuriously. No random draw is involved, so
        // SimulationRandom consumption order is untouched.
        //
        //   IS_ATTACKING     AIAttackState::onEnter (:5579) once the attack
        //                    machine reports CONTINUE, cleared in its onExit
        //                    (:5717). It therefore spans approach and chase,
        //                    not just the aim/fire children -- which is exactly
        //                    what the jet attack-run reader needs.
        //   IS_AIMING_WEAPON AIAttackAimAtTargetState::onEnter (:5009) /
        //                    onExit (:5160).
        //   IS_FIRING_WEAPON AIAttackFireWeaponState::onEnter (:5195) /
        //                    onExit (:5337).
        //   IGNORING_STEALTH AIAttackState::onEnter (:5565-5571) only when the
        //                    selected weapon has a ContinueAttackRange, cleared
        //                    immediately after the shot (:5271, :5321), on the
        //                    fire state's exit (:5337) and on outer exit.
        {
            const bool outerAttackActive =
                aiOwnsCombatOrder && !attackExecutionSuspended;
            const bool aimingWeapon = outerAttackActive &&
                hasActiveAIOperation &&
                operation->phase == ai::AIAttackPhase::Aim;
            const bool firingWeapon = outerAttackActive &&
                hasActiveAIOperation &&
                operation->phase == ai::AIAttackPhase::Fire;
            // "Has not fired yet during this attack order" reproduces RefCode's
            // set-on-enter / clear-after-firing window without a new latch:
            // activeOrderTick is the issuing tick of the order currently being
            // executed, so any shot at or after it belongs to this attack.
            bool continueRangeBeforeFirstShot = false;
            if (outerAttackActive && weapons.activeWeaponSetIndex &&
                *weapons.activeWeaponSetIndex < weapons.sets.size()) {
                const ObjectWeaponSetRuntime& activeSet =
                    weapons.sets[*weapons.activeWeaponSetIndex];
                for (const ObjectWeaponSlotRuntime& slot : activeSet.slots) {
                    const game::WeaponTemplate* slotWeapon =
                        content.findWeapon(slot.content);
                    if (!slotWeapon ||
                        slotWeapon->fixed.continueAttackRange <=
                            math::q32_32{}) {
                        continue;
                    }
                    if (slot.lastFireTick == 0 ||
                        slot.lastFireTick < weapons.activeOrderTick) {
                        continueRangeBeforeFirstShot = true;
                        break;
                    }
                }
            }
            const game::ObjectStatusMask attacking =
                game::objectStatusBit(game::ObjectStatusFlag::IsAttacking);
            const game::ObjectStatusMask aiming =
                game::objectStatusBit(game::ObjectStatusFlag::IsAimingWeapon);
            const game::ObjectStatusMask firing =
                game::objectStatusBit(game::ObjectStatusFlag::IsFiringWeapon);
            const game::ObjectStatusMask ignoringStealth =
                game::objectStatusBit(game::ObjectStatusFlag::IgnoringStealth);
            const game::ObjectStatusMask desired =
                (outerAttackActive ? attacking : 0u) |
                (aimingWeapon ? aiming : 0u) |
                (firingWeapon ? firing : 0u) |
                (continueRangeBeforeFirstShot ? ignoringStealth : 0u);
            const game::ObjectStatusMask owned =
                attacking | aiming | firing | ignoringStealth;
            static_cast<void>(ObjectStatusSystem::apply(
                registry, actor.entity, {
                    .setMask = desired,
                    .clearMask = static_cast<game::ObjectStatusMask>(
                        owned & ~desired),
                    .confirmedTick = confirmedTick,
                }));
        }

        // ActiveBody's subdual transition sets DISABLED_SUBDUED for ordinary
        // objects. Preserve the attack intent for a later recovery, but do
        // not advance/firing-gate weapon runtime while that Body state owns
        // the unit. Projectile jamming has a separate controller boundary.
        if (attackExecutionSuspended) {
            aiSnapshot.aimTemporarilyPrevented = hasActiveAIOperation;
            weapons.target = INVALID_OBJECT_ID;
            static_cast<void>(releaseWeaponLock(
                weapons, ObjectWeaponLockType::Temporary));
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::Idle;
            continue;
        }
        if (weapons.activeWeaponSetIndex && *weapons.activeWeaponSetIndex < weapons.sets.size()) {
            // Inactive WeaponSets do not continue cooling/reloading in the
            // background. RefCode destroys their Weapon instances on a set
            // transition and advances only the currently materialized set.
            advanceWeaponSet(weapons.sets[*weapons.activeWeaponSetIndex], content, confirmedTick);
        }

        // BattlePlanUpdate may disable the Strategy Center turret and force
        // the legacy RECENTER state before its door is allowed to close.
        // This state must win over an existing Attack order; otherwise the
        // target path would keep trying to aim and the plan transition would
        // wait forever.
        if (objectTurretsAreForcedRecentering(weapons)) {
            advanceTurretsTowardNatural(
                weapons, logicFramesPerSecond, confirmedTick, random);
            weapons.target = INVALID_OBJECT_ID;
            static_cast<void>(releaseWeaponLock(
                weapons, ObjectWeaponLockType::Temporary));
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::Idle;
            continue;
        }

        // Exact AI-owned Attack/AttackMove heads are never handed to the
        // legacy auto-fire consumer. Until BeginAim/BeginFire arrives, the
        // queue remains untouched and the weapon/turret runtime stays idle.
        if (aiOwnsCombatOrder && !hasActiveAIOperation) {
            advanceTurretsTowardNatural(
                weapons, logicFramesPerSecond, confirmedTick, random);
            weapons.target = INVALID_OBJECT_ID;
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::Idle;
            continue;
        }

        std::optional<ObjectOrderIntent> nestedAttackIntent;
        if ((aiOwnsAttackMoveChild || aiOwnsTacticalChild ||
             aiOwnsAutonomousAttack) &&
            hasActiveAIOperation) {
            nestedAttackIntent = directOrder
                ? *directOrder : ObjectOrderIntent{};
            if (aiOwnsAutonomousAttack) {
                nestedAttackIntent->source = ObjectOrderSource::System;
                nestedAttackIntent->systemPurpose =
                    ObjectOrderSystemPurpose::Generic;
                nestedAttackIntent->issuedTick =
                    operation->correlation.stateRequest.issuedTick;
                nestedAttackIntent->sourceSequence =
                    operation->correlation.stateRequest.sequence;
            }
            nestedAttackIntent->kind = ObjectOrderKind::Attack;
            nestedAttackIntent->tacticalAttackSubtype =
                ObjectTacticalAttackSubtype::None;
            nestedAttackIntent->targetObject = operation->target;
            nestedAttackIntent->targetX = math::q32_32::from_raw(
                operation->targetPosition.xRaw);
            nestedAttackIntent->targetY = math::q32_32::from_raw(
                operation->targetPosition.yRaw);
            nestedAttackIntent->targetZ = math::q32_32::from_raw(
                operation->targetPosition.zRaw);
            nestedAttackIntent->hasTargetPosition =
                !operation->attacksObject;
            nestedAttackIntent->maximumShots = operation->hasMaximumShots
                ? std::optional<uint32_t>{operation->maximumShots}
                : std::nullopt;
            nestedAttackIntent->shotsFired = operation->shotsFired;
            nestedAttackIntent->forceAttack = operation->forceAttack;
            nestedAttackIntent->attackMove = false;
            nestedAttackIntent->allArmyHunt = false;
            nestedAttackIntent->useTeamCommonTarget = false;
            nestedAttackIntent->tacticalTargetTeam =
                INVALID_OBJECT_TEAM_ID;
            nestedAttackIntent->tacticalTargetAreaId =
                std::numeric_limits<uint32_t>::max();
            nestedAttackIntent->tacticalTargetRevision = 0;
        }

        const ObjectOrderIntent* attackIntent = nullptr;
        ObjectOrderQueueComponent* consumableQueue = nullptr;
        if (contained && contained->enclosing) {
            if (nestedAttackIntent && aiOwnsAutonomousAttack) {
                attackIntent = &*nestedAttackIntent;
            }
            // Ordinary off-map passengers do not own an independent combat
            // order while contained. PassengersFireUpgrade grants a
            // read-only projection of the direct host's current Attack
            // intent; it does not copy into or revise the passenger queue.
            const std::optional<ecs::entity> host =
                lifecycle.entityFromId(contained->container);
            const ObjectContainmentComponent* hostContainment = host
                ? ecs::try_get<ObjectContainmentComponent>(registry, *host)
                : nullptr;
            const ObjectContainmentRuntimeComponent* hostRuntime = host
                ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry,
                                                                   *host)
                : nullptr;
            const ObjectContainmentRule* firingRule = hostRuntime &&
                    hostRuntime->plan &&
                    contained->containmentRuleIndex <
                        hostRuntime->plan->rules.size()
                ? &hostRuntime->plan->rules[
                    contained->containmentRuleIndex]
                : nullptr;
            // RefCode's OverlordContain/HelixContain reject firing when
            // the special host itself is contained (the nested-containment
            // path is deliberately non-firing).  Keep that rule on the
            // selected edge rather than accidentally allowing a passenger
            // through the host's aggregate permission bit.
            const ObjectContainedByComponent* hostContained = host
                ? ecs::try_get<ObjectContainedByComponent>(registry, *host)
                : nullptr;
            const bool nestedSpecialHost = hostContained &&
                hostContained->enclosing && firingRule &&
                (firingRule->kind == ObjectContainmentKind::Overlord ||
                 firingRule->kind == ObjectContainmentKind::Helix);
            const bool subduedGarrison = host && hostRuntime &&
                firingRule && firingRule->kind ==
                    ObjectContainmentKind::Garrison &&
                (objectDisabledMask(registry, *host, confirmedTick) &
                 objectDisabledBit(ObjectDisabledReason::Subdued)) != 0;
            const ObjectOrderQueueComponent* hostQueue =
                host && hostContainment && !subduedGarrison &&
                    hostContainment->passengersAllowedToFire
                ? ecs::try_get<ObjectOrderQueueComponent>(registry, *host)
                : nullptr;
            if (!attackIntent && hostQueue && !hostQueue->orders.empty() &&
                hostQueue->orders.front().kind == ObjectOrderKind::Attack &&
                (hostQueue->orders.front().source == ObjectOrderSource::Player ||
                 hostQueue->orders.front().source == ObjectOrderSource::Script)) {
                const ObjectKindOfComponent* passengerKinds =
                    ecs::try_get<ObjectKindOfComponent>(registry, actor.entity);
                const bool portableStructure = passengerKinds &&
                    game::objectHasKind(
                        passengerKinds->mask,
                        game::ObjectKindOf::PortableStructure);
                const bool infantry = passengerKinds &&
                    game::objectHasKind(passengerKinds->mask,
                                        game::ObjectKindOf::Infantry);
                bool allowedPassengerKind = false;
                if (firingRule &&
                    firingRule->kind == ObjectContainmentKind::Transport) {
                    allowedPassengerKind = infantry;
                } else if (firingRule &&
                           (firingRule->kind ==
                                ObjectContainmentKind::Overlord ||
                            firingRule->kind ==
                                 ObjectContainmentKind::Helix)) {
                    allowedPassengerKind = infantry || portableStructure;
                } else if (firingRule &&
                           (firingRule->kind == ObjectContainmentKind::Open ||
                            firingRule->kind == ObjectContainmentKind::Garrison)) {
                    // OpenContain and GarrisonContain do not impose an
                    // additional KindOf restriction in RefCode; their
                    // module-level permission is the gate.
                    allowedPassengerKind = true;
                }
                const ObjectDisabledMask portableBlocked =
                    objectDisabledBit(ObjectDisabledReason::Hacked) |
                    objectDisabledBit(ObjectDisabledReason::Emp) |
                    objectDisabledBit(ObjectDisabledReason::Subdued) |
                    objectDisabledBit(ObjectDisabledReason::Paralyzed);
                if (!nestedSpecialHost && allowedPassengerKind &&
                    (!portableStructure ||
                     (objectDisabledMask(registry, actor.entity,
                                         confirmedTick) & portableBlocked) == 0)) {
                    attackIntent = &hostQueue->orders.front();
                }
            }
        } else {
            // Structural children are attachments rather than ordinary
            // transport passengers, so preserve their independent order path.
            ObjectOrderQueueComponent* queue =
                ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                        actor.entity);
            if (nestedAttackIntent) {
                attackIntent = &*nestedAttackIntent;
            } else if (queue && !queue->orders.empty() &&
                queue->orders.front().kind == ObjectOrderKind::Attack) {
                attackIntent = &queue->orders.front();
                consumableQueue = aiOwnsDirectAttack ? nullptr : queue;
            }
        }
        if (!attackIntent) {
            advanceTurretsTowardNatural(
                weapons, logicFramesPerSecond, confirmedTick, random);
            weapons.target = INVALID_OBJECT_ID;
            static_cast<void>(releaseWeaponLock(
                weapons, ObjectWeaponLockType::Temporary));
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::Idle;
            continue;
        }
        const ObjectOrderIntent& order = *attackIntent;
        const bool hasPositionTarget = !order.targetObject && order.hasTargetPosition;
        if (!order.targetObject && !hasPositionTarget) {
            // Keep malformed internal callers from leaving an unconsumable
            // attack at the queue head. A valid ground/position attack is a
            // first-class Weapon path below, matching projectileFireAtObjectOrPosition.
            consumeAttackOrder(consumableQueue);
            weapons.target = INVALID_OBJECT_ID;
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::Idle;
            continue;
        }
        if (order.maximumShots && order.shotsFired >= *order.maximumShots) {
            consumeAttackOrder(consumableQueue);
            weapons.target = INVALID_OBJECT_ID;
            static_cast<void>(releaseWeaponLock(
                weapons, ObjectWeaponLockType::Temporary));
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::Idle;
            continue;
        }
        // RefCode's object-target attack query rejects source == victim. A
        // self-affecting weapon is represented by its radius/self mask or a
        // dedicated future SelfFire behavior, never by a normal Attack order.
        if (order.targetObject == actor.id) {
            consumeAttackOrder(consumableQueue);
            weapons.target = INVALID_OBJECT_ID;
            static_cast<void>(releaseWeaponLock(
                weapons, ObjectWeaponLockType::Temporary));
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::Idle;
            continue;
        }
        if (!sameActiveAttack(weapons, order)) {
            for (ObjectWeaponSetRuntime& set : weapons.sets) {
                for (ObjectWeaponSlotRuntime& slot : set.slots)
                    slot.leechRangeActive = false;
            }
            // DeliverPayload selects its strafing slot immediately before it
            // publishes a one-shot AttackPosition, and OCL's Attack nugget
            // does the same for its authored WeaponSlot. Do not erase that
            // fresh temporary lock merely because this is a new attack
            // identity; ordinary completion/no-order paths still release it.
            const bool producerSelectedSlotWithOrder =
                (order.source == ObjectOrderSource::System &&
                 (order.systemPurpose ==
                      ObjectOrderSystemPurpose::DeliverPayload ||
                  order.systemPurpose ==
                      ObjectOrderSystemPurpose::ObjectCreationAttack)) ||
                (order.source == ObjectOrderSource::Script &&
                 order.systemPurpose ==
                     ObjectOrderSystemPurpose::CommandButtonFireWeapon);
            const bool preserveProducerLock =
                producerSelectedSlotWithOrder &&
                weapons.lockType == ObjectWeaponLockType::Temporary;
            resetAttackLock(weapons, !preserveProducerLock);
            weapons.target = order.targetObject;
            weapons.activeOrderTick = order.issuedTick;
            weapons.activeOrderSequence = order.sourceSequence;
            weapons.activeSourceScriptId = order.sourceScriptId;
        }

        std::optional<ecs::entity> targetEntity;
        const ObjectHealthComponent* targetHealth = nullptr;
        const TransformComponent* targetTransform = nullptr;
        LogicFixedVec3 targetFixedPosition{};
        bool targetContainedPassenger = false;
        bool targetHiddenStealth = false;
        bool targetSameOwner = false;
        bool targetAllied = false;
        bool targetUnattackable = false;
        bool targetMasked = false;
        if (order.targetObject) {
            targetEntity = lifecycle.entityFromId(order.targetObject);
            targetHealth = targetEntity
                ? ecs::try_get<ObjectHealthComponent>(registry, *targetEntity) : nullptr;
            targetTransform = targetEntity
                ? ecs::try_get<TransformComponent>(registry, *targetEntity) : nullptr;
            aiSnapshot.targetValid = targetEntity && targetHealth &&
                targetTransform && targetHealth->acceptsDamage;
            aiSnapshot.targetEffectivelyDead = targetHealth &&
                targetHealth->effectivelyDead;
            if (targetEntity) {
                const ObjectContainedByComponent* targetContained =
                    ecs::try_get<ObjectContainedByComponent>(
                        registry, *targetEntity);
                targetContainedPassenger = targetContained &&
                    targetContained->enclosing;
                targetUnattackable = containsKind(
                    ecs::try_get<ObjectKindOfComponent>(
                        registry, *targetEntity),
                    game::ObjectKindOf::Unattackable);
                const ObjectStatusComponent* targetStatus =
                    ecs::try_get<ObjectStatusComponent>(registry,
                                                        *targetEntity);
                targetMasked = targetStatus && targetStatus->hasAny(
                    game::objectStatusBit(game::ObjectStatusFlag::Masked));
                const bool attackerIgnoresStealth = actorStatus &&
                    actorStatus->hasAny(game::objectStatusBit(
                        game::ObjectStatusFlag::IgnoringStealth));
                if (!attackerIgnoresStealth && targetStatus &&
                    targetStatus->hasAny(game::objectStatusBit(
                        game::ObjectStatusFlag::Stealthed)) &&
                    !targetStatus->hasAny(game::objectStatusBit(
                        game::ObjectStatusFlag::Detected)) &&
                    !targetStatus->hasAny(game::objectStatusBit(
                        game::ObjectStatusFlag::Disguised))) {
                    targetHiddenStealth = true;
                }
                if (!order.forceAttack && players) {
                    const OwnerComponent* sourceOwner =
                        ecs::try_get<OwnerComponent>(registry, actor.entity);
                    const OwnerComponent* targetOwner =
                        ecs::try_get<OwnerComponent>(registry, *targetEntity);
                    if (sourceOwner && targetOwner) {
                        targetSameOwner =
                            sourceOwner->player == targetOwner->player;
                        targetAllied = relationshipBetweenObjects(
                            registry, *players, actor.entity,
                            *targetEntity) == PlayerRelationship::Allies;
                    }
                }
            }
            if (targetTransform) {
                const LogicFixedVec3 fixedTarget =
                    readAuthoritativeObjectPosition(
                        registry, *targetEntity, *targetTransform);
                targetFixedPosition = fixedTarget;
                aiSnapshot.targetPosition = {
                    .xRaw = fixedTarget.x.raw(),
                    .yRaw = fixedTarget.y.raw(),
                    .zRaw = fixedTarget.z.raw(),
                };
                const ObjectKindOfComponent* targetKind =
                    ecs::try_get<ObjectKindOfComponent>(registry,
                                                        *targetEntity);
                aiSnapshot.targetMobile = !containsKind(
                    targetKind, game::ObjectKindOf::Immobile);
                aiSnapshot.canPursue = aiSnapshot.targetMobile;
            }
            // KINDOF_UNATTACKABLE joins the terminal target-validity gate
            // rather than only the AI snapshot: RefCode rejects the victim
            // inside WeaponSet::getAbleToAttackSpecificObject, which every
            // command source (player, script, AI, force-fire) funnels through.
            if (!targetEntity || !targetHealth || !targetTransform || !targetHealth->acceptsDamage ||
                targetHealth->effectivelyDead || targetUnattackable ||
                targetMasked) {
                m_events.push_back({
                    .kind = ObjectWeaponEventKind::TargetLost,
                    .source = actor.id,
                    .target = order.targetObject,
                    .confirmedTick = confirmedTick,
                });
                consumeAttackOrder(consumableQueue);
                weapons.target = INVALID_OBJECT_ID;
                static_cast<void>(releaseWeaponLock(
                    weapons, ObjectWeaponLockType::Temporary));
                releaseContainedGarrisonFirePoint();
                weapons.state = ObjectWeaponRuntimeState::Idle;
                continue;
            }
        } else {
            targetFixedPosition = {
                order.targetX, order.targetY, order.targetZ};
        }
        if (hasActiveAIOperation && operation->attackLimitReached) {
            // AttackMove's parent Move remains in its queue. Expose the shot
            // cap as a terminal target fact for the AI child, then leave the
            // locomotion owner to resume that Move order.
            aiSnapshot.targetEffectivelyDead = true;
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::TrackingTarget;
            continue;
        }
        const bool targetPolicyAllowed =
            objectAIAttackTargetPolicyAllowed({
                .forceAttack = order.forceAttack,
                .containedPassenger = targetContainedPassenger,
                .hiddenStealth = targetHiddenStealth,
                .sameOwner = targetSameOwner,
                .allied = targetAllied,
                .unattackable = targetUnattackable,
            });

        const ObjectCombatProfileComponent* combat =
            ecs::try_get<ObjectCombatProfileComponent>(registry, actor.entity);
        if (!combat || !combat->profile) {
            consumeAttackOrder(consumableQueue);
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::NoUsableWeapon;
            continue;
        }
        const container::Span<const game::WeaponSetProfile> authoredSets = combat->profile->weaponSets();
        const game::WeaponSetProfile* authoredSet =
            combat->profile->findBestWeaponSet(combat->weaponConditions);
        if (!authoredSet || authoredSets.empty()) {
            consumeAttackOrder(consumableQueue);
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::NoUsableWeapon;
            continue;
        }
        const size_t setIndex = static_cast<size_t>(authoredSet - authoredSets.data());
        if (setIndex >= weapons.sets.size()) {
            consumeAttackOrder(consumableQueue);
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::NoUsableWeapon;
            continue;
        }
        ObjectWeaponSetRuntime& runtimeSet = weapons.sets[setIndex];
        static_cast<void>(activateWeaponSetRuntime(
            weapons, setIndex, content, logicFramesPerSecond,
            confirmedTick));
        const bool sharedReloading =
            runtimeSet.sharedReloadCompleteTick != 0 && confirmedTick < runtimeSet.sharedReloadCompleteTick;
        const TransformComponent& actorTransform =
            ecs::get<TransformComponent>(registry, actor.entity);
        LogicFixedVec3 sourcePosition = readAuthoritativeObjectPosition(
            registry, actor.entity, actorTransform);
        Fixed sourceYaw = readAuthoritativeObjectYaw(
            registry, actor.entity, actorTransform);
        const ObjectGeometryComponent* sourceGeometry = ecs::try_get<ObjectGeometryComponent>(registry, actor.entity);
        if (contained && contained->container)
        {
            const std::optional<ecs::entity> host = lifecycle.entityFromId(contained->container);
            const TransformComponent* hostTransform =
                host ? ecs::try_get<TransformComponent>(registry, *host) : nullptr;
            if (host && hostTransform)
            {
                const ObjectContainmentRuntimeComponent* hostRuntime =
                    ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *host);
                const ObjectContainmentRule* containmentRule =
                    hostRuntime && hostRuntime->plan &&
                            contained->containmentRuleIndex < hostRuntime->plan->rules.size()
                        ? &hostRuntime->plan->rules[contained->containmentRuleIndex]
                        : nullptr;
                const bool garrison = containmentRule && containmentRule->kind == ObjectContainmentKind::Garrison;
                // Enclosed passengers fire from host FIREPOINTs. A
                // non-enclosing garrison remains independently targetable
                // but still fires from its assigned STATION. Portable
                // Overlord/Helix add-ons keep their own model/weapon origin.
                if (contained->enclosing || garrison)
                {
                    sourcePosition = readAuthoritativeObjectPosition(registry, *host, *hostTransform);
                    sourceYaw = readAuthoritativeObjectYaw(
                        registry, *host, *hostTransform);
                    sourceGeometry = nullptr;
                    const ObjectContainmentComponent* hostContents =
                        ecs::try_get<ObjectContainmentComponent>(registry, *host);
                    if (hostContents && containmentRule)
                    {
                        const bool station = garrison && !containmentRule->enclosingContainer;
                        const container::StringView pointPrefix =
                            station ? container::StringView{"STATION"} : container::StringView{"FIREPOINT"};
                        const size_t pointLimit = garrison ? 40u : 32u;
                        container::Vector<LogicFixedVec3> points;
                        points.reserve(pointLimit);
                        for (size_t ordinal = 1; ordinal <= pointLimit; ++ordinal)
                        {
                            container::String pointName{pointPrefix};
                            if (ordinal < 10u)
                                pointName.push_back('0');
                            pointName += std::to_string(ordinal);
                            const auto point = containmentFirePointWorldPosition(registry,
                                                                                 *host,
                                                                                 content,
                                                                                 pointName,
                                                                                 containmentRule->passengersInTurret,
                                                                                 sourcePosition,
                                                                                 *hostTransform);
                            // getMultiLogicalBonePosition consumes one dense
                            // Name01..NameNN prefix and stops at its first gap.
                            if (!point)
                                break;
                            points.push_back(*point);
                        }

                        size_t passengerOrdinal = 0;
                        bool foundPassenger = false;
                        for (const ObjectContainedObjectRecord& record : hostContents->objects)
                        {
                            if (record.object == actor.id)
                            {
                                foundPassenger = true;
                                break;
                            }
                            // STATION and Open cursor placement use the
                            // complete stable contained order, including
                            // occupants which do not currently own a weapon.
                            ++passengerOrdinal;
                        }
                        if (foundPassenger && !points.empty())
                        {
                            std::optional<size_t> selected;
                            if (garrison && !station)
                            {
                                ObjectGarrisonFirePointComponent* firePoints =
                                    ecs::try_get<ObjectGarrisonFirePointComponent>(
                                        registry, *host);
                                if (!firePoints)
                                    firePoints = &ecs::emplace<
                                        ObjectGarrisonFirePointComponent>(
                                            registry, *host);
                                // RefCode's GarrisonContain::trackTargets keeps
                                // a passenger's point across ticks, then moves
                                // it only when a free point is strictly closer
                                // to the newly-bound target. ObjectId ordering
                                // makes admission and tie-breaking stable.
                                const uint64_t firePointRevision =
                                    firePoints->revision;
                                selected = assignGarrisonFirePoint(
                                    *firePoints, actor.id,
                                    targetEntity ? order.targetObject
                                                 : INVALID_OBJECT_ID,
                                    targetFixedPosition, points);
                                if (selected) {
                                    // assignGarrisonFirePoint may choose a
                                    // different point than the provisional
                                    // value above. Seal the exact selected
                                    // world point into the value-only
                                    // presentation record.
                                    const auto assignment = std::lower_bound(
                                        firePoints->assignments.begin(),
                                        firePoints->assignments.end(), actor.id,
                                        [](const ObjectGarrisonFirePointAssignment& value,
                                           ObjectId id) {
                                            return value.occupant < id;
                                        });
                                    if (assignment != firePoints->assignments.end() &&
                                        assignment->occupant == actor.id &&
                                        assignment->pointIndex < points.size() &&
                                        (assignment->pointPosition.x != points[assignment->pointIndex].x ||
                                         assignment->pointPosition.y != points[assignment->pointIndex].y ||
                                         assignment->pointPosition.z != points[assignment->pointIndex].z)) {
                                        assignment->pointPosition =
                                            points[assignment->pointIndex];
                                        ++firePoints->revision;
                                    }
                                }
                                if (firePoints->revision != firePointRevision) {
                                    markObjectDirty(
                                        registry, *host,
                                        ObjectDirtyDomain::RenderExtraction);
                                }
                            }
                            else
                            {
                                // OpenContain wraps its fixed firepoint cursor;
                                // non-enclosing garrisons retain one STATION per
                                // passenger rather than following the target.
                                selected = passengerOrdinal % points.size();
                            }
                            if (selected)
                            {
                                sourcePosition = points[*selected];
                            }
                        }
                        else if (garrison && !station)
                        {
                            // A model-condition change may temporarily expose
                            // no dense FIREPOINT prefix. Do not retain a stale
                            // logical reservation while combat falls back to
                            // the host origin.
                            releaseGarrisonFirePoint(registry, *host, actor.id);
                        }
                    }
                }
            }
        }
        const ObjectGeometryComponent* targetGeometry =
            targetEntity ? ecs::try_get<ObjectGeometryComponent>(registry, *targetEntity) : nullptr;
        const ObjectKindOfComponent* targetKinds =
            targetEntity ? ecs::try_get<ObjectKindOfComponent>(registry, *targetEntity) : nullptr;
        const ObjectTerrainLayerComponent* targetTerrainLayer =
            targetEntity ? ecs::try_get<ObjectTerrainLayerComponent>(registry, *targetEntity) : nullptr;
        const ObjectAirborneComponent* targetAirborne =
            targetEntity ? ecs::try_get<ObjectAirborneComponent>(registry, *targetEntity) : nullptr;
        const ObjectArmorComponent* targetArmor =
            targetEntity ? ecs::try_get<ObjectArmorComponent>(registry, *targetEntity) : nullptr;
        const ObjectStatusComponent* targetStatus =
            targetEntity ? ecs::try_get<ObjectStatusComponent>(registry, *targetEntity) : nullptr;
        const ObjectLocomotionComponent* targetLocomotion =
            targetEntity ? ecs::try_get<ObjectLocomotionComponent>(registry, *targetEntity) : nullptr;
        const bool targetDoingGroundMovement =
            targetLocomotion && (!targetAirborne || !targetAirborne->isAirborne) &&
            (!targetEntity || (objectDisabledMask(registry, *targetEntity, confirmedTick) &
                               objectDisabledBit(ObjectDisabledReason::Held)) == 0);
        const Fixed distance = combatDistance(
            sourcePosition, sourceGeometry, targetFixedPosition,
            targetGeometry);
        const ObjectWeaponBonusComponent* bonusState = ecs::try_get<ObjectWeaponBonusComponent>(registry, actor.entity);
        game::WeaponBonusConditionMask bonusConditions =
            bonusState ? bonusState->conditions : game::WeaponBonusConditionMask{};
        // OpenContain::getWeaponBonusPassedToPassengers forwards the host's
        // complete condition mask while this passenger resolves its weapon.
        // Keep that as an ephemeral Combat projection: the passenger does
        // not permanently acquire host bonuses when it exits or changes
        // transport in the same confirmed session.
        if (contained && contained->container)
        {
            const std::optional<ecs::entity> host =
                lifecycle.entityFromId(contained->container);
            const ObjectContainmentRuntimeComponent* hostRuntime = host
                ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry,
                                                                   *host)
                : nullptr;
            if (hostRuntime && hostRuntime->plan &&
                contained->containmentRuleIndex <
                    hostRuntime->plan->rules.size() &&
                hostRuntime->plan->rules[contained->containmentRuleIndex]
                    .weaponBonusPassedToPassengers) {
                const ObjectWeaponBonusComponent* hostBonus =
                    ecs::try_get<ObjectWeaponBonusComponent>(registry, *host);
                if (hostBonus) bonusConditions |= hostBonus->conditions;
            }
        }

        container::Array<game::WeaponSlotSelectionCandidate, game::kWeaponSlotCount> choices;
        container::Array<game::WeaponBonus, game::kWeaponSlotCount> resolvedBonuses;
        for (size_t index = 0; index < game::kWeaponSlotCount; ++index) {
            const game::WeaponSlotProfile& authoredSlot = authoredSet->slots[index];
            ObjectWeaponSlotRuntime& runtimeSlot = runtimeSet.slots[index];
            const game::WeaponTemplate* definition = content.findWeapon(runtimeSlot.content);
            game::WeaponSlotSelectionCandidate& choice = choices[index];
            if (!authoredSlot.hasWeapon() || !definition) continue;
            game::WeaponBonus& bonus = resolvedBonuses[index];
            bonus = content.resolveWeaponBonus(*definition, bonusConditions);

            const bool compatible = targetMatchesAntiMask(
                *definition, targetKinds, targetAirborne,
                targetStatus);
            const bool pitch = pitchMatches(
                *definition, sourcePosition, targetFixedPosition);
            const bool empty = hasFiniteEmptyClip(runtimeSlot, *definition);
            const bool reloading = isReloading(runtimeSlot, confirmedTick);
            choice.present = true;
            choice.outOfAmmo = empty;
            choice.autoReloadsClip = definition->reloadType == game::WeaponReloadType::Auto;
            // WeaponSet chooses the best compatible weapon independently of
            // current range; the attack/locomotion state is responsible for
            // approaching that weapon's range. Stage-1 has no pursuit yet,
            // so the actual-fire gate below reports OutOfRange after this
            // faithful selection instead of incorrectly falling back to a
            // weaker in-range slot.
            choice.canTarget = compatible;
            choice.withinTargetPitch = pitch;
            choice.readyToFire = !empty && !reloading && !sharedReloading &&
                runtimeSlot.nextReadyTick <= confirmedTick &&
                runtimeSlot.preAttackCompleteTick <= confirmedTick;
            choice.permitsZeroDamage = definition->damageType == game::DamageType::UNRESISTABLE;
            choice.preferredAgainstTarget = matchesPreferredAgainst(authoredSlot, targetKinds);
            choice.estimatedDamage = estimatedDamage(
                *definition, bonus, targetArmor, targetHealth, targetKinds,
                targetStatus, registry, lifecycle, targetEntity);
            choice.attackRange = resolvedAttackRange(*definition, bonus);
        }

        if (hasActiveAIOperation) {
            for (const game::WeaponSlotSelectionCandidate& choice : choices) {
                aiSnapshot.hasWeapon = aiSnapshot.hasWeapon || choice.present;
                aiSnapshot.canPossiblyAttack =
                    aiSnapshot.canPossiblyAttack ||
                    (choice.present && choice.canTarget &&
                     (!choice.outOfAmmo || choice.autoReloadsClip));
            }
            aiSnapshot.attackAllowed = aiSnapshot.targetValid &&
                targetPolicyAllowed &&
                aiSnapshot.canPossiblyAttack;
        }

        std::optional<game::WeaponSlot> locked;
        if (weapons.lockedSlot) {
            const size_t lockedIndex = static_cast<size_t>(*weapons.lockedSlot);
            if (lockedIndex < choices.size() && choices[lockedIndex].present) {
                locked = weapons.lockedSlot;
            } else {
                static_cast<void>(releaseWeaponLock(
                    weapons, ObjectWeaponLockType::Permanent));
            }
        }
        const game::WeaponSlotSelection selected = game::chooseBestWeaponSlot(
            *authoredSet, choices, toWeaponCommandSource(order.source),
            game::WeaponChoiceCriterion::MostDamage, true, locked);
        aiSnapshot.weaponSlotAllowed = selected.found;
        if (hasActiveAIOperation && selected.found && targetEntity &&
            !targetContainedPassenger && aiInput.aiCrushesInfantry) {
            const OwnerComponent* sourceOwner =
                ecs::try_get<OwnerComponent>(registry, actor.entity);
            const PlayerState* sourcePlayer = sourceOwner && players
                ? players->get(sourceOwner->player) : nullptr;
            const ThingTemplateComponent* sourceType =
                ecs::try_get<ThingTemplateComponent>(registry, actor.entity);
            const ThingTemplateComponent* victimType =
                ecs::try_get<ThingTemplateComponent>(registry, *targetEntity);
            const game::ThingTemplate* sourceTemplate =
                sourceType && sourceType->archetype
                    ? &sourceType->archetype->templateData : nullptr;
            const game::ThingTemplate* victimTemplate =
                victimType && victimType->archetype
                    ? &victimType->archetype->templateData : nullptr;
            const ObjectKindOfComponent* sourceKinds =
                ecs::try_get<ObjectKindOfComponent>(registry, actor.entity);
            const bool computerControlled = sourcePlayer &&
                sourcePlayer->controller == PlayerControllerKind::Ai;
            const bool unmanned = isObjectDisabledBy(
                registry, actor.entity, ObjectDisabledReason::Unmanned,
                confirmedTick);
            const bool canCrush = computerControlled && !unmanned &&
                sourceTemplate && victimTemplate &&
                sourceTemplate->crusherLevel != 0 && !targetAllied &&
                (static_cast<bool>(victimType->archetype->squishCollidePlan) ||
                 sourceTemplate->crusherLevel >
                     victimTemplate->crushableLevel);
            if (canCrush) {
                const uint8_t slotBit = static_cast<uint8_t>(
                    uint8_t{1} << static_cast<size_t>(selected.slot));
                const bool turreted = std::any_of(
                    weapons.turrets.begin(), weapons.turrets.end(),
                    [slotBit](const ObjectTurretRuntime& turret) noexcept {
                        return (turret.controlledWeaponSlots & slotBit) != 0;
                    });
                aiSnapshot.wantToSquishTarget = turreted &&
                    !(sourceKinds && game::objectHasKind(
                        sourceKinds->mask,
                        game::ObjectKindOf::DontAutoCrushInfantry));
                // RefCode canPursue() always permits a computer crusher to
                // pursue a crushable victim, even when ordinary speed tests
                // would reject the chase.
                aiSnapshot.canPursue = true;
            }
        }
        if (!selected.found) {
            // A finite NO/RETURN_TO_BASE clip is deliberately excluded by
            // selection. Until a base-reload system exists, it must terminate
            // the intent rather than leaving an unconsumable Attack at the
            // queue head forever.
            weapons.state = ObjectWeaponRuntimeState::NoUsableWeapon;
            if (!hasActiveAIOperation) {
                m_events.push_back({
                    .kind = ObjectWeaponEventKind::WeaponUnavailable,
                    .source = actor.id,
                    .target = order.targetObject,
                    .confirmedTick = confirmedTick,
                });
            }
            consumeAttackOrder(consumableQueue);
            weapons.target = INVALID_OBJECT_ID;
            static_cast<void>(releaseWeaponLock(
                weapons, ObjectWeaponLockType::Temporary));
            releaseContainedGarrisonFirePoint();
            continue;
        }

        weapons.currentSlot = selected.slot;

        const size_t slotIndex = static_cast<size_t>(selected.slot);
        ObjectWeaponSlotRuntime& runtimeSlot = runtimeSet.slots[slotIndex];
        const game::WeaponTemplate* definition = content.findWeapon(runtimeSlot.content);
        if (!definition) {
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::NoUsableWeapon;
            continue;
        }
        const game::WeaponBonus& selectedBonus = resolvedBonuses[slotIndex];
        aiSnapshot.weaponPreAttack = runtimeSlot.preAttackArmed &&
            runtimeSlot.preAttackCompleteTick > confirmedTick;
        aiSnapshot.weaponReady = selected.usesReadyWeapon &&
            !aiSnapshot.weaponPreAttack;
        const Fixed selectedTurretAttackRangeFixed = Fixed::max(
            kFixedZero, selectedBonus.scale(
                definition->fixed.attackRange,
                game::WeaponBonusField::Range));
        const bool forcePositionFire = hasPositionTarget &&
            order.source == ObjectOrderSource::Script &&
            order.systemPurpose ==
                ObjectOrderSystemPurpose::CommandButtonFireWeapon;
        const bool targetUsesGroundPitch = hasPositionTarget ||
            containsKind(targetKinds, game::ObjectKindOf::Immobile) ||
            targetDoingGroundMovement;
        advanceTurretsTowardTarget(
            weapons, sourcePosition, sourceYaw, targetFixedPosition,
            sourceGeometry,
            targetGeometry, selected.slot, selectedTurretAttackRangeFixed,
            targetUsesGroundPitch, logicFramesPerSecond, confirmedTick);
        // FIRE_WEAPON at a position is the legacy force-fire path.  Its
        // clicked point is the projectile destination, not an object attack
        // victim that the actor must first approach.  RefCode passes
        // ignoreRanges=true to Weapon::privateFireWeapon for this command;
        // keep the authored ready/cooldown, pitch, turret and projectile
        // gates below while bypassing only the ordinary attack-radius gate.
        const bool selectedInRange = forcePositionFire ||
            runtimeSlot.leechRangeActive ||
            isWithinRange(*definition, selectedBonus, distance);
        aiSnapshot.inRange = selectedInRange;
        aiSnapshot.contactWeapon = selectedTurretAttackRangeFixed <=
            kFixedRationalizedRangeUndersize;
        aiSnapshot.attackArrivalRadiusRaw =
            resolvedAttackRange(*definition, selectedBonus).raw();
        aiSnapshot.attackMinimumArrivalRadiusRaw = Fixed::max(
            kFixedZero,
            definition->fixed.minimumAttackRange -
                kFixedRationalizedRangeUndersize).raw();
        // Pathfinder::isAttackViewBlockedByObstacle, reached from
        // AIStates::outOfWeaponRangeObject and AIAttackApproachTargetState.
        // A blocked view keeps the unit out of Aim/Fire and sends it into
        // Chase/Approach, which the AI kernel already implements from
        // AIAttackFeedback::viewBlocked; only the producer was missing.
        if (hasActiveAIOperation && attackLineOfSightEnabled &&
            !runtimeSlot.leechRangeActive) {
            aiSnapshot.viewBlocked = objectAttackViewBlockedByObstacle(
                registry, lifecycle, aiInput.navigation,
                aiInput.attackUsesLineOfSight, seeThroughObstacles,
                actor.entity, actor.id, sourcePosition, order.targetObject,
                targetFixedPosition, aiSnapshot.contactWeapon);
        }
        if (!selectedInRange) {
            // RefCode's attemptBestFirePointPosition is transactional: a
            // point chosen for range testing is returned immediately when
            // the weapon cannot attack from it.
            releaseContainedGarrisonFirePoint();
            weapons.state = ObjectWeaponRuntimeState::OutOfRange;
            continue;
        }
        const bool turretAligned = turretAlignedForWeaponSlot(
                weapons, selected.slot, sourcePosition, sourceYaw,
                targetFixedPosition,
                sourceGeometry, targetGeometry,
                selectedTurretAttackRangeFixed,
                definition->fixed.acceptableAimDeltaRadians,
                targetUsesGroundPitch);
        const bool jetLockonBlocked = targetEntity &&
            jetAimTemporarilyPrevented(
                registry, lifecycle, order.targetObject,
                actor.id, confirmedTick);
        aiSnapshot.aimTemporarilyPrevented = jetLockonBlocked;
        aiSnapshot.aimReady = turretAligned && !jetLockonBlocked;
        if (!turretAligned || jetLockonBlocked) {
            // Aim is an authoritative gameplay phase.  Cooldown/ammo/barrel
            // state must not advance until TurretAI has reached the target;
            // rendering merely samples the same confirmed yaw and pitch.
            weapons.state = ObjectWeaponRuntimeState::TrackingTarget;
            continue;
        }
        if (hasActiveAIOperation &&
            operation->phase == ai::AIAttackPhase::Aim) {
            // BeginAim owns target/turret tracking only. Cooldown and prefire
            // are reported, but no pre-attack marker or shot is authored.
            weapons.state = ObjectWeaponRuntimeState::TrackingTarget;
            continue;
        }
        if (!selected.usesReadyWeapon) {
            weapons.state = sharedReloading || isReloading(runtimeSlot, confirmedTick) ||
                    hasFiniteEmptyClip(runtimeSlot, *definition)
                ? ObjectWeaponRuntimeState::Reloading
                : runtimeSlot.preAttackCompleteTick > confirmedTick
                    ? ObjectWeaponRuntimeState::WindingUp
                    : ObjectWeaponRuntimeState::CoolingDown;
            continue;
        }
        if (runtimeSlot.preAttackArmed && runtimeSlot.preAttackCompleteTick <= confirmedTick) {
            // The pending prefire has matured exactly once.  Keep the
            // PerAttack/PerClip markers so the selected prefire policy can
            // decide whether a later shot needs another delay.
            runtimeSlot.preAttackArmed = false;
            aiSnapshot.weaponPreAttack = false;
            aiSnapshot.weaponReady = true;
        } else if (requiresPreAttack(runtimeSlot, *definition, order)) {
            if (beginPreAttack(runtimeSlot, *definition, selectedBonus, order,
                               logicFramesPerSecond, confirmedTick)) {
                if (RenderModelComponent* visual =
                        ecs::try_get<RenderModelComponent>(
                            registry, actor.entity)) {
                    const uint64_t durationTicks =
                        runtimeSlot.preAttackCompleteTick > confirmedTick
                        ? runtimeSlot.preAttackCompleteTick - confirmedTick
                        : 0u;
                    visual->weaponPreattackLoopDurationSeconds =
                        static_cast<float>(durationTicks) /
                        static_cast<float>(std::max<uint32_t>(
                            1u, logicFramesPerSecond));
                }
                ecs::remove<ObjectUndetectedDefectorComponent>(
                    registry, actor.entity);
                aiSnapshot.weaponPreAttack = true;
                aiSnapshot.weaponReady = false;
                weapons.state = ObjectWeaponRuntimeState::WindingUp;
                continue;
            }
        }

        if (hasActiveAIOperation && !operation->fireRequested) {
            // BeginFire may arm and mature pre-attack, but only an explicit
            // Fire value grants one firing cycle.
            aiSnapshot.weaponPreAttack = runtimeSlot.preAttackArmed;
            aiSnapshot.weaponReady = !runtimeSlot.preAttackArmed;
            weapons.state = ObjectWeaponRuntimeState::TrackingTarget;
            continue;
        }

        const auto fireSlotNow = [&](size_t fireIndex) -> bool {
            ObjectWeaponSlotRuntime& firingRuntime = runtimeSet.slots[fireIndex];
            const game::WeaponTemplate* firingDefinition =
                content.findWeapon(firingRuntime.content);
            if (!firingDefinition) return false;
            const game::WeaponSlot firingSlot =
                static_cast<game::WeaponSlot>(fireIndex);
            const game::WeaponBonus& firingBonus = resolvedBonuses[fireIndex];

            const uint32_t shotSequence = weapons.nextShotSequence++;
            if (weapons.nextShotSequence == 0) ++weapons.nextShotSequence;
            // Actual firing immediately blows defector cover at the same
            // gameplay boundary as damage/projectile emission; waiting for
            // the later presentation event would leave one stale relation.
            ecs::remove<ObjectUndetectedDefectorComponent>(registry,
                                                            actor.entity);
            const auto firingConditions = firingPresentationConditions(
                registry, actor.entity, firingSlot);
            const auto presentation = pristineWeaponPresentation(
                registry, actor.entity, content, firingSlot,
                firingConditions ? &*firingConditions : nullptr);
            const uint32_t barrelSequenceOrdinal =
                game::selectAndAdvanceWeaponBarrel(
                    firingRuntime.currentBarrel,
                    firingRuntime.shotsRemainingForCurrentBarrel,
                    presentation
                        ? static_cast<uint32_t>(
                              presentation->barrels.barrels.size())
                        : 0u,
                    firingDefinition->shotsPerBarrel);
            const WeaponLaunchTransform weaponLaunch =
                pristineWeaponLaunchTransform(
                    registry, actor.entity, content, firingSlot,
                    barrelSequenceOrdinal, sourcePosition,
                    firingConditions ? &*firingConditions : nullptr);
            const uint64_t readyAt = saturatingTickAdd(
                confirmedTick,
                chooseShotDelayFrames(*firingDefinition,
                                      logicFramesPerSecond, random,
                                      firingBonus));
            firingRuntime.nextReadyTick = readyAt;
            firingRuntime.previousFireTick = firingRuntime.lastFireTick;
            firingRuntime.previousFireSequence = firingRuntime.lastFireSequence;
            firingRuntime.lastFireTick = confirmedTick;
            firingRuntime.lastFireSequence = shotSequence;
            notifyTurretWeaponFired(
                weapons, firingSlot, confirmedTick);
            if (firingDefinition->clipSize > 0) {
                if (firingRuntime.ammoInClip > 0) --firingRuntime.ammoInClip;
                if (firingRuntime.ammoInClip == 0 &&
                    firingDefinition->reloadType ==
                        game::WeaponReloadType::Auto) {
                    const uint64_t reloadFrames = clipReloadFrames(
                        *firingDefinition, firingBonus,
                        logicFramesPerSecond);
                    firingRuntime.reloadCompleteTick = saturatingTickAdd(
                        confirmedTick, std::max<uint64_t>(1, reloadFrames));
                    firingRuntime.reloadReplenishesClip = true;
                    firingRuntime.nextReadyTick = std::max(
                        firingRuntime.nextReadyTick,
                        firingRuntime.reloadCompleteTick);
                    if (runtimeSet.shareWeaponReloadTime) {
                        runtimeSet.sharedReloadCompleteTick = std::max(
                            runtimeSet.sharedReloadCompleteTick,
                            firingRuntime.reloadCompleteTick);
                    }
                }
            }
            if (runtimeSet.shareWeaponReloadTime) {
                const uint64_t sharedReadyAt = std::max(
                    readyAt, runtimeSet.sharedReloadCompleteTick);
                for (ObjectWeaponSlotRuntime& sibling : runtimeSet.slots) {
                    sibling.nextReadyTick = std::max(
                        sibling.nextReadyTick, sharedReadyAt);
                }
            }

            LogicFixedVec3 impactPosition = targetFixedPosition;
            if (targetGeometry &&
                targetGeometry->shape != ObjectGeometryShape::Sphere) {
                impactPosition.z += Fixed::max(
                    kFixedZero, targetGeometry->heightFixed) /
                    Fixed{int32_t{2}};
            }
            ObjectSystemWeaponFireCommand command{
                .source = actor.id,
                .target = order.targetObject,
                .content = firingRuntime.content,
                .bonusConditions = bonusConditions,
                .sourcePosition = sourcePosition,
                .impactPosition = impactPosition,
                .intendedTargetBasePosition =
                    targetFixedPosition,
                .sourceShotSequence = shotSequence,
                .sourceBarrelSequenceOrdinal =
                    barrelSequenceOrdinal,
                .projectileStreamOwnerGeneration =
                    weapons.weaponSetGeneration,
                .launchSlot = firingSlot,
                .usesFiringPresentation = true,
                .authoredOrder = static_cast<uint32_t>(fireIndex),
                .emissionSequence =
                    reserveFrameFireEmissionSequence(),
                .weaponFxSuspendedByDelay =
                    confirmedTick <
                        firingRuntime.suspendFxUntilTick,
                .hasIntendedTargetBasePosition =
                    static_cast<bool>(order.targetObject),
                .confirmedTick = confirmedTick,
            };
            if (!firingDefinition->projectileObject.empty()) {
                ObjectProjectileSpawnRequest projectileSample{
                    .launcher = actor.id,
                    .intendedTarget = order.targetObject,
                    .detonationWeapon = firingRuntime.content,
                    .projectileTemplate =
                        firingDefinition->projectileObject,
                    .launchPosition = weaponLaunch.position,
                    .targetPosition = impactPosition,
                    .intendedTargetBasePosition =
                        targetFixedPosition,
                    .sourceShotSequence = shotSequence,
                    .sourceBarrelSequenceOrdinal =
                        barrelSequenceOrdinal,
                    .hasIntendedTargetBasePosition =
                        static_cast<bool>(order.targetObject),
                    .confirmedTick = confirmedTick,
                };
                applyProjectileScatter(
                    projectileSample, *firingDefinition, targetKinds,
                    targetTerrainLayer
                        ? targetTerrainLayer->pathfindLayer
                        : game::terrain::kGroundPathfindLayer,
                    random, &firingRuntime.scatterTargetsUnused);
                populateTumbleLaunchRates(
                    projectileSample, content, random);
                command.target = projectileSample.intendedTarget;
                command.impactPosition =
                    projectileSample.targetPosition;
                command.targetWasScattered =
                    projectileSample.targetWasScattered;
                command.scatteredTargetPathfindLayer =
                    projectileSample.scatteredTargetPathfindLayer;
                command.hasTumbleAngularRates =
                    projectileSample.hasTumbleAngularRates;
                command.tumbleYawRate =
                    projectileSample.tumbleYawRate;
                command.tumblePitchRate =
                    projectileSample.tumblePitchRate;
                command.tumbleRollRate =
                    projectileSample.tumbleRollRate;
            }
            actorWeaponFires.push_back(std::move(command));
            notifyObjectFiringTrackerShot(
                registry, lifecycle, actor.entity, actor.id,
                order.targetObject, weapons, *firingDefinition,
                firingRuntime.content, firingSlot,
                firingRuntime.nextReadyTick,
                content, random, logicFramesPerSecond, confirmedTick,
                m_events);
            if (firingDefinition->leechRangeWeapon)
                firingRuntime.leechRangeActive = true;
            return true;
        };

        bool firedCycle = false;
        if (weapons.turretsLinked && !order.targetObject) {
            // RefCode supports linked turrets only in the position-fire path
            // and invokes every Weapon instance in strict WeaponSlot order.
            // Siblings do not repeat AI target/range/turret/pre-attack
            // admission; Weapon::fireWeapon only rejects a non-READY runtime.
            for (size_t linkedIndex = 0;
                 linkedIndex < game::kWeaponSlotCount; ++linkedIndex) {
                ObjectWeaponSlotRuntime& linkedRuntime =
                    runtimeSet.slots[linkedIndex];
                const game::WeaponTemplate* linkedDefinition =
                    content.findWeapon(linkedRuntime.content);
                if (!linkedDefinition ||
                    linkedRuntime.nextReadyTick > confirmedTick ||
                    isReloading(linkedRuntime, confirmedTick) ||
                    linkedRuntime.preAttackCompleteTick > confirmedTick ||
                    hasFiniteEmptyClip(linkedRuntime, *linkedDefinition)) {
                    continue;
                }
                if (linkedRuntime.preAttackArmed) {
                    linkedRuntime.preAttackArmed = false;
                }
                firedCycle = fireSlotNow(linkedIndex) || firedCycle;
            }
        } else {
            firedCycle = fireSlotNow(slotIndex);
        }
        if (!firedCycle) {
            weapons.state = ObjectWeaponRuntimeState::CoolingDown;
            continue;
        }
        if (hasActiveAIOperation) {
            if (operation->hasMaximumShots &&
                operation->shotsFired != std::numeric_limits<uint32_t>::max()) {
                ++operation->shotsFired;
                if (operation->shotsFired >= operation->maximumShots) {
                    operation->attackLimitReached = true;
                    releaseContainedGarrisonFirePoint();
                }
            }
            operation->fireRequested = false;
            ai::AIAttackFeedback completed = aiSnapshot;
            completed.kind = ai::AIAttackFeedbackKind::FireCompleted;
            appendAIAttackFeedbackOnce(m_aiAttackFeedback, completed);
            snapshotScope.cancel();
            weapons.state = ObjectWeaponRuntimeState::TrackingTarget;
            continue;
        }
        if (consumableQueue && !consumableQueue->orders.empty() &&
            consumableQueue->orders.front().kind == ObjectOrderKind::Attack &&
            consumableQueue->orders.front().maximumShots.has_value()) {
            ObjectOrderIntent& admitted = consumableQueue->orders.front();
            if (admitted.shotsFired != std::numeric_limits<uint32_t>::max()) {
                ++admitted.shotsFired;
            }
            if (admitted.shotsFired >= *admitted.maximumShots) {
                consumeAttackOrder(consumableQueue);
                weapons.target = INVALID_OBJECT_ID;
                static_cast<void>(releaseWeaponLock(
                    weapons, ObjectWeaponLockType::Temporary));
                releaseContainedGarrisonFirePoint();
                weapons.state = ObjectWeaponRuntimeState::Idle;
                continue;
            }
        }
        weapons.state = ObjectWeaponRuntimeState::TrackingTarget;
    }
    return !sinkFailed;
}

} // namespace engine
