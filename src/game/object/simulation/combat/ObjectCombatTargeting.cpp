#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectWeaponDamage.h"
#include "game/object/simulation/combat/ObjectWeaponTargetPolicy.h"
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

namespace engine::object_combat_detail {

bool targetMatchesAntiMask(
    const game::WeaponTemplate& weapon,
    const ObjectKindOfComponent* kinds,
    const ObjectAirborneComponent* airborne,
    const ObjectStatusComponent* status) noexcept {
    return weaponTargetMatchesAntiMask(
        weapon, kinds, airborne, status);
}

[[nodiscard]] Fixed combatDistance(
    const LogicFixedVec3& source,
    const ObjectGeometryComponent* sourceGeometry,
    const LogicFixedVec3& target,
    const ObjectGeometryComponent* targetGeometry) noexcept {
    const Fixed dx = target.x - source.x;
    const Fixed dy = target.y - source.y;
    const Fixed centerDistance = Fixed::sqrt(dx * dx + dy * dy);
    const Fixed sourceRadius = sourceGeometry
        ? Fixed::max(kFixedZero,
              sourceGeometry->boundingCircleRadiusFixed)
        : kFixedZero;
    const Fixed targetRadius = targetGeometry
        ? Fixed::max(kFixedZero,
              targetGeometry->boundingCircleRadiusFixed)
        : kFixedZero;
    return Fixed::max(
        kFixedZero, centerDistance - sourceRadius - targetRadius);
}

[[nodiscard]] bool pitchMatches(const game::WeaponTemplate& weapon,
                                const LogicFixedVec3& source,
                                const LogicFixedVec3& target) noexcept {
    const Fixed dz = target.z - source.z;
    if (Fixed::abs(dz) < kFixedPitchAlwaysAcceptDeltaZ) return true;
    const Fixed dx = target.x - source.x;
    const Fixed dy = target.y - source.y;
    const Fixed horizontal = Fixed::sqrt(dx * dx + dy * dy);
    const Fixed pitch = math::fixed_atan2(
        dz, Fixed::max(horizontal, kFixedHorizontalEpsilon));
    return pitch >= weapon.fixed.minTargetPitchRadians &&
           pitch <= weapon.fixed.maxTargetPitchRadians;
}

[[nodiscard]] Fixed resolvedAttackRange(
    const game::WeaponTemplate& weapon,
    const game::WeaponBonus& bonus) noexcept {
    return Fixed::max(kFixedZero,
        bonus.scale(weapon.fixed.attackRange,
                    game::WeaponBonusField::Range) -
            kFixedRationalizedRangeUndersize);
}

[[nodiscard]] bool isWithinRange(const game::WeaponTemplate& weapon,
                                 const game::WeaponBonus& bonus,
                                 Fixed distance) noexcept {
    const Fixed maximum = Fixed::max(kFixedZero,
        bonus.scale(weapon.fixed.attackRange,
                    game::WeaponBonusField::Range) -
        kFixedRationalizedRangeUndersize);
    const Fixed minimum = Fixed::max(kFixedZero,
        weapon.fixed.minimumAttackRange -
        kFixedRationalizedRangeUndersize);
    // The refcode compares squared 2D boundary distance with strict `<` for
    // the minimum and `>` for the maximum, so exact authored boundaries are
    // legal.  `combatDistance` already implements BoundaryAndBoundary_2D.
    return distance >= minimum && distance <= maximum;
}

[[nodiscard]] bool hasClearableGarrisonOccupant(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity targetEntity) noexcept {
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry,
                                                        targetEntity);
    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, targetEntity);
    const ObjectIdentityComponent* targetIdentity =
        ecs::try_get<ObjectIdentityComponent>(registry, targetEntity);
    if (!runtime || !runtime->plan || !contents || !targetIdentity ||
        !targetIdentity->id) {
        return false;
    }
    for (const ObjectContainedObjectRecord& record : contents->objects) {
        const std::optional<ecs::entity> occupant =
            lifecycle.entityFromId(record.object);
        if (!occupant) continue;
        const ObjectHealthComponent* occupantHealth =
            ecs::try_get<ObjectHealthComponent>(registry, *occupant);
        if (occupantHealth && occupantHealth->effectivelyDead) continue;
        const ObjectContainedByComponent* edge =
            ecs::try_get<ObjectContainedByComponent>(registry, *occupant);
        if (!edge || edge->container != targetIdentity->id ||
            edge->containmentRuleIndex >= runtime->plan->rules.size()) {
            continue;
        }
        const ObjectContainmentRule& rule =
            runtime->plan->rules[edge->containmentRuleIndex];
        if (rule.kind == ObjectContainmentKind::Garrison &&
            !rule.immuneToClearBuildingAttacks) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr bool isSubdualWeaponDamage(
    game::DamageType type) noexcept {
    return type == game::DamageType::SUBDUAL_MISSILE ||
           type == game::DamageType::SUBDUAL_VEHICLE ||
           type == game::DamageType::SUBDUAL_BUILDING ||
           type == game::DamageType::SUBDUAL_UNRESISTABLE;
}

[[nodiscard]] Fixed estimatedDamage(
    const game::WeaponTemplate& weapon, const game::WeaponBonus& bonus,
    const ObjectArmorComponent* armor, const ObjectHealthComponent* health,
    const ObjectKindOfComponent* kinds, const ObjectStatusComponent* status,
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    std::optional<ecs::entity> targetEntity) noexcept {
    // ActiveBody::estimateDamage is also the WeaponSet selection policy.  A
    // specialised damage type returning zero must not win merely because its
    // raw authored PrimaryDamage is large.
    if (isSubdualWeaponDamage(weapon.damageType) &&
        (!health || health->subdualDamageCapFixed <= math::q32_32{})) {
        return kFixedZero;
    }
    if (weapon.damageType == game::DamageType::KILL_GARRISONED) {
        return targetEntity &&
                       hasClearableGarrisonOccupant(registry, lifecycle,
                                                    *targetEntity)
                   ? kFixedOne
                   : kFixedZero;
    }
    if (weapon.damageType == game::DamageType::SNIPER &&
        containsKind(kinds, game::ObjectKindOf::Structure) && status &&
        status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::UnderConstruction))) {
        return kFixedZero;
    }
    // WeaponTemplate::estimateWeaponTemplateDamage admits a SURRENDER weapon,
    // or any weapon authored with AllowAttackGarrisonedBldgs, against an
    // occupied clearable garrison with a small nonzero estimate. Without it
    // the structure's armor coefficient (SURRENDER 0%, SNIPER 0%) zeroes the
    // estimate and the attack is rejected before it can clear the occupants.
    if ((weapon.damageType == game::DamageType::SURRENDER ||
         weapon.allowAttackGarrisonedBldgs) &&
        targetEntity &&
        hasClearableGarrisonOccupant(registry, lifecycle, *targetEntity)) {
        return kFixedOne;
    }
    Fixed amount = bonus.scale(
        weapon.fixed.primaryDamage, game::WeaponBonusField::Damage);
    const size_t index = static_cast<size_t>(weapon.damageType);
    if (armor && index < armor->damageMultipliersFixed.size()) {
        amount *= Fixed::max(
            kFixedZero, armor->damageMultipliersFixed[index]);
    }
    return amount;
}

