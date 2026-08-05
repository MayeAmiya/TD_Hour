#include "game/object/simulation/combat/ObjectCombatTargetability.h"

#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/plan/containment/ObjectContainmentPlanTypes.h"
#include "game/object/simulation/combat/ObjectCombatDetail.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/status/ObjectDisabled.h"

#include <algorithm>

namespace engine {

namespace {

[[nodiscard]] bool currentWeaponSetHasEnabledWeapon(
    const ObjectWeaponComponent& weapons,
    const ObjectWeaponSetRuntime& set,
    const GameContentSnapshot& content) noexcept {
    bool anyWeapon = false;
    bool anyEnabled = false;
    for (size_t slotIndex = 0; slotIndex < set.slots.size(); ++slotIndex) {
        if (!content.findWeapon(set.slots[slotIndex].content)) continue;
        anyWeapon = true;
        const uint8_t slotBit = static_cast<uint8_t>(
            uint8_t{1} << slotIndex);
        bool turreted = false;
        for (const ObjectTurretRuntime& turret : weapons.turrets) {
            if ((turret.controlledWeaponSlots & slotBit) == 0) continue;
            turreted = true;
            if (turret.enabled) anyEnabled = true;
        }
        if (!turreted) anyEnabled = true;
    }
    return anyWeapon && anyEnabled;
}

} // namespace

bool objectOwnWeaponsAbleToAttack(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ecs::entity source,
    uint64_t confirmedTick) noexcept {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, source);
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, source);
    if (!type || !type->archetype || !type->archetype->hasAiUpdate ||
        !weapons || !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return false;
    }

    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, source);
    const game::ObjectStatusMask unableStatus =
        game::objectStatusBit(game::ObjectStatusFlag::NoAttack) |
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
        game::objectStatusBit(game::ObjectStatusFlag::Sold);
    if ((status && status->hasAny(unableStatus)) ||
        isObjectDisabledBy(
            registry, source, ObjectDisabledReason::Subdued,
            confirmedTick) ||
        !objectPassengerAllowedToFire(
            registry, lifecycle, source, confirmedTick)) {
        return false;
    }

    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, source);
    const bool portableOrSpawnWeapon = kinds &&
        (game::objectHasKind(
             kinds->mask, game::ObjectKindOf::PortableStructure) ||
         game::objectHasKind(
             kinds->mask, game::ObjectKindOf::SpawnsAreTheWeapons));
    if (portableOrSpawnWeapon) {
        const ObjectDisabledMask blocked =
            objectDisabledBit(ObjectDisabledReason::Hacked) |
            objectDisabledBit(ObjectDisabledReason::Emp);
        if ((objectDisabledMask(registry, source, confirmedTick) & blocked) != 0)
            return false;
        if (game::objectHasKind(kinds->mask, game::ObjectKindOf::Infantry)) {
            const ObjectSpawnSlaveComponent* slave =
                ecs::try_get<ObjectSpawnSlaveComponent>(registry, source);
            if (slave && !slave->slaved.empty() &&
                slave->slaved.front().master) {
                const std::optional<ecs::entity> master =
                    lifecycle.entityFromId(slave->slaved.front().master);
                if (master && isObjectDisabledBy(
                        registry, *master, ObjectDisabledReason::Subdued,
                        confirmedTick)) {
                    return false;
                }
            }
        }
    }

    const ObjectWeaponSetRuntime& set =
        weapons->sets[*weapons->activeWeaponSetIndex];
    const bool kindCanAttack = kinds && game::objectHasKind(
        kinds->mask, game::ObjectKindOf::CanAttack);
    if (kindCanAttack) {
        return std::any_of(
            set.slots.begin(), set.slots.end(),
            [&content](const ObjectWeaponSlotRuntime& slot) {
                return content.findWeapon(slot.content) != nullptr;
            });
    }
    return currentWeaponSetHasEnabledWeapon(*weapons, set, content);
}

