#pragma once

#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/weapon/WeaponTemplate.h"

#include <algorithm>
#include <cstddef>

namespace engine {

// Value-only equivalent of WeaponSet::getVictimAntiMask. Keep target category
// selection shared by confirmed Combat and command/cursor admission.
[[nodiscard]] inline bool weaponTargetMatchesAntiMask(
    const game::WeaponTemplate& weapon,
    const ObjectKindOfComponent* kinds,
    const ObjectAirborneComponent* airborne,
    const ObjectStatusComponent* status) noexcept {
    const auto has = [kinds](game::ObjectKindOf kind) noexcept {
        return kinds && game::objectHasKind(kinds->mask, kind);
    };
    if (has(game::ObjectKindOf::SmallMissile)) {
        return (weapon.antiMask & game::weaponAntiBit(
            game::WeaponAntiTarget::SmallMissile)) != 0;
    }
    if (has(game::ObjectKindOf::BallisticMissile)) {
        return (weapon.antiMask & game::weaponAntiBit(
            game::WeaponAntiTarget::BallisticMissile)) != 0;
    }
    if (has(game::ObjectKindOf::Projectile)) {
        return (weapon.antiMask & game::weaponAntiBit(
            game::WeaponAntiTarget::Projectile)) != 0;
    }
    if (has(game::ObjectKindOf::Mine) ||
        has(game::ObjectKindOf::Demotrap)) {
        return (weapon.antiMask &
            (game::weaponAntiBit(game::WeaponAntiTarget::Mine) |
             game::weaponAntiBit(game::WeaponAntiTarget::Ground))) != 0;
    }
    if (has(game::ObjectKindOf::Parachute)) {
        return (weapon.antiMask & game::weaponAntiBit(
            game::WeaponAntiTarget::Parachute)) != 0;
    }
    const bool explicitlyAirborne =
        (airborne && airborne->isAirborne) ||
        (status && status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::AirborneTarget)));
    if (explicitlyAirborne && has(game::ObjectKindOf::Vehicle)) {
        return (weapon.antiMask & game::weaponAntiBit(
            game::WeaponAntiTarget::AirborneVehicle)) != 0;
    }
    if (explicitlyAirborne && has(game::ObjectKindOf::Infantry)) {
        return (weapon.antiMask & game::weaponAntiBit(
            game::WeaponAntiTarget::AirborneInfantry)) != 0;
    }
    return (weapon.antiMask & game::weaponAntiBit(
        game::WeaponAntiTarget::Ground)) != 0;
}

// Shared read-only admission for player cursors, commands and autonomous AI.
// Keeping this beside the anti-mask classifier prevents perception from
// selecting a target that confirmed Combat can never service.
[[nodiscard]] inline bool weaponRuntimeCanAttackTarget(
    const ecs::registry& registry, ecs::entity entity,
    ecs::entity target, const GameContentSnapshot& content) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    if (status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::NoAttack))) {
        return false;
    }
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    if (!weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return false;
    }
    const ObjectKindOfComponent* targetKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, target);
    const ObjectAirborneComponent* targetAirborne =
        ecs::try_get<ObjectAirborneComponent>(registry, target);
    const ObjectStatusComponent* targetStatus =
        ecs::try_get<ObjectStatusComponent>(registry, target);
    const ObjectWeaponSetRuntime& activeSet =
        weapons->sets[*weapons->activeWeaponSetIndex];
    const auto slotCanAttack = [&](const ObjectWeaponSlotRuntime& slot) {
        const game::WeaponTemplate* weapon = content.findWeapon(slot.content);
        return weapon && weaponTargetMatchesAntiMask(
            *weapon, targetKinds, targetAirborne, targetStatus);
    };
    if (weapons->lockedSlot) {
        const size_t slot = static_cast<size_t>(*weapons->lockedSlot);
        return slot < activeSet.slots.size() &&
            slotCanAttack(activeSet.slots[slot]);
    }
    return std::any_of(
        activeSet.slots.begin(), activeSet.slots.end(), slotCanAttack);
}

} // namespace engine