[[nodiscard]] bool sameActiveAttack(const ObjectWeaponComponent& weapons,
                                    const ObjectOrderIntent& order) noexcept {
    return weapons.target == order.targetObject && weapons.activeOrderTick == order.issuedTick &&
           weapons.activeOrderSequence == order.sourceSequence &&
           weapons.activeSourceScriptId == order.sourceScriptId;
}

void resetAttackLock(ObjectWeaponComponent& weapons,
                     bool releaseTemporary) noexcept {
    if (releaseTemporary) {
        static_cast<void>(releaseWeaponLock(
            weapons, ObjectWeaponLockType::Temporary));
    }
    for (ObjectWeaponSetRuntime& set : weapons.sets) {
        for (ObjectWeaponSlotRuntime& slot : set.slots) {
            slot.preAttackArmed = false;
            slot.preAttackCompleteTick = 0;
        }
    }
}

[[nodiscard]] game::WeaponCommandSource toWeaponCommandSource(ObjectOrderSource source) noexcept {
    switch (source) {
    case ObjectOrderSource::Player: return game::WeaponCommandSource::Player;
    case ObjectOrderSource::Script: return game::WeaponCommandSource::Script;
    case ObjectOrderSource::System: return game::WeaponCommandSource::AI;
    }
    return game::WeaponCommandSource::AI;
}

[[nodiscard]] bool samePreAttackOrder(const ObjectWeaponSlotRuntime& slot,
                                      const ObjectOrderIntent& order) noexcept {
    return slot.preAttackOrderTick == order.issuedTick &&
           slot.preAttackOrderSequence == order.sourceSequence &&
           slot.preAttackSourceScriptId == order.sourceScriptId &&
           slot.preAttackTarget == order.targetObject;
}

[[nodiscard]] bool requiresPreAttack(const ObjectWeaponSlotRuntime& slot,
                                     const game::WeaponTemplate& definition,
                                     const ObjectOrderIntent& order) noexcept {
    if (definition.preAttackDelayMilliseconds == 0 || slot.preAttackArmed) return false;
    switch (definition.preAttackType) {
    case game::WeaponPreAttackType::PerShot: return true;
    case game::WeaponPreAttackType::PerAttack: return !samePreAttackOrder(slot, order);
    case game::WeaponPreAttackType::PerClip:
        return slot.preAttackClipGeneration != slot.clipGeneration;
    }
    return false;
}

[[nodiscard]] bool beginPreAttack(ObjectWeaponSlotRuntime& slot,
                                  const game::WeaponTemplate& definition,
                                  const game::WeaponBonus& bonus,
                                  const ObjectOrderIntent& order,
                                  uint32_t framesPerSecond,
                                  uint64_t tick) noexcept {
    const uint64_t delay = multiplyFramesByPreAttack(
        millisecondsToFrames(definition.preAttackDelayMilliseconds, framesPerSecond), bonus);
    slot.preAttackCompleteTick = saturatingTickAdd(tick, delay);
    slot.preAttackArmed = delay != 0;
    slot.preAttackOrderTick = order.issuedTick;
    slot.preAttackOrderSequence = order.sourceSequence;
    slot.preAttackSourceScriptId = order.sourceScriptId;
    slot.preAttackTarget = order.targetObject;
    slot.preAttackClipGeneration = slot.clipGeneration;
    return slot.preAttackArmed;
}

void consumeAttackOrder(ObjectOrderQueueComponent* queue) {
    // Borrowed passenger intents deliberately pass nullptr: a passenger's
    // weapon/target failure must never mutate either participant's queue.
    if (!queue || queue->orders.empty()) return;
    queue->orders.erase(queue->orders.begin());
    ++queue->revision;
}

} // namespace engine::object_combat_detail
