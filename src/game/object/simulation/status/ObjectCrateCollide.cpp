#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"

#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/TerrainLogic.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] bool containsKind(const ObjectKindOfComponent* kinds,
                                game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

using container::asciiEqualIgnoreCase;

struct Candidate final {
    ObjectId id = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

[[nodiscard]] bool kindsMatch(const ObjectKindOfComponent* kinds,
                              const game::ObjectCrateCollideRule& rule) noexcept {
    return kinds && game::objectKindsMatch(
        kinds->mask, rule.requiredKinds, rule.forbiddenKinds);
}

[[nodiscard]] bool noCollisions(const ecs::registry& registry,
                                ecs::entity entity) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    return status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::NoCollisions));
}

[[nodiscard]] bool hasStatus(const ecs::registry& registry, ecs::entity entity,
                             game::ObjectStatusFlag flag) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    return status && status->hasAny(game::objectStatusBit(flag));
}

[[nodiscard]] bool excludedFromPhysicalContact(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    if (const ObjectContainedByComponent* contained =
            ecs::try_get<ObjectContainedByComponent>(registry, entity);
        contained && contained->enclosing) {
        return true;
    }
    const ObjectMapStatusComponent* map =
        ecs::try_get<ObjectMapStatusComponent>(registry, entity);
    return map && map->offMap;
}

struct VerticalInterval final {
    math::q32_32 minimum{};
    math::q32_32 maximum{};
};

[[nodiscard]] VerticalInterval verticalInterval(
    const LogicFixedVec3& position,
    const ObjectGeometryComponent* geometry) noexcept {
    if (!geometry) {
        return {position.z, position.z + math::q32_32{int32_t{2}}};
    }
    if (geometry->shape == ObjectGeometryShape::Sphere) {
        const math::q32_32 radius = math::q32_32::max(
            math::q32_32{}, geometry->boundingSphereRadiusFixed);
        return {position.z - radius, position.z + radius};
    }
    return {position.z, position.z + math::q32_32::max(
        math::q32_32{}, geometry->heightFixed)};
}

[[nodiscard]] bool overlaps(const ecs::registry& registry,
                            ecs::entity leftEntity,
                            const TransformComponent& leftTransform,
                            const ObjectGeometryComponent* leftGeometry,
                            ecs::entity rightEntity,
                            const TransformComponent& rightTransform,
                            const ObjectGeometryComponent* rightGeometry) noexcept {
    const LogicFixedVec3 leftPosition = readAuthoritativeObjectPosition(
        registry, leftEntity, leftTransform);
    const LogicFixedVec3 rightPosition = readAuthoritativeObjectPosition(
        registry, rightEntity, rightTransform);
    const math::q32_32 leftRadius = leftGeometry
        ? math::q32_32::max(math::q32_32{},
                            leftGeometry->boundingCircleRadiusFixed)
        : math::q32_32{int32_t{1}};
    const math::q32_32 rightRadius = rightGeometry
        ? math::q32_32::max(math::q32_32{},
                            rightGeometry->boundingCircleRadiusFixed)
        : math::q32_32{int32_t{1}};
    const math::q32_32 dx = leftPosition.x - rightPosition.x;
    const math::q32_32 dy = leftPosition.y - rightPosition.y;
    const math::q32_32 radius = leftRadius + rightRadius;
    if (dx * dx + dy * dy > radius * radius) return false;
    const VerticalInterval left = verticalInterval(leftPosition, leftGeometry);
    const VerticalInterval right = verticalInterval(rightPosition, rightGeometry);
    return left.maximum >= right.minimum && right.maximum >= left.minimum;
}

[[nodiscard]] bool hasAiUpdate(const ThingTemplateComponent* type) noexcept {
    return type && type->archetype && type->archetype->hasAiUpdate;
}