ObjectCombatTargetability queryObjectCombatTargetability(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ObjectId source,
    ObjectId target, uint64_t confirmedTick) noexcept {
    using namespace object_combat_detail;
    ObjectCombatTargetability result;
    if (!source || !target || source == target) return result;
    const std::optional<ecs::entity> sourceEntity =
        lifecycle.entityFromId(source);
    const std::optional<ecs::entity> targetEntity =
        lifecycle.entityFromId(target);
    if (!sourceEntity || !targetEntity) return result;

    const ObjectHealthComponent* sourceHealth =
        ecs::try_get<ObjectHealthComponent>(registry, *sourceEntity);
    const ObjectHealthComponent* targetHealth =
        ecs::try_get<ObjectHealthComponent>(registry, *targetEntity);
    if ((sourceHealth && sourceHealth->effectivelyDead) ||
        (targetHealth && targetHealth->effectivelyDead)) {
        return result;
    }
    if (!objectOwnWeaponsAbleToAttack(
            registry, lifecycle, content, *sourceEntity, confirmedTick)) {
        return result;
    }
    const ObjectCombatProfileComponent* combat =
        ecs::try_get<ObjectCombatProfileComponent>(registry, *sourceEntity);
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, *sourceEntity);
    if (!combat || !combat->profile || !weapons) return result;
    const container::Span<const game::WeaponSetProfile> authoredSets =
        combat->profile->weaponSets();
    const game::WeaponSetProfile* authoredSet =
        combat->profile->findBestWeaponSet(combat->weaponConditions);
    if (!authoredSet || authoredSets.empty()) return result;
    const size_t setIndex = static_cast<size_t>(
        authoredSet - authoredSets.data());
    if (setIndex >= weapons->sets.size()) return result;
    const ObjectWeaponSetRuntime& runtimeSet = weapons->sets[setIndex];

    const TransformComponent* sourceTransform =
        ecs::try_get<TransformComponent>(registry, *sourceEntity);
    const TransformComponent* targetTransform =
        ecs::try_get<TransformComponent>(registry, *targetEntity);
    if (!sourceTransform || !targetTransform) return result;
    LogicFixedVec3 sourcePosition = readAuthoritativeObjectPosition(
        registry, *sourceEntity, *sourceTransform);
    const LogicFixedVec3 targetPosition = readAuthoritativeObjectPosition(
        registry, *targetEntity, *targetTransform);
    const ObjectGeometryComponent* sourceGeometry =
        ecs::try_get<ObjectGeometryComponent>(registry, *sourceEntity);
    const ObjectContainedByComponent* contained =
        ecs::try_get<ObjectContainedByComponent>(registry, *sourceEntity);
    std::optional<ecs::entity> host;
    const ObjectContainmentRule* containmentRule = nullptr;
    if (contained && contained->container) {
        host = lifecycle.entityFromId(contained->container);
        const TransformComponent* hostTransform = host
            ? ecs::try_get<TransformComponent>(registry, *host) : nullptr;
        const ObjectContainmentRuntimeComponent* hostRuntime = host
            ? ecs::try_get<ObjectContainmentRuntimeComponent>(registry, *host)
            : nullptr;
        if (hostRuntime && hostRuntime->plan &&
            contained->containmentRuleIndex < hostRuntime->plan->rules.size()) {
            containmentRule =
                &hostRuntime->plan->rules[contained->containmentRuleIndex];
        }
        if (host && hostTransform && containmentRule &&
            (contained->enclosing || containmentRule->kind ==
                ObjectContainmentKind::Garrison)) {
            sourcePosition = readAuthoritativeObjectPosition(
                registry, *host, *hostTransform);
            sourceGeometry = nullptr;
        }
    }

    const ObjectGeometryComponent* targetGeometry =
        ecs::try_get<ObjectGeometryComponent>(registry, *targetEntity);
    const ObjectKindOfComponent* targetKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, *targetEntity);
    const ObjectAirborneComponent* targetAirborne =
        ecs::try_get<ObjectAirborneComponent>(registry, *targetEntity);
    const ObjectArmorComponent* targetArmor =
        ecs::try_get<ObjectArmorComponent>(registry, *targetEntity);
    const ObjectStatusComponent* targetStatus =
        ecs::try_get<ObjectStatusComponent>(registry, *targetEntity);
    const Fixed distance = combatDistance(
        sourcePosition, sourceGeometry, targetPosition, targetGeometry);

    game::WeaponBonusConditionMask bonusConditions{};
    if (const ObjectWeaponBonusComponent* bonus =
            ecs::try_get<ObjectWeaponBonusComponent>(registry, *sourceEntity)) {
        bonusConditions = bonus->conditions;
    }
    if (host && containmentRule &&
        containmentRule->weaponBonusPassedToPassengers) {
        if (const ObjectWeaponBonusComponent* bonus =
                ecs::try_get<ObjectWeaponBonusComponent>(registry, *host)) {
            bonusConditions |= bonus->conditions;
        }
    }

    for (size_t index = 0; index < game::kWeaponSlotCount; ++index) {
        const game::WeaponSlotProfile& authoredSlot =
            authoredSet->slots[index];
        const ObjectWeaponSlotRuntime& runtimeSlot = runtimeSet.slots[index];
        const game::WeaponTemplate* definition =
            content.findWeapon(runtimeSlot.content);
        if (!authoredSlot.hasWeapon() || !definition) continue;
        const game::WeaponBonus bonus =
            content.resolveWeaponBonus(*definition, bonusConditions);
        result.withinAnyWeaponRange = result.withinAnyWeaponRange ||
            isWithinRange(*definition, bonus, distance);
        if (!authoredSlot.allowsAutoChoose(game::WeaponCommandSource::AI) ||
            !targetMatchesAntiMask(
                *definition, targetKinds, targetAirborne, targetStatus) ||
            !pitchMatches(*definition, sourcePosition, targetPosition)) {
            continue;
        }
        const bool empty = hasFiniteEmptyClip(runtimeSlot, *definition);
        if (empty && definition->reloadType != game::WeaponReloadType::Auto)
            continue;
        const Fixed damage = estimatedDamage(
            *definition, bonus, targetArmor, targetHealth, targetKinds,
            targetStatus, registry, lifecycle, targetEntity);
        if (damage > Fixed{} ||
            definition->damageType == game::DamageType::UNRESISTABLE) {
            result.canAttack = true;
        }
    }
    return result;
}

} // namespace engine