[[nodiscard]] uint32_t veterancyLevelsToGain(
    const ecs::registry& registry, ecs::entity crateEntity,
    const game::ObjectCrateCollideRule& rule) noexcept {
    if (!rule.veterancyAddsOwnerVeterancy) return 1u;
    const ObjectVeterancyComponent* veterancy =
        ecs::try_get<ObjectVeterancyComponent>(registry, crateEntity);
    return veterancy ? static_cast<uint32_t>(veterancy->level) : 0u;
}

[[nodiscard]] bool canGainVeterancyLevels(
    const ecs::registry& registry, ecs::entity entity,
    uint32_t levelsToGain) noexcept {
    if (levelsToGain == 0) return false;
    const ObjectExperienceComponent* experience =
        ecs::try_get<ObjectExperienceComponent>(registry, entity);
    const ObjectVeterancyComponent* veterancy =
        ecs::try_get<ObjectVeterancyComponent>(registry, entity);
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!experience || !experience->trainable || !veterancy ||
        !type || !type->archetype) {
        return false;
    }
    const uint32_t current = static_cast<uint32_t>(veterancy->level);
    const uint32_t heroic =
        static_cast<uint32_t>(game::ObjectVeterancyLevel::Heroic);
    return current < heroic && levelsToGain <= heroic - current;
}

[[nodiscard]] bool hasSelectableWeaponSetCondition(
    const ObjectCombatProfileComponent* combat,
    game::WeaponSetCondition condition) noexcept {
    if (!combat || !combat->profile) return false;
    const game::WeaponSetConditionMask bit =
        game::weaponSetConditionBit(condition);
    const game::WeaponSetProfile* selected =
        combat->profile->findBestWeaponSet(combat->weaponConditions | bit);
    return selected && (selected->conditions & bit) != 0 &&
           (combat->weaponConditions & bit) == 0;
}

[[nodiscard]] bool hasTransportPassengers(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectContainmentComponent* containment =
        ecs::try_get<ObjectContainmentComponent>(registry, entity);
    return containment && !containment->objects.empty();
}

[[nodiscard]] bool isSabotageKind(
    game::ObjectCrateCollideKind kind) noexcept {
    switch (kind) {
    case game::ObjectCrateCollideKind::SabotageCommandCenter:
    case game::ObjectCrateCollideKind::SabotageFakeBuilding:
    case game::ObjectCrateCollideKind::SabotageInternetCenter:
    case game::ObjectCrateCollideKind::SabotageMilitaryFactory:
    case game::ObjectCrateCollideKind::SabotagePowerPlant:
    case game::ObjectCrateCollideKind::SabotageSuperweapon:
    case game::ObjectCrateCollideKind::SabotageSupplyCenter:
    case game::ObjectCrateCollideKind::SabotageSupplyDropzone:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool requiresIntentionalTarget(
    game::ObjectCrateCollideKind kind) noexcept {
    return kind == game::ObjectCrateCollideKind::Veterancy ||
        kind == game::ObjectCrateCollideKind::ConvertToCarBomb ||
        kind == game::ObjectCrateCollideKind::ConvertToHijackedVehicle ||
        isSabotageKind(kind);
}

[[nodiscard]] bool hasIntentionalContactTarget(
    const ecs::registry& registry, ecs::entity source,
    ObjectId target, game::ObjectCrateCollideKind ruleKind) noexcept {
    const ObjectOrderQueueComponent* orders =
        ecs::try_get<ObjectOrderQueueComponent>(registry, source);
    if (!orders || orders->orders.empty()) return false;
    const ObjectOrderIntent& intent = orders->orders.front();
    if (intent.targetObject != target) return false;
    if (intent.systemPurpose ==
        ObjectOrderSystemPurpose::IntentionalContact) {
        if (intent.systemPurposeInstance == 0u) return false;
        const auto kind = static_cast<ObjectIntentionalContactKind>(
            intent.systemPurposeInstance - 1u);
        switch (kind) {
        case ObjectIntentionalContactKind::HijackVehicle:
            return ruleKind == game::ObjectCrateCollideKind::
                ConvertToHijackedVehicle;
        case ObjectIntentionalContactKind::ConvertToCarBomb:
            return ruleKind == game::ObjectCrateCollideKind::
                ConvertToCarBomb;
        case ObjectIntentionalContactKind::SabotageBuilding:
            return isSabotageKind(ruleKind);
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool sabotageTargetMatches(
    const ObjectKindOfComponent* kinds,
    game::ObjectCrateCollideKind kind) noexcept {
    switch (kind) {
    case game::ObjectCrateCollideKind::SabotageCommandCenter:
        return containsKind(kinds, game::ObjectKindOf::CommandCenter);
    case game::ObjectCrateCollideKind::SabotageFakeBuilding:
        return containsKind(kinds, game::ObjectKindOf::FsFake);
    case game::ObjectCrateCollideKind::SabotageInternetCenter:
        return containsKind(kinds, game::ObjectKindOf::FsInternetCenter);
    case game::ObjectCrateCollideKind::SabotageMilitaryFactory:
        return !containsKind(kinds, game::ObjectKindOf::AircraftCarrier) &&
            (containsKind(kinds, game::ObjectKindOf::FsBarracks) ||
             containsKind(kinds, game::ObjectKindOf::FsWarfactory) ||
             containsKind(kinds, game::ObjectKindOf::FsAirfield));
    case game::ObjectCrateCollideKind::SabotagePowerPlant:
        return containsKind(kinds, game::ObjectKindOf::FsPower);
    case game::ObjectCrateCollideKind::SabotageSuperweapon:
        return containsKind(kinds, game::ObjectKindOf::FsSuperweapon) ||
               containsKind(kinds, game::ObjectKindOf::FsStrategyCenter);
    case game::ObjectCrateCollideKind::SabotageSupplyCenter:
        return containsKind(kinds, game::ObjectKindOf::FsSupplyCenter);
    case game::ObjectCrateCollideKind::SabotageSupplyDropzone:
        return containsKind(kinds, game::ObjectKindOf::FsSupplyDropzone);
    default:
        return false;
    }
}

[[nodiscard]] int32_t firstMoneyBoost(
    const game::ObjectCrateCollideRule& rule,
    const PlayerRegistry& players, PlayerId player,
    const UpgradeCatalog* catalog) noexcept {
    for (const game::ObjectCrateUpgradeBoost& boost : rule.upgradedBoosts) {
        if (catalog &&
            players.hasUpgradeComplete(player, boost.upgrade, *catalog)) {
            return boost.amount;
        }
    }
    return 0;
}

[[nodiscard]] bool validPicker(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain, const PlayerRegistry& players,
    ecs::entity crateEntity, const Candidate& picker,
    const game::ObjectCrateCollideRule& rule,
    uint32_t veterancyGainLevels = 0,
    math::q32_32 significantAboveTerrainHeight = {},
    bool requireIntentionalTarget = true) noexcept {
    if (!picker.id || lifecycle.isPendingDestroy(picker.id) ||
        noCollisions(registry, crateEntity) || noCollisions(registry, picker.entity) ||
        excludedFromPhysicalContact(registry, crateEntity) ||
        excludedFromPhysicalContact(registry, picker.entity)) return false;

    // Pilot, terrorist, hijacker and saboteur Collide modules are contact
    // completions for an explicit AI/order target, not generic proximity
    // pickups. RefCode checks getGoalObject()==other before every effect; the
    // stable order head is the modern equivalent and prevents a nearby unit
    // from being converted or sabotaged merely because paths overlap.
    if (requireIntentionalTarget && requiresIntentionalTarget(rule.kind) &&
        !hasIntentionalContactTarget(
            registry, crateEntity, picker.id, rule.kind)) {
        return false;
    }

    const OwnerComponent* crateOwner = ecs::try_get<OwnerComponent>(registry, crateEntity);
    const OwnerComponent* pickerOwner = ecs::try_get<OwnerComponent>(registry, picker.entity);
    const PlayerState* player = pickerOwner ? players.get(pickerOwner->player) : nullptr;
    if (!pickerOwner || !player || player->isNeutral()) return false;

    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, picker.entity);
    const bool structurePickup = rule.buildingPickup &&
        containsKind(kinds, game::ObjectKindOf::Structure);
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, picker.entity);
    if (!structurePickup && !hasAiUpdate(type)) return false;
    if (!kindsMatch(kinds, rule)) return false;
    if (rule.kind == game::ObjectCrateCollideKind::Salvage &&
        !containsKind(kinds, game::ObjectKindOf::Salvager)) return false;
    if (rule.kind == game::ObjectCrateCollideKind::ConvertToCarBomb) {
        // RefCode permits any valid controlling player here. Unlike hijack or
        // sabotage, car-bomb conversion has no enemy-relationship gate; a
        // neutral scripted source is therefore a legal legacy-map producer.
        if (!crateOwner || !players.get(crateOwner->player)) {
            return false;
        }
        if (containsKind(kinds, game::ObjectKindOf::Aircraft) ||
            containsKind(kinds, game::ObjectKindOf::Boat) ||
            hasStatus(registry, picker.entity,
                      game::ObjectStatusFlag::IsCarBomb)) {
            return false;
        }
        if (!hasSelectableWeaponSetCondition(
                ecs::try_get<ObjectCombatProfileComponent>(
                    registry, picker.entity),
                game::WeaponSetCondition::CarBomb)) {
            return false;
        }
    }
    if (rule.kind ==
        game::ObjectCrateCollideKind::ConvertToHijackedVehicle) {
        if (!crateOwner || !players.get(crateOwner->player) ||
            players.get(crateOwner->player)->isNeutral()) {
            return false;
        }
        if (relationshipBetweenObjects(
                registry, players, crateEntity, picker.entity) !=
            PlayerRelationship::Enemies) {
            return false;
        }
        if (containsKind(kinds, game::ObjectKindOf::ImmuneToCapture) ||
            containsKind(kinds, game::ObjectKindOf::Aircraft) ||
            containsKind(kinds, game::ObjectKindOf::Boat) ||
            containsKind(kinds, game::ObjectKindOf::Drone) ||
            hasStatus(registry, picker.entity,
                      game::ObjectStatusFlag::Hijacked)) {
            return false;
        }
        if (containsKind(kinds, game::ObjectKindOf::Transport) &&
            hasTransportPassengers(registry, picker.entity)) {
            return false;
        }
    }
    if (rule.kind == game::ObjectCrateCollideKind::Veterancy) {
        if (!canGainVeterancyLevels(registry, picker.entity,
                                    veterancyGainLevels)) {
            return false;
        }
        const TransformComponent* pickerTransform =
            ecs::try_get<TransformComponent>(registry, picker.entity);
        if (!pickerTransform) return false;
        const LogicFixedVec3 pickerPosition = readAuthoritativeObjectPosition(
            registry, picker.entity, *pickerTransform);
        const math::q32_32 ground = math::q32_32::from_raw(
            terrain.groundHeightRaw(pickerPosition.x.raw(),
                                    pickerPosition.y.raw()));
        if (pickerPosition.z > ground + significantAboveTerrainHeight) {
            return false;
        }
        if (rule.veterancyIsPilot) {
            const OwnerComponent* crateOwner =
                ecs::try_get<OwnerComponent>(registry, crateEntity);
            if (!crateOwner || crateOwner->player != pickerOwner->player) {
                return false;
            }
            const ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(registry,
                                                         picker.entity);
            if (locomotion &&
                (locomotion->surfaces & game::locomotorSurfaceBit(
                    game::LocomotorSurface::Air)) != 0) {
                // Object::isUsingAirborneLocomotor tests the selected
                // locomotor's legal AIR surface, not current altitude. This
                // keeps a newly built/parked aircraft from accepting a pilot.
                return false;
            }
        }
    }
    if (isSabotageKind(rule.kind)) {
        if (!crateOwner || !players.get(crateOwner->player) ||
            players.get(crateOwner->player)->isNeutral()) {
            return false;
        }
        if (!sabotageTargetMatches(kinds, rule.kind) ||
            relationshipBetweenObjects(
                registry, players, crateEntity, picker.entity) !=
                PlayerRelationship::Enemies) {
            return false;
        }
        if (hasStatus(registry, picker.entity,
                      game::ObjectStatusFlag::UnderConstruction) ||
            hasStatus(registry, picker.entity,
                      game::ObjectStatusFlag::Sold)) {
            return false;
        }
    }

    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, picker.entity);
    if (health && health->effectivelyDead) return false;

    const TransformComponent* crateTransform =
        ecs::try_get<TransformComponent>(registry, crateEntity);
    if (!crateTransform) return false;
    const LogicFixedVec3 cratePosition = readAuthoritativeObjectPosition(
        registry, crateEntity, *crateTransform);
    if (!structurePickup && cratePosition.z.raw() >
            terrain.groundHeightRaw(
                cratePosition.x.raw(), cratePosition.y.raw())) return false;

    if (rule.forbidOwnerPlayer && crateOwner &&
        crateOwner->player == pickerOwner->player) return false;
    if (rule.humanOnly && player->controller != PlayerControllerKind::Human) return false;
    if (!rule.pickupScience.empty() &&
        !players.hasScience(pickerOwner->player, rule.pickupScience)) return false;
    if (containsKind(kinds, game::ObjectKindOf::Parachute)) return false;
    return true;
}

} // namespace

bool canObjectPerformIntentionalCrateContact(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const PlayerRegistry& players, ObjectId source, ObjectId target,
    ObjectIntentionalContactKind kind) noexcept {
    if (!source || !target || source == target ||
        lifecycle.isPendingDestroy(source) ||
        lifecycle.isPendingDestroy(target)) {
        return false;
    }
    const std::optional<ecs::entity> sourceEntity =
        lifecycle.entityFromId(source);
    const std::optional<ecs::entity> targetEntity =
        lifecycle.entityFromId(target);
    const ObjectCrateCollideComponent* component = sourceEntity
        ? ecs::try_get<ObjectCrateCollideComponent>(registry, *sourceEntity)
        : nullptr;
    if (!sourceEntity || !targetEntity || !component || !component->plan) {
        return false;
    }
    const auto matchesKind = [kind](
                                 game::ObjectCrateCollideKind ruleKind) {
        switch (kind) {
        case ObjectIntentionalContactKind::HijackVehicle:
            return ruleKind == game::ObjectCrateCollideKind::
                ConvertToHijackedVehicle;
        case ObjectIntentionalContactKind::ConvertToCarBomb:
            return ruleKind == game::ObjectCrateCollideKind::ConvertToCarBomb;
        case ObjectIntentionalContactKind::SabotageBuilding:
            return isSabotageKind(ruleKind);
        }
        return false;
    };
    const Candidate candidate{.id = target, .entity = *targetEntity};
    return std::any_of(
        component->plan->rules.begin(), component->plan->rules.end(),
        [&](const game::ObjectCrateCollideRule& rule) {
            return matchesKind(rule.kind) &&
                validPicker(registry, lifecycle, terrain, players,
                            *sourceEntity, candidate, rule, 0u, {}, false);
        });
}

bool hasEarlierConsumingCrateCollisionPriority(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const PlayerRegistry& players, ObjectId source, ecs::entity sourceEntity,
    ObjectId target, ecs::entity targetEntity,
    uint32_t laterAuthoredOrder,
    math::q32_32 significantAboveTerrainHeight) noexcept {
    if (!source || !target || lifecycle.isPendingDestroy(source) ||
        lifecycle.isPendingDestroy(target)) {
        return false;
    }
    const ObjectCrateCollideComponent* component =
        ecs::try_get<ObjectCrateCollideComponent>(registry, sourceEntity);
    const TransformComponent* sourceTransform =
        ecs::try_get<TransformComponent>(registry, sourceEntity);
    const TransformComponent* targetTransform =
        ecs::try_get<TransformComponent>(registry, targetEntity);
    if (!component || !component->plan || !sourceTransform ||
        !targetTransform || !overlaps(
            registry, sourceEntity, *sourceTransform,
            ecs::try_get<ObjectGeometryComponent>(registry, sourceEntity),
            targetEntity, *targetTransform,
            ecs::try_get<ObjectGeometryComponent>(registry, targetEntity))) {
        return false;
    }
    const Candidate candidate{.id = target, .entity = targetEntity};
    for (const game::ObjectCrateCollideRule& rule : component->plan->rules) {
        if (rule.authoredOrder >= laterAuthoredOrder) break;
        // Ejectable Hijacker deliberately returns false after the owner
        // transaction and is protected separately by HijackerUpdate's target
        // veto. Sabotage targets are structures, not active crushers.
        if (rule.kind != game::ObjectCrateCollideKind::ConvertToCarBomb &&
            rule.kind != game::ObjectCrateCollideKind::Veterancy)
            continue;
        const uint32_t veterancyGainLevels =
            rule.kind == game::ObjectCrateCollideKind::Veterancy
                ? veterancyLevelsToGain(registry, sourceEntity, rule)
                : 0u;
        if (validPicker(registry, lifecycle, terrain, players,
                        sourceEntity, candidate, rule, veterancyGainLevels,
                        significantAboveTerrainHeight)) {
            return true;
        }
    }
    return false;
}

bool canObjectAIAutonomouslyPickUpCrate(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const PlayerRegistry& players, const ObjectSimulationRules& rules,
    ObjectId picker, ObjectId crate) noexcept {
    if (!picker || !crate || picker == crate ||
        lifecycle.isPendingDestroy(picker) ||
        lifecycle.isPendingDestroy(crate)) {
        return false;
    }
    const std::optional<ecs::entity> pickerEntity =
        lifecycle.entityFromId(picker);
    const std::optional<ecs::entity> crateEntity =
        lifecycle.entityFromId(crate);
    if (!pickerEntity || !crateEntity) return false;

    const ObjectCrateCollideComponent* component =
        ecs::try_get<ObjectCrateCollideComponent>(registry, *crateEntity);
    if (!component || !component->plan) return false;

    const math::q32_32 framesPerSecond{
        static_cast<int32_t>(std::max<uint32_t>(1u,
            rules.logicFramesPerSecond))};
    const math::q32_32 significantAboveTerrainHeight =
        math::q32_32{int32_t{9}} *
        math::q32_32::abs(rules.gravityUnitsPerSecondSq) /
        (framesPerSecond * framesPerSecond);
    const Candidate candidate{.id = picker, .entity = *pickerEntity};
    for (const game::ObjectCrateCollideRule& rule : component->plan->rules) {
        if (requiresIntentionalTarget(rule.kind)) continue;
        if (validPicker(registry, lifecycle, terrain, players, *crateEntity,
                        candidate, rule,
                        veterancyLevelsToGain(registry, *crateEntity, rule),
                        significantAboveTerrainHeight)) {
            return true;
        }
    }
    return false;
}

void ObjectCrateCollideSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype || !type->archetype->crateCollidePlan) return;
    ObjectCrateCollideComponent value{.plan = type->archetype->crateCollidePlan};
    if (ObjectCrateCollideComponent* existing =
            ecs::try_get<ObjectCrateCollideComponent>(registry, entity)) {
        *existing = std::move(value);
    } else {
        ecs::emplace<ObjectCrateCollideComponent>(registry, entity, std::move(value));
    }
}

void ObjectCrateCollideSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectSpatialIndex& spatialIndex,
    const game::terrain::TerrainLogic& terrain,
    const PlayerRegistry& players, const GameContentSnapshot& content,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick,
    container::Vector<ObjectCratePickupCommand>& outCommands) const {
    const math::q32_32 framesPerSecond{
        static_cast<int32_t>(std::max<uint32_t>(1u,
            rules.logicFramesPerSecond))};
    const math::q32_32 significantAboveTerrainHeight =
        math::q32_32{int32_t{9}} *
        math::q32_32::abs(rules.gravityUnitsPerSecondSq) /
        (framesPerSecond * framesPerSecond);
    container::Vector<Candidate> crates;
    const auto crateView = ecs::view<const ObjectIdentityComponent,
                                     const ObjectCrateCollideComponent,
                                     const TransformComponent>(registry);
    for (const ecs::entity entity : crateView) {
        const ObjectIdentityComponent& identity =
            crateView.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || lifecycle.isPendingDestroy(identity.id)) continue;
        crates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(crates.begin(), crates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.id < right.id;
              });

    container::Vector<ObjectId> nearby;
    for (const Candidate& crate : crates) {
        const ObjectCrateCollideComponent& component =
            ecs::get<ObjectCrateCollideComponent>(registry, crate.entity);
        if (!component.plan) continue;
        const TransformComponent& crateTransform =
            ecs::get<TransformComponent>(registry, crate.entity);
        const ObjectGeometryComponent* crateGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, crate.entity);
        const math::q32_32 crateRadius = crateGeometry
            ? math::q32_32::max(math::q32_32{},
                                crateGeometry->boundingCircleRadiusFixed)
            : math::q32_32{int32_t{1}};
        const LogicFixedVec3 cratePosition = readAuthoritativeObjectPosition(
            registry, crate.entity, crateTransform);
        spatialIndex.queryRadiusFixed(
            cratePosition, crateRadius, nearby);
        for (const game::ObjectCrateCollideRule& rule : component.plan->rules) {
            if (rule.kind == game::ObjectCrateCollideKind::Unit &&
                !content.findObjectArchetype(rule.unitName)) {
                continue;
            }
            const uint32_t veterancyGainLevels =
                rule.kind == game::ObjectCrateCollideKind::Veterancy
                    ? veterancyLevelsToGain(registry, crate.entity, rule)
                    : 0u;
            if (rule.kind == game::ObjectCrateCollideKind::Veterancy &&
                veterancyGainLevels == 0) {
                continue;
            }
            for (const ObjectId pickerId : nearby) {
                if (pickerId == crate.id ||
                    lifecycle.isPendingDestroy(pickerId)) continue;
                const std::optional<ecs::entity> pickerEntity =
                    lifecycle.entityFromId(pickerId);
                if (!pickerEntity) continue;
                const Candidate picker{.id = pickerId,
                                       .entity = *pickerEntity};
                const TransformComponent& pickerTransform =
                    ecs::get<TransformComponent>(registry, picker.entity);
                if (!overlaps(registry, crate.entity, crateTransform,
                              crateGeometry, picker.entity, pickerTransform,
                              ecs::try_get<ObjectGeometryComponent>(registry, picker.entity))) {
                    continue;
                }
                if (!validPicker(registry, lifecycle, terrain, players,
                                 crate.entity, picker, rule,
                                 veterancyGainLevels,
                                 significantAboveTerrainHeight)) continue;

                const OwnerComponent* owner =
                    ecs::try_get<OwnerComponent>(registry, picker.entity);
                if (!owner) continue;
                const OwnerComponent* crateOwner =
                    ecs::try_get<OwnerComponent>(registry, crate.entity);
                const LogicFixedVec3 pickerPosition =
                    readAuthoritativeObjectPosition(
                        registry, picker.entity, pickerTransform);
                const math::q32_32 pickerYaw =
                    readAuthoritativeObjectYaw(
                        registry, picker.entity, pickerTransform);
                const bool sourcePlayerCommand =
                    rule.kind == game::ObjectCrateCollideKind::ConvertToCarBomb ||
                    rule.kind == game::ObjectCrateCollideKind::ConvertToHijackedVehicle ||
                    isSabotageKind(rule.kind);
                const PlayerId commandPlayer =
                    sourcePlayerCommand && crateOwner
                        ? crateOwner->player : owner->player;
                bool preserveSourceOnSuccess = false;
                if (rule.kind ==
                    game::ObjectCrateCollideKind::ConvertToHijackedVehicle) {
                    const ObjectDeathReactionComponent* reaction =
                        ecs::try_get<ObjectDeathReactionComponent>(
                            registry, picker.entity);
                    if (reaction && reaction->plan) {
                        preserveSourceOnSuccess = std::any_of(
                            reaction->plan->rules.begin(),
                            reaction->plan->rules.end(),
                            [](const game::ObjectDeathReactionRule& deathRule) {
                                return deathRule.kind ==
                                    game::ObjectDeathReactionKind::EjectPilot;
                            });
                    }
                }
                ObjectCratePickupCommand command{
                    .kind = rule.kind,
                    .crate = crate.id,
                    .picker = picker.id,
                    .player = commandPlayer,
                    .victimPlayer = owner->player,
                    .authoredOrder = rule.authoredOrder,
                    .moneyAmount = rule.moneyProvided +
                        firstMoneyBoost(rule, players, owner->player,
                                       content.upgradeCatalog()),
                    .unitCount = rule.unitCount,
                    .unitName = rule.unitName,
                    .veterancyLevelsToGain = veterancyGainLevels,
                    .veterancyEffectRange = rule.veterancyEffectRange,
                    .veterancyIsPilot = rule.veterancyIsPilot,
                    .salvageWeaponChance = rule.salvageWeaponChance,
                    .salvageLevelChance = rule.salvageLevelChance,
                    .salvageMoneyChance = rule.salvageMoneyChance,
                    .salvageMinimumMoney = rule.salvageMinimumMoney,
                    .salvageMaximumMoney = rule.salvageMaximumMoney,
                    .sabotageDurationMilliseconds =
                        rule.sabotageDurationMilliseconds,
                    .stealCashAmount = rule.stealCashAmount,
                    .cratePosition = cratePosition,
                    .position = pickerPosition,
                    .rotationRadians = pickerYaw,
                    .executeFx = rule.executeFx,
                    .executeAnimation = rule.executeAnimation,
                    .convertFxList = rule.convertFxList,
                    .executeAnimationTimeSeconds =
                        rule.executeAnimationTimeSeconds,
                    .executeAnimationZRisePerSecond =
                        rule.executeAnimationZRisePerSecond,
                    .executeAnimationFades = rule.executeAnimationFades,
                    .allowMultiPickup = rule.allowMultiPickup,
                    .preserveSourceOnSuccess = preserveSourceOnSuccess,
                    .confirmedTick = confirmedTick,
                };
                outCommands.push_back(std::move(command));
            }
        }
    }

    // Intentional-contact Move heads are retained by Movement until this
    // post-movement narrow phase observes them. Consume the head after a real
    // contact command, or after locomotion reached the target but no authored
    // collide rule remained valid.
    for (const Candidate& crate : crates) {
        ObjectOrderQueueComponent* queuePtr =
            ecs::try_get<ObjectOrderQueueComponent>(registry, crate.entity);
        if (!queuePtr) continue;
        ObjectOrderQueueComponent& queue = *queuePtr;
        if (queue.orders.empty() ||
            queue.orders.front().systemPurpose !=
                ObjectOrderSystemPurpose::IntentionalContact) {
            continue;
        }
        const ObjectId source = crate.id;
        const bool produced = std::any_of(
            outCommands.begin(), outCommands.end(),
            [source](const ObjectCratePickupCommand& command) {
                return command.crate == source;
            });
        const ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(registry, crate.entity);
        const bool reachedWithoutEffect = locomotion &&
            !locomotion->hasActiveMove &&
            locomotion->state == ObjectLocomotionState::Idle;
        if (!produced && !reachedWithoutEffect) continue;
        queue.orders.erase(queue.orders.begin());
        ++queue.revision;
    }
}

} // namespace engine
