#include "game/object/simulation/combat/ObjectTactical.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "core/math/fixed/q32_32_trig.h"

#include "game/base/SimulationRandom.h"
#include "game/data/base/SpecialPowerCatalog.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/contracts/ObjectToppleMath.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/combat/ObjectCombatDetail.h"
#include "game/object/simulation/combat/ObjectCombatTargetability.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectCrateCollide.h"
#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/structure/ObjectBridge.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "game/terrain/TerrainLogic.h"
#include "game/navigation/runtime/NavigationSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>

#include "game/object/simulation/combat/ObjectTacticalDetail.h"

namespace engine {
using namespace object_tactical_detail;

namespace {

[[nodiscard]] bool currentWeaponReadyForAssistance(
    const ObjectWeaponComponent& weapons,
    const GameContentSnapshot& content,
    uint64_t confirmedTick) noexcept {
    if (!weapons.activeWeaponSetIndex || !weapons.currentSlot ||
        *weapons.activeWeaponSetIndex >= weapons.sets.size()) {
        return false;
    }
    const ObjectWeaponSetRuntime& set =
        weapons.sets[*weapons.activeWeaponSetIndex];
    const size_t slotIndex = static_cast<size_t>(*weapons.currentSlot);
    if (slotIndex >= set.slots.size()) return false;
    const ObjectWeaponSlotRuntime& slot = set.slots[slotIndex];
    const game::WeaponTemplate* weapon = content.findWeapon(slot.content);
    if (!weapon || slot.preAttackCompleteTick > confirmedTick ||
        slot.nextReadyTick > confirmedTick ||
        (set.sharedReloadCompleteTick != 0 &&
         set.sharedReloadCompleteTick > confirmedTick) ||
        object_combat_detail::isReloading(slot, confirmedTick) ||
        object_combat_detail::hasFiniteEmptyClip(slot, *weapon)) {
        return false;
    }
    return true;
}

[[nodiscard]] bool deployStyleCurrentWeaponIsInRange(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ecs::entity sourceEntity,
    const ObjectOrderIntent& order) noexcept {
    using namespace object_combat_detail;
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, sourceEntity);
    const TransformComponent* sourceTransform =
        ecs::try_get<TransformComponent>(registry, sourceEntity);
    if (!weapons || !sourceTransform || !weapons->activeWeaponSetIndex ||
        !weapons->currentSlot ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return false;
    }
    const size_t slot = static_cast<size_t>(*weapons->currentSlot);
    const ObjectWeaponSetRuntime& set =
        weapons->sets[*weapons->activeWeaponSetIndex];
    if (slot >= set.slots.size()) return false;
    const game::WeaponTemplate* weapon =
        content.findWeapon(set.slots[slot].content);
    if (!weapon) return false;

    const LogicFixedVec3 sourcePosition = readAuthoritativeObjectPosition(
        registry, sourceEntity, *sourceTransform);
    const ObjectGeometryComponent* sourceGeometry =
        ecs::try_get<ObjectGeometryComponent>(registry, sourceEntity);
    LogicFixedVec3 targetPosition{order.targetX, order.targetY, order.targetZ};
    const ObjectGeometryComponent* targetGeometry = nullptr;
    if (order.targetObject) {
        const std::optional<ecs::entity> targetEntity =
            lifecycle.entityFromId(order.targetObject);
        const TransformComponent* targetTransform = targetEntity
            ? ecs::try_get<TransformComponent>(registry, *targetEntity)
            : nullptr;
        if (!targetEntity || !targetTransform) return false;
        targetPosition = readAuthoritativeObjectPosition(
            registry, *targetEntity, *targetTransform);
        targetGeometry = ecs::try_get<ObjectGeometryComponent>(
            registry, *targetEntity);
    } else if (!order.hasTargetPosition) {
        return false;
    }

    game::WeaponBonusConditionMask bonusConditions{};
    if (const ObjectWeaponBonusComponent* bonus =
            ecs::try_get<ObjectWeaponBonusComponent>(registry, sourceEntity)) {
        bonusConditions = bonus->conditions;
    }
    const game::WeaponBonus bonus =
        content.resolveWeaponBonus(*weapon, bonusConditions);
    return isWithinRange(
        *weapon, bonus,
        combatDistance(sourcePosition, sourceGeometry,
                       targetPosition, targetGeometry));
}

[[nodiscard]] constexpr bool specialAbilityCreatesPreparationObject(
    game::SpecialPowerType type) noexcept {
    switch (type) {
    case game::SpecialPowerType::MissileDefenderLaserGuidedMissiles:
    case game::SpecialPowerType::HackerDisableBuilding:
    case game::SpecialPowerType::BlackLotusCaptureBuilding:
    case game::SpecialPowerType::BlackLotusDisableVehicleHack:
    case game::SpecialPowerType::BlackLotusStealCashHack:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr bool specialAbilityEndPreparationDestroysObjects(
    game::SpecialPowerType type) noexcept {
    switch (type) {
    case game::SpecialPowerType::MissileDefenderLaserGuidedMissiles:
    case game::SpecialPowerType::HackerDisableBuilding:
    case game::SpecialPowerType::BlackLotusDisableVehicleHack:
    case game::SpecialPowerType::BlackLotusCaptureBuilding:
    case game::SpecialPowerType::BlackLotusStealCashHack:
    case game::SpecialPowerType::InfantryCaptureBuilding:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr bool specialAbilityUsesStickyBomb(
    game::SpecialPowerType type) noexcept {
    switch (type) {
    case game::SpecialPowerType::TankHunterTntAttack:
    case game::SpecialPowerType::TimedCharges:
    case game::SpecialPowerType::BoobyTrap:
    case game::SpecialPowerType::RemoteCharges:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool commandSetContainsButton(
    const ecs::registry& registry, const GameContentSnapshot& content,
    ecs::entity entity, container::StringView buttonName) noexcept {
    const container::StringView commandSetName =
        effectiveObjectCommandSetName(registry, entity);
    const game::CommandSetTemplate* commandSet =
        content.findCommandSet(commandSetName);
    return commandSet && std::any_of(
        commandSet->commands.begin(), commandSet->commands.end(),
        [buttonName](const container::String& candidate) {
            return equalInsensitive(candidate, buttonName);
        });
}

[[nodiscard]] bool hiddenFromCommandButtonHunter(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ecs::entity source,
    ecs::entity entity) noexcept {
    return objectHiddenFromObserverForAcquisition(
        registry, lifecycle, players, source, entity);
}

[[nodiscard]] bool targetAppearsToContainFriendlies(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ecs::entity source,
    ecs::entity target) noexcept {
    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, target);
    if (!contents) return false;
    for (const ObjectContainedObjectRecord& record : contents->objects) {
        const std::optional<ecs::entity> occupant =
            lifecycle.entityFromId(record.object);
        if (occupant && relationshipBetweenObjects(
                registry, players, source, *occupant) !=
                PlayerRelationship::Enemies) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool hasNonStealthGarrisonOccupant(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity target) noexcept {
    const ObjectContainmentRuntimeComponent* runtime =
        ecs::try_get<ObjectContainmentRuntimeComponent>(registry, target);
    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, target);
    if (!runtime || !runtime->plan || !contents ||
        (runtime->plan->kindMask & objectContainmentKindBit(
             ObjectContainmentKind::Garrison)) == 0) {
        return false;
    }
    for (const ObjectContainedObjectRecord& record : contents->objects) {
        const std::optional<ecs::entity> occupant =
            lifecycle.entityFromId(record.object);
        const ObjectStatusComponent* status = occupant
            ? ecs::try_get<ObjectStatusComponent>(registry, *occupant)
            : nullptr;
        if (occupant && (!status || !status->hasAny(
                statusBit(game::ObjectStatusFlag::Stealthed)))) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool targetNearFriendlyMine(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity source, ecs::entity target,
    math::q32_32 range) noexcept {
    if (range <= math::q32_32{}) return false;
    const OwnerComponent* sourceOwner =
        ecs::try_get<OwnerComponent>(registry, source);
    const TransformComponent* targetTransform =
        ecs::try_get<TransformComponent>(registry, target);
    if (!sourceOwner || !targetTransform) return false;
    const LogicFixedVec3 targetPosition = readAuthoritativeObjectPosition(
        registry, target, *targetTransform);
    const ObjectGeometryComponent* targetGeometry =
        ecs::try_get<ObjectGeometryComponent>(registry, target);
    const auto mines = ecs::view<const ObjectIdentityComponent,
                                 const OwnerComponent,
                                 const TransformComponent,
                                 const ObjectKindOfComponent>(registry);
    for (const ecs::entity mine : mines) {
        const ObjectIdentityComponent& identity = mines.template get<
            const ObjectIdentityComponent>(mine);
        const OwnerComponent& owner = mines.template get<
            const OwnerComponent>(mine);
        const ObjectKindOfComponent& kinds = mines.template get<
            const ObjectKindOfComponent>(mine);
        if (owner.player != sourceOwner->player ||
            !game::objectHasKind(kinds.mask, game::ObjectKindOf::Mine) ||
            !alive(registry, lifecycle, identity.id, mine) ||
            !sameMapStatus(registry, target, mine)) {
            continue;
        }
        const TransformComponent& mineTransform = mines.template get<
            const TransformComponent>(mine);
        const LogicFixedVec3 minePosition = readAuthoritativeObjectPosition(
            registry, mine, mineTransform);
        const ObjectGeometryComponent* mineGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, mine);
        if (object_combat_detail::combatDistance(
                targetPosition, targetGeometry, minePosition,
                mineGeometry) <= range) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool specialPowerHuntIsReady(
    const ecs::registry& registry, ecs::entity source,
    SpecialPowerContentId specialPower, uint64_t confirmedTick) noexcept {
    const ObjectSpecialPowerComponent* powers =
        ecs::try_get<ObjectSpecialPowerComponent>(registry, source);
    if (!powers) return false;
    const auto power = std::find_if(
        powers->instances.begin(), powers->instances.end(),
        [specialPower](const ObjectSpecialPowerRuntime& runtime) {
            return runtime.content == specialPower;
        });
    if (power == powers->instances.end() || power->pausedCount != 0 ||
        confirmedTick < power->readyTick) {
        return false;
    }
    const ObjectTacticalComponent* tactical =
        ecs::try_get<ObjectTacticalComponent>(registry, source);
    if (!tactical) return false;
    const auto ability = std::find_if(
        tactical->specialAbilities.begin(), tactical->specialAbilities.end(),
        [specialPower](const ObjectSpecialAbilityRuntime& runtime) {
            return runtime.specialPower == specialPower;
        });
    return ability != tactical->specialAbilities.end() && !ability->active;
}

[[nodiscard]] bool specialPowerHuntTargetAllowed(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ecs::entity source, ecs::entity target,
    game::SpecialPowerType type, uint64_t confirmedTick) noexcept {
    const PlayerRelationship relationship =
        relationshipBetweenObjects(registry, players, source, target);
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, target);
    const auto hasKind = [kinds](game::ObjectKindOf kind) noexcept {
        return kinds && game::objectHasKind(kinds->mask, kind);
    };
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, target);
    const bool underConstruction = status && status->hasAny(
        statusBit(game::ObjectStatusFlag::UnderConstruction));
    const bool sold = status && status->hasAny(
        statusBit(game::ObjectStatusFlag::Sold));
    const bool containsApparentFriendlies =
        targetAppearsToContainFriendlies(
            registry, lifecycle, players, source, target);

    switch (type) {
    case game::SpecialPowerType::BattleshipBombardment:
        return relationship != PlayerRelationship::Allies;
    case game::SpecialPowerType::TankHunterTntAttack:
        return relationship == PlayerRelationship::Enemies &&
            (hasKind(game::ObjectKindOf::Structure) ||
             (hasKind(game::ObjectKindOf::Vehicle) &&
              !hasKind(game::ObjectKindOf::Aircraft)));
    case game::SpecialPowerType::BoobyTrap:
        return hasKind(game::ObjectKindOf::Structure) &&
            (relationship == PlayerRelationship::Allies ||
             relationship == PlayerRelationship::Neutral);
    case game::SpecialPowerType::MissileDefenderLaserGuidedMissiles:
        return relationship == PlayerRelationship::Enemies &&
            hasKind(game::ObjectKindOf::Vehicle);
    case game::SpecialPowerType::HackerDisableBuilding:
        return relationship == PlayerRelationship::Enemies &&
            hasKind(game::ObjectKindOf::Structure) &&
            !hasKind(game::ObjectKindOf::RebuildHole) &&
            !underConstruction && !containsApparentFriendlies &&
            (hasKind(game::ObjectKindOf::Capturable) ||
             (hasKind(game::ObjectKindOf::FsTechnology) &&
              !hasKind(game::ObjectKindOf::ImmuneToCapture)));
    case game::SpecialPowerType::InfantryCaptureBuilding:
    case game::SpecialPowerType::BlackLotusCaptureBuilding: {
        if (!hasKind(game::ObjectKindOf::Structure) ||
            hasKind(game::ObjectKindOf::ImmuneToCapture) ||
            underConstruction || sold ||
            relationship == PlayerRelationship::Allies ||
            hasNonStealthGarrisonOccupant(
                registry, lifecycle, target) ||
            containsApparentFriendlies) {
            return false;
        }
        const OwnerComponent* sourceOwner =
            ecs::try_get<OwnerComponent>(registry, source);
        const OwnerComponent* targetOwner =
            ecs::try_get<OwnerComponent>(registry, target);
        return (!sourceOwner || !targetOwner ||
                sourceOwner->player != targetOwner->player) &&
            (relationship == PlayerRelationship::Enemies ||
             hasKind(game::ObjectKindOf::Capturable));
    }
    case game::SpecialPowerType::BlackLotusDisableVehicleHack: {
        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(registry, target);
        return relationship == PlayerRelationship::Enemies &&
            hasKind(game::ObjectKindOf::Vehicle) &&
            !hasKind(game::ObjectKindOf::Aircraft) &&
            !(airborne && airborne->isAirborne) &&
            objectDisabledMask(registry, target, confirmedTick) == 0 &&
            !containsApparentFriendlies;
    }
    case game::SpecialPowerType::BlackLotusStealCashHack:
    case game::SpecialPowerType::CashHack:
        return relationship == PlayerRelationship::Enemies &&
            hasKind(game::ObjectKindOf::Structure) &&
            hasKind(game::ObjectKindOf::CashGenerator) &&
            hasKind(game::ObjectKindOf::Capturable) &&
            !hasKind(game::ObjectKindOf::RebuildHole) &&
            !underConstruction && !containsApparentFriendlies;
    case game::SpecialPowerType::DisguiseAsVehicle:
        return relationship == PlayerRelationship::Enemies &&
            hasKind(game::ObjectKindOf::Vehicle) &&
            !hasKind(game::ObjectKindOf::Aircraft) &&
            !hasKind(game::ObjectKindOf::Boat) &&
            !hasKind(game::ObjectKindOf::CliffJumper) &&
            !ecs::try_get<ObjectRailroadComponent>(registry, target);
    case game::SpecialPowerType::Defector:
        return relationship == PlayerRelationship::Enemies &&
            !hasKind(game::ObjectKindOf::Structure);
    case game::SpecialPowerType::RemoteCharges:
    case game::SpecialPowerType::TimedCharges:
    case game::SpecialPowerType::HelixNapalmBomb:
        return relationship == PlayerRelationship::Enemies &&
            !hasKind(game::ObjectKindOf::Bridge) &&
            !hasKind(game::ObjectKindOf::BridgeTower) &&
            (hasKind(game::ObjectKindOf::Structure) ||
             hasKind(game::ObjectKindOf::Vehicle));

    // These powers are location/no-target abilities. CommandButtonHunt must
    // never manufacture an object target for them.
    case game::SpecialPowerType::Invalid:
    case game::SpecialPowerType::DaisyCutter:
    case game::SpecialPowerType::ParadropAmerica:
    case game::SpecialPowerType::CarpetBomb:
    case game::SpecialPowerType::ClusterMines:
    case game::SpecialPowerType::EmpPulse:
    case game::SpecialPowerType::NapalmStrike:
    case game::SpecialPowerType::NeutronMissile:
    case game::SpecialPowerType::SpySatellite:
    case game::SpecialPowerType::TerrorCell:
    case game::SpecialPowerType::Ambush:
    case game::SpecialPowerType::BlackMarketNuke:
    case game::SpecialPowerType::AnthraxBomb:
    case game::SpecialPowerType::ScudStorm:
    case game::SpecialPowerType::DemoralizeObsolete:
    case game::SpecialPowerType::CrateDrop:
    case game::SpecialPowerType::A10ThunderboltStrike:
    case game::SpecialPowerType::DetonateDirtyNuke:
    case game::SpecialPowerType::ArtilleryBarrage:
    case game::SpecialPowerType::RadarVanScan:
    case game::SpecialPowerType::SpyDrone:
    case game::SpecialPowerType::RepairVehicles:
    case game::SpecialPowerType::ParticleUplinkCannon:
    case game::SpecialPowerType::CashBounty:
    case game::SpecialPowerType::ChangeBattlePlans:
    case game::SpecialPowerType::CiaIntelligence:
    case game::SpecialPowerType::CleanupArea:
    case game::SpecialPowerType::LaunchBaikonurRocket:
    case game::SpecialPowerType::SpectreGunship:
    case game::SpecialPowerType::GpsScrambler:
    case game::SpecialPowerType::Frenzy:
    case game::SpecialPowerType::SneakAttack:
    case game::SpecialPowerType::ChinaCarpetBomb:
    case game::SpecialPowerType::EarlyChinaCarpetBomb:
    case game::SpecialPowerType::LeafletDrop:
    case game::SpecialPowerType::EarlyLeafletDrop:
    case game::SpecialPowerType::EarlyFrenzy:
    case game::SpecialPowerType::CommunicationsDownload:
    case game::SpecialPowerType::EarlyRepairVehicles:
    case game::SpecialPowerType::TankParadrop:
    case game::SpecialPowerType::SupwParticleUplinkCannon:
    case game::SpecialPowerType::AirfDaisyCutter:
    case game::SpecialPowerType::NukeClusterMines:
    case game::SpecialPowerType::NukeNeutronMissile:
    case game::SpecialPowerType::AirfA10ThunderboltStrike:
    case game::SpecialPowerType::AirfSpectreGunship:
    case game::SpecialPowerType::InfaParadropAmerica:
    case game::SpecialPowerType::SlthGpsScrambler:
    case game::SpecialPowerType::AirfCarpetBomb:
    case game::SpecialPowerType::SuprCruiseMissile:
    case game::SpecialPowerType::LazrParticleUplinkCannon:
    case game::SpecialPowerType::SupwNeutronMissile:
    case game::SpecialPowerType::Count:
        return false;
    }
    return false;
}

} // namespace

void ObjectTacticalSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerRegistry& players, const GameContentSnapshot& content,
    const game::terrain::TerrainLogic& terrain,
    const ObjectAITargetPriorityQuery& targetPriority,
    const game::terrain::MapVisibilitySnapshot* visibility,
    const navigation::NavigationSystem* navigation,
    const ObjectSimulationRules& rules, SimulationRandom* random,
    uint64_t confirmedTick, int32_t rankLevelLimit,
    uint64_t& nextGameplaySubmissionOrdinal,
    container::Vector<ObjectDamageRequest>& damageRequests,
    container::Vector<ObjectDefectionRequest>& defectionRequests,
    container::Vector<ObjectSpecialAbilityEffectRequest>& effectRequests,
    container::Vector<ObjectSpecialAbilityFacingRequest>& facingRequests) const {
    consumeToppleRequests(
        registry, lifecycle, confirmedTick,
        nextGameplaySubmissionOrdinal);
    const auto claimGameplayOrdinal = [&]() noexcept {
        const uint64_t result = nextGameplaySubmissionOrdinal++;
        if (nextGameplaySubmissionOrdinal == 0)
            ++nextGameplaySubmissionOrdinal;
        return result;
    };
    struct Candidate { ObjectId id; ecs::entity entity; };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent, ObjectTacticalComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectId id = view.template get<const ObjectIdentityComponent>(entity).id;
        if (alive(registry, lifecycle, id, entity)) candidates.push_back({id, entity});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.id < b.id; });
    container::Vector<uint64_t> seeThroughObstacles;
    bool seeThroughObstaclesLoaded = false;

    // Defection is committed after this system yields its transaction batch.
    // Without a per-tick claim, two capture abilities that complete in the
    // same deterministic update both still see the old owner and each emit a
    // completion.  RefCode's first SpecialAbilityUpdate which reaches the
    // target wins the object; later abilities stop once that transition is
    // pending.  Candidate order is stable ObjectId order above, so this local
    // claim has no dependence on registry iteration or presentation timing.
    container::Vector<ObjectId> captureClaimedTargets;

    // PropagandaTowerBehavior: refresh the stable membership roster only on
    // the authored scan clock, then consume that roster every confirmed tick.
    // This matches RefCode's doScan()/m_insideList split and retains the
    // original sole-benefactor healing non-stacking rule.
    container::HashMap<ObjectId, game::WeaponBonusConditionMask> aura;
    for (const Candidate& source : candidates) {
        auto& component = ecs::get<ObjectTacticalComponent>(registry, source.entity);
        const OwnerComponent* sourceOwner = ecs::try_get<OwnerComponent>(registry, source.entity);
        const TransformComponent* sourceTransform = ecs::try_get<TransformComponent>(registry, source.entity);
        if (!component.plan || !sourceOwner || !sourceOwner->player || !sourceTransform) continue;
        const LogicFixedVec3 sourcePosition = readAuthoritativeObjectPosition(
            registry, source.entity, *sourceTransform);
        for (size_t index = 0; index < component.plan->propagandaTowers.size(); ++index) {
            const auto& rule = component.plan->propagandaTowers[index];
            auto& runtime = component.propagandaTowers[index];
            const ObjectDisabledMask disabled = objectDisabledMask(registry, source.entity, confirmedTick);
            if ((disabled & ~objectDisabledBit(ObjectDisabledReason::Held)) != 0) {
                runtime.members.clear();
                continue;
            }
            const ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(registry, source.entity);
            if (status && status->hasAny(statusBit(game::ObjectStatusFlag::UnderConstruction) |
                                         statusBit(game::ObjectStatusFlag::Sold))) {
                runtime.members.clear();
                continue;
            }
            if (sourceOwner->player == NEUTRAL_PLAYER_ID) {
                runtime.members.clear();
                continue;
            }
            if (const auto* contained = ecs::try_get<ObjectContainedByComponent>(registry, source.entity)) {
                const bool portable = hasKind(
                    registry, source.entity,
                    game::ObjectKindOf::PortableStructure);
                const bool vehicle = hasKind(
                    registry, source.entity, game::ObjectKindOf::Vehicle);
                const auto host = lifecycle.entityFromId(contained->container);
                if ((vehicle && !portable) ||
                    (host && ecs::try_get<ObjectContainedByComponent>(registry, *host))) {
                    runtime.members.clear();
                    continue;
                }
            }
            const ObjectUpgradeInventoryComponent* sourceUpgrades =
                ecs::try_get<ObjectUpgradeInventoryComponent>(
                    registry, source.entity);
            const bool objectUpgraded = sourceUpgrades &&
                rule.upgradeRequiredId &&
                upgradeMaskTest(sourceUpgrades->completed,
                                rule.upgradeRequiredId);
            const bool upgraded = rule.upgradeRequiredId &&
                (players.hasUpgradeComplete(
                      sourceOwner->player, rule.upgradeRequiredId) ||
                 objectUpgraded);
            if (confirmedTick >= runtime.nextScanTick) {
                runtime.nextScanTick = saturatingAdd(confirmedTick,
                    std::max<uint64_t>(1, millisecondsToTicks(rule.scanDelayMilliseconds,
                                                               rules.logicFramesPerSecond)));
                const container::String& pulseFx = upgraded
                    ? rule.upgradedPulseFx : rule.pulseFx;
                if (!pulseFx.empty()) {
                    m_tacticalPresentationEvents.push_back({
                        .kind = ObjectTacticalPresentationEventKind::
                            PropagandaPulse,
                        .source = source.id,
                        .fxList = pulseFx,
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                }
                container::Vector<ObjectId> refreshed;
                const math::q32_32 radiusSquared = rule.radius * rule.radius;
                const auto targets = ecs::view<const ObjectIdentityComponent,
                                               const OwnerComponent,
                                               const TransformComponent>(registry);
                refreshed.reserve(targets.size_hint());
                for (const ecs::entity target : targets) {
                    const ObjectId targetId = targets.template get<
                        const ObjectIdentityComponent>(target).id;
                    if (!alive(registry, lifecycle, targetId, target) ||
                        (!rule.affectsSelf && targetId == source.id) ||
                        !sameMapStatus(registry, source.entity, target) ||
                        hasKind(registry, target,
                                game::ObjectKindOf::Structure) ||
                        relationshipBetweenObjects(registry, players,
                                                   source.entity, target) !=
                            PlayerRelationship::Allies) {
                        continue;
                    }
                    const TransformComponent& targetTransform = targets.template get<
                        const TransformComponent>(target);
                    const LogicFixedVec3 targetPosition =
                        readAuthoritativeObjectPosition(
                            registry, target, targetTransform);
                    const math::q32_32 dx{
                        targetPosition.x - sourcePosition.x};
                    const math::q32_32 dy{
                        targetPosition.y - sourcePosition.y};
                    if (dx * dx + dy * dy <= radiusSquared)
                        refreshed.push_back(targetId);
                }
                std::sort(refreshed.begin(), refreshed.end());
                refreshed.erase(std::unique(refreshed.begin(), refreshed.end()),
                                refreshed.end());
                runtime.members = std::move(refreshed);
            }

            for (const ObjectId targetId : runtime.members) {
                const std::optional<ecs::entity> target =
                    lifecycle.entityFromId(targetId);
                if (!target || !alive(registry, lifecycle, targetId, *target) ||
                    !(hasKind(registry, *target, game::ObjectKindOf::Score) ||
                      hasKind(registry, *target,
                              game::ObjectKindOf::ScoreCreate) ||
                      hasKind(registry, *target,
                              game::ObjectKindOf::ScoreDestroy) ||
                      hasKind(registry, *target,
                              game::ObjectKindOf::MpCountForVictory))) {
                    continue;
                }
                if (hasAnyDamageWeapon(registry, *target, content)) {
                    auto& desired = aura[targetId];
                    desired |= game::weaponBonusConditionBit(
                        game::WeaponBonusCondition::Enthusiastic);
                    if (upgraded) {
                        desired |= game::weaponBonusConditionBit(
                            game::WeaponBonusCondition::Subliminal);
                    }
                }

                auto* benefactor = ecs::try_get<ObjectPropagandaBenefactorComponent>(registry, *target);
                if (!benefactor) benefactor =
                    &ecs::emplace<ObjectPropagandaBenefactorComponent>(
                        registry, *target);
                if (!benefactor->source || benefactor->source == source.id || confirmedTick > benefactor->expiresTick) {
                    benefactor->source = source.id;
                    benefactor->expiresTick = runtime.nextScanTick;
                    if (const auto* health = ecs::try_get<ObjectHealthComponent>(registry, *target)) {
                        const math::q32_32 percent = upgraded ? rule.upgradedHealPercentPerSecond : rule.healPercentPerSecond;
                        const math::q32_32 amount = health->maximumFixed * percent /
                            math::q32_32{static_cast<int32_t>(std::max(1u, rules.logicFramesPerSecond))};
                        if (amount > math::q32_32{}) damageRequests.push_back({
                            .target = targetId, .source = source.id,
                            .sourceSequence = rule.authoredOrder,
                            .submissionOrdinal = claimGameplayOrdinal(),
                            .amount = amount,
                            .damageType = game::DamageType::HEALING,
                            .confirmedTick = confirmedTick,
                        });
                    }
                }
            }
        }
    }
    // Snapshot and sort by ObjectId before touching anything: the transitions
    // below consume the shared SimulationRandom (setObjectWeaponBonusCondition
    // restarts weapon timers via chooseShotDelayFrames), so walking raw entt
    // storage order would couple the RNG stream position to the registry's
    // dense layout — the same reason `candidates` above is sorted.  Collecting
    // first also stops the emplace/remove below from mutating a pool while the
    // view over it is being iterated.
    container::Vector<Candidate> auraSubjects;
    const auto auraView = ecs::view<const ObjectIdentityComponent>(registry);
    for (const ecs::entity entity : auraView) {
        auraSubjects.push_back(
            {auraView.template get<const ObjectIdentityComponent>(entity).id, entity});
    }
    std::sort(auraSubjects.begin(), auraSubjects.end(),
              [](const Candidate& a, const Candidate& b) { return a.id < b.id; });
    for (const Candidate& subject : auraSubjects) {
        const ecs::entity entity = subject.entity;
        const auto found = aura.find(subject.id);
        const game::WeaponBonusConditionMask desired = found == aura.end() ? 0 : found->second;
        auto* projection = ecs::try_get<ObjectPropagandaAuraProjection>(registry, entity);
        const game::WeaponBonusConditionMask previous = projection ? projection->conditions : 0;
        for (const game::WeaponBonusCondition condition : {
                game::WeaponBonusCondition::Enthusiastic,
                game::WeaponBonusCondition::Subliminal}) {
            const auto bit = game::weaponBonusConditionBit(condition);
            if ((previous & bit) != (desired & bit)) static_cast<void>(setObjectWeaponBonusCondition(
                registry, entity, condition, (desired & bit) != 0, &content, random,
                rules.logicFramesPerSecond, confirmedTick));
        }
        if (desired == 0) {
            if (projection) ecs::remove<ObjectPropagandaAuraProjection>(registry, entity);
        } else if (projection) projection->conditions = desired;
        else ecs::emplace<ObjectPropagandaAuraProjection>(registry, entity, desired);
    }

    // Assisted targeting observes actual shots published by Combat earlier in
    // this confirmed frame, then gives equivalent nearby units a typed attack.
    for (const Candidate& requester : candidates) {
        const auto* requesterType = ecs::try_get<ThingTemplateComponent>(registry, requester.entity);
        const auto* requesterTransform = ecs::try_get<TransformComponent>(registry, requester.entity);
        const auto* requesterOwner = ecs::try_get<OwnerComponent>(registry, requester.entity);
        const auto* weapons = ecs::try_get<ObjectWeaponComponent>(registry, requester.entity);
        if (!requesterType || !requesterType->archetype || !requesterTransform || !requesterOwner ||
            !weapons || !weapons->target || !weapons->activeWeaponSetIndex || !weapons->currentSlot) continue;
        if (*weapons->activeWeaponSetIndex >= weapons->sets.size()) continue;
        const auto& set = weapons->sets[*weapons->activeWeaponSetIndex];
        const size_t firedSlot = static_cast<size_t>(*weapons->currentSlot);
        if (firedSlot >= set.slots.size() || set.slots[firedSlot].lastFireTick != confirmedTick) continue;
        const auto* weapon = content.findWeapon(set.slots[firedSlot].content);
        if (!weapon || weapon->fixed.requestAssistRange <= math::q32_32{}) continue;
        const math::q32_32 rangeSquared =
            weapon->fixed.requestAssistRange *
            weapon->fixed.requestAssistRange;
        const LogicFixedVec3 requesterPosition =
            readAuthoritativeObjectPosition(
                registry, requester.entity, *requesterTransform);
        for (const Candidate& assistant : candidates) {
            if (assistant.id == requester.id) continue;
            auto& tactical = ecs::get<ObjectTacticalComponent>(registry, assistant.entity);
            if (!tactical.plan || tactical.plan->assistedTargeting.empty()) continue;
            const auto* assistantType = ecs::try_get<ThingTemplateComponent>(registry, assistant.entity);
            const auto* assistantOwner = ecs::try_get<OwnerComponent>(registry, assistant.entity);
            const auto* assistantTransform = ecs::try_get<TransformComponent>(registry, assistant.entity);
            auto* assistantWeapons = ecs::try_get<ObjectWeaponComponent>(registry, assistant.entity);
            if (!assistantType || !assistantType->archetype || !assistantOwner || !assistantTransform ||
                !assistantWeapons || assistantOwner->player != requesterOwner->player ||
                !game::legacyThingTemplatesEquivalent(
                    assistantType->archetype->templateData,
                    requesterType->archetype->templateData) ||
                !assistantWeapons->activeWeaponSetIndex ||
                *assistantWeapons->activeWeaponSetIndex >=
                    assistantWeapons->sets.size() ||
                !objectOwnWeaponsAbleToAttack(
                    registry, lifecycle, content, assistant.entity,
                    confirmedTick) ||
                !currentWeaponReadyForAssistance(
                    *assistantWeapons, content, confirmedTick)) continue;
            const LogicFixedVec3 assistantPosition =
                readAuthoritativeObjectPosition(
                    registry, assistant.entity, *assistantTransform);
            const math::q32_32 dx =
                assistantPosition.x - requesterPosition.x;
            const math::q32_32 dy =
                assistantPosition.y - requesterPosition.y;
            if (dx * dx + dy * dy > rangeSquared) continue;
            const auto& rule = tactical.plan->assistedTargeting.front();
            const size_t slot = static_cast<size_t>(rule.assistingWeaponSlot);
            auto& assistantSet = assistantWeapons->sets[*assistantWeapons->activeWeaponSetIndex];
            if (slot >= assistantSet.slots.size() ||
                !content.findWeapon(assistantSet.slots[slot].content)) continue;
            assistantWeapons->lockedSlot = rule.assistingWeaponSlot;
            assistantWeapons->lockType = ObjectWeaponLockType::Temporary;
            auto* queue = ecs::try_get<ObjectOrderQueueComponent>(registry, assistant.entity);
            if (!queue) queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, assistant.entity);
            insertSystemOrder(*queue, {
                .kind = ObjectOrderKind::Attack, .source = ObjectOrderSource::System,
                .contextPlayer = assistantOwner->player, .issuedTick = confirmedTick,
                .sourceSequence = rule.authoredOrder, .targetObject = weapons->target,
                .maximumShots = rule.assistingClipSize,
            }, ObjectOrderSystemPurpose::TacticalAssist, rule.authoredOrder);
            if (!rule.laserFromAssisted.empty() ||
                !rule.laserToTarget.empty()) {
                m_tacticalPresentationEvents.push_back({
                    .kind = ObjectTacticalPresentationEventKind::AssistedTargetingLasers,
                    .source = requester.id,
                    .assisted = assistant.id,
                    .target = weapons->target,
                    .primaryResource = rule.laserFromAssisted,
                    .secondaryResource = rule.laserToTarget,
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                });
            }
        }
    }

    // Per-object tactical state machines.
    for (const Candidate& candidate : candidates) {
        auto& component = ecs::get<ObjectTacticalComponent>(registry, candidate.entity);
        if (!component.plan) continue;
        auto* queue = ecs::try_get<ObjectOrderQueueComponent>(registry, candidate.entity);

        for (size_t index = 0; index < component.deployStyles.size(); ++index) {
            auto& runtime = component.deployStyles[index];
            const auto& rule = component.plan->deployStyles[index];
            const ObjectOrderIntent* headOrder =
                queue && !queue->orders.empty() ? &queue->orders.front()
                                                 : nullptr;
            const ObjectOrderKind head = headOrder
                ? headOrder->kind : ObjectOrderKind::Stop;
            ObjectWeaponComponent* combatWeapons =
                ecs::try_get<ObjectWeaponComponent>(
                    registry, candidate.entity);
            const ObjectLocomotionComponent* locomotion =
                ecs::try_get<ObjectLocomotionComponent>(
                    registry, candidate.entity);
            // DeployStyleAIUpdate does not deploy merely because an Attack
            // command is pending.  Its inherited AI first approaches the
            // victim, and the deploy gate opens only once the current weapon
            // is in range.  Guard-idle is the one deliberate eager-deploy
            // case so the unit is ready for the next acquisition.
            bool attackInRange =
                head == ObjectOrderKind::Attack && headOrder &&
                deployStyleCurrentWeaponIsInRange(
                    registry, lifecycle, content, candidate.entity,
                    *headOrder);
            if (!attackInRange && combatWeapons && combatWeapons->target &&
                combatWeapons->state != ObjectWeaponRuntimeState::Idle) {
                // Hunt/Guard/AttackArea own their concrete victim in the
                // nested AttackObject child rather than replacing the
                // TacticalAttack queue head.  RefCode asks AIUpdate for the
                // current victim, so project the combat-owned target here.
                ObjectOrderIntent activeAttack;
                activeAttack.kind = ObjectOrderKind::Attack;
                activeAttack.targetObject = combatWeapons->target;
                attackInRange = deployStyleCurrentWeaponIsInRange(
                    registry, lifecycle, content, candidate.entity,
                    activeAttack);
            }
            const bool guardIdle = headOrder &&
                headOrder->kind == ObjectOrderKind::TacticalAttack &&
                headOrder->tacticalAttackSubtype ==
                    ObjectTacticalAttackSubtype::Guard;
            const bool wantsDeployed = attackInRange || guardIdle;
            const bool wantsMovement =
                (locomotion && locomotion->hasActiveMove) ||
                head == ObjectOrderKind::Move ||
                (head == ObjectOrderKind::Attack && !attackInRange);
            // DeployStyleAIUpdate::setMyState owns the turret gate. RefCode
            // enables the turret only on entry to READY_TO_ATTACK and disables
            // it on entry to UNDEPLOY, both gated on
            // TurretsFunctionOnlyWhenDeployed, so a packed Nuke/Inferno Cannon
            // cannot fire. TurretsMustCenterBeforePacking inserts the
            // ALIGNING_TURRETS wait that recentres the barrel before the pack
            // animation starts. Shipped data always co-authors the two flags,
            // so either one claims ownership of the enabled bit and the
            // recentre request below always has a matching re-enable.
            const bool ownsTurretGate = rule.turretsFunctionOnlyWhenDeployed ||
                rule.turretsMustCenterBeforePacking;
            ObjectWeaponComponent* turretWeapons = ownsTurretGate
                ? combatWeapons : nullptr;
            const uint64_t packFrames = millisecondsToTicks(
                rule.packMilliseconds, rules.logicFramesPerSecond);
            const uint64_t unpackFrames = millisecondsToTicks(
                rule.unpackMilliseconds, rules.logicFramesPerSecond);
            const auto remainingTransitionFrames = [&]() noexcept {
                return runtime.transitionEndTick > confirmedTick
                    ? runtime.transitionEndTick - confirmedTick : uint64_t{0};
            };
            const auto beginDeploy = [&](bool reverseUndeploy) {
                const uint64_t previousFramesLeft =
                    remainingTransitionFrames();
                const uint64_t duration = reverseUndeploy
                    ? (unpackFrames > previousFramesLeft
                           ? unpackFrames - previousFramesLeft : uint64_t{0})
                    : unpackFrames;
                runtime.state = ObjectDeployStyleState::Deploying;
                runtime.transitionEndTick = saturatingAdd(
                    confirmedTick, duration);
                setModelCondition(
                    registry, candidate.entity,
                    game::ModelConditionFlag::Packing, false, confirmedTick);
                setModelCondition(
                    registry, candidate.entity,
                    game::ModelConditionFlag::Unpacking, true, confirmedTick);
                m_tacticalPresentationEvents.push_back({
                    .kind = ObjectTacticalPresentationEventKind::DeployStarted,
                    .source = candidate.id,
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                });
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, candidate.entity, {
                        .setMask = statusBit(game::ObjectStatusFlag::NoAttack),
                        .confirmedTick = confirmedTick,
                    }));
            };
            const auto beginUndeploy = [&](bool reverseDeploy) {
                const uint64_t previousFramesLeft =
                    remainingTransitionFrames();
                // RefCode reverses either direction against UnpackTime. With
                // the stock equal Pack/Unpack durations this preserves the
                // exact currently displayed manual frame.
                const uint64_t duration = reverseDeploy
                    ? (unpackFrames > previousFramesLeft
                           ? unpackFrames - previousFramesLeft : uint64_t{0})
                    : packFrames;
                runtime.state = ObjectDeployStyleState::Undeploying;
                runtime.transitionEndTick = saturatingAdd(
                    confirmedTick, duration);
                setModelCondition(
                    registry, candidate.entity,
                    game::ModelConditionFlag::Unpacking, false, confirmedTick);
                setModelCondition(
                    registry, candidate.entity,
                    game::ModelConditionFlag::Deployed, false,
                    confirmedTick);
                setModelCondition(
                    registry, candidate.entity,
                    game::ModelConditionFlag::Packing, true, confirmedTick);
                m_tacticalPresentationEvents.push_back({
                    .kind = ObjectTacticalPresentationEventKind::UndeployStarted,
                    .source = candidate.id,
                    .authoredOrder = rule.authoredOrder,
                    .confirmedTick = confirmedTick,
                });
                static_cast<void>(ObjectStatusSystem::apply(registry, candidate.entity, {
                    .setMask = statusBit(game::ObjectStatusFlag::NoAttack),
                    .clearMask = statusBit(game::ObjectStatusFlag::Deployed),
                    .confirmedTick = confirmedTick,
                }));
                if (rule.turretsFunctionOnlyWhenDeployed && turretWeapons) {
                    setObjectTurretsEnabled(*turretWeapons, false);
                }
            };

            // RefCode settles a completed transition before observing the
            // current command, allowing a new opposite transition to begin on
            // this same confirmed frame.
            if (runtime.state == ObjectDeployStyleState::Deploying &&
                confirmedTick >= runtime.transitionEndTick) {
                runtime.state = ObjectDeployStyleState::ReadyToAttack;
                setModelCondition(registry, candidate.entity, game::ModelConditionFlag::Unpacking, false,
                                  confirmedTick);
                setModelCondition(registry, candidate.entity,
                                  game::ModelConditionFlag::Deployed, true,
                                  confirmedTick);
                static_cast<void>(ObjectStatusSystem::apply(registry, candidate.entity, {
                    .setMask = statusBit(game::ObjectStatusFlag::Deployed),
                    .clearMask = statusBit(game::ObjectStatusFlag::NoAttack),
                    .confirmedTick = confirmedTick,
                }));
                if (turretWeapons) setObjectTurretsEnabled(*turretWeapons, true);
            } else if (runtime.state == ObjectDeployStyleState::Undeploying &&
                       confirmedTick >= runtime.transitionEndTick) {
                runtime.state = ObjectDeployStyleState::ReadyToMove;
                setModelCondition(registry, candidate.entity, game::ModelConditionFlag::Packing, false,
                                  confirmedTick);
                setModelCondition(registry, candidate.entity,
                                  game::ModelConditionFlag::Deployed, false,
                                  confirmedTick);
            }

            if (runtime.state == ObjectDeployStyleState::ReadyToMove &&
                wantsDeployed) {
                beginDeploy(false);
            } else if (runtime.state == ObjectDeployStyleState::Deploying &&
                       wantsMovement) {
                beginUndeploy(true);
            } else if (runtime.state == ObjectDeployStyleState::ReadyToAttack &&
                       wantsMovement) {
                if (rule.turretsMustCenterBeforePacking && turretWeapons &&
                    !objectTurretsInNaturalPosition(*turretWeapons)) {
                    runtime.state = ObjectDeployStyleState::AligningTurrets;
                    runtime.transitionEndTick = 0;
                    if (!objectTurretsAreForcedRecentering(*turretWeapons)) {
                        requestObjectTurretRecentering(*turretWeapons);
                    }
                } else {
                    beginUndeploy(false);
                }
            } else if (runtime.state == ObjectDeployStyleState::AligningTurrets) {
                if (wantsDeployed) {
                    // RefCode abandons the alignment wait and resumes firing
                    // the moment a victim is back in range; the unit never
                    // left the deployed pose.
                    runtime.state = ObjectDeployStyleState::ReadyToAttack;
                    if (turretWeapons) setObjectTurretsEnabled(*turretWeapons, true);
                } else if (!turretWeapons ||
                           objectTurretsInNaturalPosition(*turretWeapons)) {
                    beginUndeploy(false);
                }
            } else if (runtime.state == ObjectDeployStyleState::Undeploying &&
                       wantsDeployed) {
                beginDeploy(true);
            }

            if (rule.manualDeployAnimations) {
                uint64_t manualFrame = 0;
                bool submitFrame = true;
                if (runtime.state == ObjectDeployStyleState::Deploying) {
                    const uint64_t framesLeft = remainingTransitionFrames();
                    manualFrame = packFrames > framesLeft
                        ? packFrames - framesLeft : uint64_t{0};
                } else if (runtime.state ==
                           ObjectDeployStyleState::Undeploying) {
                    manualFrame = remainingTransitionFrames();
                } else {
                    submitFrame = false;
                }
                if (submitFrame) {
                    m_deployStyleManualFrameEvents.push_back({
                        .object = candidate.id,
                        .frame = static_cast<uint32_t>(std::min<uint64_t>(
                            manualFrame,
                            std::numeric_limits<uint32_t>::max())),
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                }
            }
        }

        for (size_t index = 0; index < component.topple.size(); ++index) {
            auto& runtime = component.topple[index];
            const auto& rule = component.plan->topple[index];
            if (!runtime.active || runtime.finished) continue;
            if (rule.killWhenStartToppling && !runtime.startKillIssued) {
                damageRequests.push_back({.target = candidate.id, .sourceSequence = rule.authoredOrder,
                    .submissionOrdinal = claimGameplayOrdinal(),
                    .damageType = game::DamageType::UNRESISTABLE, .forceKill = true,
                    .confirmedTick = confirmedTick});
                runtime.startKillIssued = true;
                continue;
            }
            auto* physics = ecs::try_get<ObjectPhysicsComponent>(registry, candidate.entity);
            if (!physics) continue;
            if (runtime.yawStepsRemaining != 0) {
                physics->yaw = game::normalizeToppleAngle(
                    physics->yaw + runtime.yawDelta);
                --runtime.yawStepsRemaining;
            }

            // ToppleSpeed and the velocity/acceleration percentages are
            // authored per logic frame. Keep the signed velocity: after the
            // first ground strike ZH reverses it, rises slightly, then falls
            // again until BounceVelocityPercent decays below the stop limit.
            math::q32_32 velocity = runtime.angularVelocity;
            if (velocity > math::q32_32{} &&
                runtime.angularAccumulation + velocity >
                    game::kToppleHalfPi) {
                velocity = game::kToppleHalfPi -
                    runtime.angularAccumulation;
            }
            physics->roll -= velocity * runtime.direction.y;
            physics->pitch += velocity * runtime.direction.x;
            runtime.angularAccumulation += velocity;
            physics->ownsAttitude = true;
            physics->sleeping = false;
            markObjectDirty(
                registry, candidate.entity,
                objectDirtyBit(ObjectDirtyDomain::Spatial) |
                    objectDirtyBit(ObjectDirtyDomain::RenderExtraction));

            if (runtime.angularAccumulation >= game::kToppleHalfPi &&
                runtime.angularVelocity > math::q32_32{}) {
                constexpr math::q32_32 kBounceStopVelocity =
                    math::q32_32::from_fraction(1, 100);
                constexpr math::q32_32 kBounceFxVelocity =
                    math::q32_32::from_fraction(3, 100);
                runtime.angularVelocity =
                    -runtime.angularVelocity * rule.bounceVelocityPercent;
                if (runtime.noBounce ||
                    math::q32_32::abs(runtime.angularVelocity) <
                        kBounceStopVelocity) {
                    runtime.angularVelocity = {};
                    runtime.finished = true;

                    if (rule.killWhenFinishedToppling) {
                        damageRequests.push_back({
                            .target = candidate.id,
                            .sourceSequence = rule.authoredOrder,
                            .submissionOrdinal = claimGameplayOrdinal(),
                            .damageType = game::DamageType::UNRESISTABLE,
                            .deathType = game::DeathType::TOPPLED,
                            .forceKill = true,
                            .confirmedTick = confirmedTick,
                        });

                        if (rule.reorientToppledRubble) {
                            const ObjectGeometryComponent* geometry =
                                ecs::try_get<ObjectGeometryComponent>(
                                    registry, candidate.entity);
                            const math::q32_32 topHeight = geometry
                                ? geometry->shape == ObjectGeometryShape::Sphere
                                    ? geometry->majorRadiusFixed
                                    : geometry->heightFixed
                                : math::q32_32{};
                            LogicFixedVec3 position = physics->position;
                            if (!physics->hasAuthoritativePosition) {
                                if (const ObjectFixedTransformComponent* fixed =
                                        ecs::try_get<
                                            ObjectFixedTransformComponent>(
                                            registry, candidate.entity)) {
                                    position = fixed->position;
                                }
                            }
                            position.x += runtime.direction.x * topHeight;
                            position.y += runtime.direction.y * topHeight;
                            position.z += runtime.direction.z * topHeight;
                            writeAuthoritativeObjectPosition(
                                registry, candidate.entity, position);
                            physics->position = position;
                            physics->hasAuthoritativePosition = true;
                            physics->pitch = {};
                            physics->roll = {};
                            physics->ownsAttitude = true;
                        }
                    }

                    if (rule.killStumpWhenToppled) {
                        ObjectId stump = INVALID_OBJECT_ID;
                        const auto stumpView = ecs::view<
                            const ObjectIdentityComponent,
                            const ObjectToppleStumpOwnerComponent>(registry);
                        for (const ecs::entity stumpEntity : stumpView) {
                            const ObjectToppleStumpOwnerComponent& link =
                                stumpView.template get<
                                    const ObjectToppleStumpOwnerComponent>(
                                    stumpEntity);
                            if (link.source != candidate.id ||
                                link.ruleIndex != index) {
                                continue;
                            }
                            const ObjectId found = stumpView.template get<
                                const ObjectIdentityComponent>(stumpEntity).id;
                            if (found && (!stump || found < stump))
                                stump = found;
                        }
                        if (stump) {
                            damageRequests.push_back({
                                .target = stump,
                                .sourceSequence = rule.authoredOrder,
                                .submissionOrdinal = claimGameplayOrdinal(),
                                .damageType = game::DamageType::UNRESISTABLE,
                                .deathType = game::DeathType::TOPPLED,
                                .forceKill = true,
                                .confirmedTick = confirmedTick,
                            });
                        }
                    }
                } else if (!runtime.noFx && !rule.bounceFx.empty() &&
                           math::q32_32::abs(runtime.angularVelocity) >=
                               kBounceFxVelocity) {
                    LogicFixedVec3 position{};
                    if (const ObjectFixedTransformComponent* fixed =
                            ecs::try_get<ObjectFixedTransformComponent>(
                                registry, candidate.entity)) {
                        position = fixed->position;
                    }
                    m_toppleFxEvents.push_back({
                        .object = candidate.id,
                        .position = position,
                        .yawRadians = physics->yaw,
                        .fxList = rule.bounceFx,
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                }
            } else {
                runtime.angularVelocity += runtime.angularAcceleration;
            }
        }

        for (size_t index = 0; index < component.battlePlans.size(); ++index) {
            auto& runtime = component.battlePlans[index];
            const auto& rule = component.plan->battlePlans[index];
            if (confirmedTick < runtime.nextTransitionTick) continue;
            const OwnerComponent* battlePlanOwner =
                ecs::try_get<OwnerComponent>(registry, candidate.entity);
            const PlayerId owner = battlePlanOwner
                ? battlePlanOwner->player : INVALID_PLAYER_ID;
            if (runtime.transition == ObjectBattlePlanTransition::Idle) {
                if (runtime.desired == game::ObjectBattlePlanStatus::None)
                    continue;
                runtime.current = runtime.desired;
                runtime.transition = ObjectBattlePlanTransition::Unpacking;
                runtime.nextTransitionTick = saturatingAdd(
                    confirmedTick,
                    millisecondsToTicks(
                        ruleAnimationMilliseconds(rule, runtime.current),
                        rules.logicFramesPerSecond));
                setBattlePlanDoorCondition(
                    registry, candidate.entity,
                    battlePlanDoorSlot(runtime.current),
                    ObjectModelConditionDoorPhase::Opening,
                    confirmedTick);
                appendBattlePlanPresentationEvent(
                    m_battlePlanPresentationEvents,
                    ObjectBattlePlanPresentationPhase::Unpacking,
                    candidate.id, owner, rule, runtime.current,
                    confirmedTick);
            } else if (runtime.transition == ObjectBattlePlanTransition::Unpacking) {
                setBattlePlanDoorCondition(
                    registry, candidate.entity,
                    battlePlanDoorSlot(runtime.current),
                    ObjectModelConditionDoorPhase::Unspecified,
                    confirmedTick);
                setBattlePlanDoorCondition(
                    registry, candidate.entity,
                    battlePlanDoorSlot(runtime.current),
                    ObjectModelConditionDoorPhase::WaitingToClose,
                    confirmedTick);
                runtime.transition = ObjectBattlePlanTransition::Active;
                if (runtime.current ==
                    game::ObjectBattlePlanStatus::Bombardment) {
                    if (ObjectWeaponComponent* weapons =
                            ecs::try_get<ObjectWeaponComponent>(
                                registry, candidate.entity)) {
                        setObjectTurretsEnabled(*weapons, true);
                    }
                }
                applyStrategyCenterBattlePlan(
                    registry, candidate.entity, rule, runtime.current, true,
                    rules, confirmedTick);
                appendBattlePlanPresentationEvent(
                    m_battlePlanPresentationEvents,
                    ObjectBattlePlanPresentationPhase::Active,
                    candidate.id, owner, rule, runtime.current,
                    confirmedTick);
            } else if (runtime.transition == ObjectBattlePlanTransition::Active && runtime.desired != runtime.current) {
                if (runtime.current ==
                    game::ObjectBattlePlanStatus::Bombardment) {
                    if (ObjectWeaponComponent* weapons =
                            ecs::try_get<ObjectWeaponComponent>(
                                registry, candidate.entity)) {
                        if (!objectTurretsInNaturalPosition(*weapons)) {
                            if (!objectTurretsAreForcedRecentering(*weapons)) {
                                requestObjectTurretRecentering(*weapons);
                            }
                            continue;
                        }
                        setObjectTurretsEnabled(*weapons, false);
                    }
                }
                setBattlePlanDoorCondition(
                    registry, candidate.entity,
                    battlePlanDoorSlot(runtime.current),
                    ObjectModelConditionDoorPhase::Unspecified,
                    confirmedTick);
                setBattlePlanDoorCondition(
                    registry, candidate.entity,
                    battlePlanDoorSlot(runtime.current),
                    ObjectModelConditionDoorPhase::Closing,
                    confirmedTick);
                applyStrategyCenterBattlePlan(
                    registry, candidate.entity, rule, runtime.current, false,
                    rules, confirmedTick);
                appendBattlePlanPresentationEvent(
                    m_battlePlanPresentationEvents,
                    ObjectBattlePlanPresentationPhase::Packing,
                    candidate.id, owner, rule, runtime.current,
                    confirmedTick);
                if (battlePlanOwner && rule.paralyzeMilliseconds != 0) {
                    const uint64_t until = saturatingAdd(
                        confirmedTick,
                        millisecondsToTicks(
                            rule.paralyzeMilliseconds,
                            rules.logicFramesPerSecond));
                    const auto members = ecs::view<const OwnerComponent>(registry);
                    for (const ecs::entity member : members) {
                        if (members.template get<const OwnerComponent>(member).player ==
                                battlePlanOwner->player &&
                            memberMatchesBattlePlan(registry, member, rule)) {
                            static_cast<void>(ObjectDisabledSystem::setUntil(
                                registry, member,
                                ObjectDisabledReason::Paralyzed, until,
                                confirmedTick));
                        }
                    }
                }
                runtime.transition = ObjectBattlePlanTransition::Packing;
                runtime.nextTransitionTick = saturatingAdd(confirmedTick,
                    millisecondsToTicks(ruleAnimationMilliseconds(rule, runtime.current), rules.logicFramesPerSecond));
            } else if (runtime.transition == ObjectBattlePlanTransition::Packing) {
                setBattlePlanDoorCondition(
                    registry, candidate.entity,
                    battlePlanDoorSlot(runtime.current),
                    ObjectModelConditionDoorPhase::Unspecified,
                    confirmedTick);
                runtime.current = game::ObjectBattlePlanStatus::None;
                runtime.transition = ObjectBattlePlanTransition::Idle;
                runtime.nextTransitionTick = saturatingAdd(confirmedTick,
                    millisecondsToTicks(rule.transitionIdleMilliseconds,
                                        rules.logicFramesPerSecond));
            }
        }

        for (size_t index = 0; index < component.specialAbilities.size(); ++index) {
            auto& runtime = component.specialAbilities[index];
            const auto& rule = component.plan->specialAbilities[index];

            // RefCode sleeps an inactive SpecialAbilityUpdate forever unless
            // AlwaysValidateSpecialObjects is authored.  Preserve that rule:
            // ordinary persistent lists are validated again when the ability
            // becomes active, while the explicit option keeps pruning dead
            // objects during inactive ticks.
            if (runtime.active || rule.alwaysValidateSpecialObjects) {
                runtime.specialObjects.erase(std::remove_if(
                    runtime.specialObjects.begin(),
                    runtime.specialObjects.end(),
                    [&](const ObjectSpecialAbilityObject& special) {
                        const auto entity =
                            lifecycle.entityFromId(special.object);
                        return !entity || !alive(
                            registry, lifecycle, special.object, *entity);
                    }), runtime.specialObjects.end());
            }
            if (!runtime.active) continue;

            const auto targetEntity = lifecycle.entityFromId(runtime.target);
            const auto* targetTransform = targetEntity
                ? ecs::try_get<TransformComponent>(registry, *targetEntity)
                : nullptr;
            auto* sourceTransform =
                ecs::try_get<TransformComponent>(registry, candidate.entity);
            const OwnerComponent* sourceOwner =
                ecs::try_get<OwnerComponent>(registry, candidate.entity);
            if (targetTransform) {
                runtime.targetPosition = readAuthoritativeObjectPosition(
                    registry, *targetEntity, *targetTransform);
                runtime.hasTargetPosition = true;
            }

            const bool externalInterrupted = queue &&
                queue->externalRevision !=
                    runtime.observedExternalRevision;
            const bool unpackingStillInProgress =
                runtime.phase == ObjectSpecialAbilityPhase::Unpacking &&
                confirmedTick < runtime.phaseEndTick;
            bool targetAbort = !unpackingStillInProgress && runtime.target &&
                (!targetEntity || !alive(
                    registry, lifecycle, runtime.target, *targetEntity));
            const ObjectStatusComponent* targetStatus = targetEntity
                ? ecs::try_get<ObjectStatusComponent>(registry,
                                                       *targetEntity)
                : nullptr;
            const bool hiddenTarget = targetStatus &&
                targetStatus->hasAny(statusBit(
                    game::ObjectStatusFlag::Stealthed)) &&
                !targetStatus->hasAny(statusBit(
                    game::ObjectStatusFlag::Detected));
            const bool preparationIncomplete =
                runtime.phase == ObjectSpecialAbilityPhase::Preparing &&
                 confirmedTick <= runtime.phaseEndTick &&
                 (runtime.effectTriggered
                      ? rule.persistentPrepMilliseconds != 0
                      : rule.preparationMilliseconds != 0);
            const PrimaryTeamComponent* sourceTeam =
                ecs::try_get<PrimaryTeamComponent>(registry,
                                                   candidate.entity);
            const PrimaryTeamComponent* targetTeam = targetEntity
                ? ecs::try_get<PrimaryTeamComponent>(registry,
                                                      *targetEntity)
                : nullptr;
            const bool sameTeam = sourceTeam && targetTeam &&
                sourceTeam->team && sourceTeam->team == targetTeam->team;

            if (!unpackingStillInProgress && !targetAbort && targetEntity) {
                switch (runtime.specialPowerType) {
                case game::SpecialPowerType::InfantryCaptureBuilding:
                case game::SpecialPowerType::BlackLotusCaptureBuilding:
                case game::SpecialPowerType::HackerDisableBuilding:
                    targetAbort = sameTeam ||
                        (hiddenTarget && preparationIncomplete);
                    break;
                case game::SpecialPowerType::BlackLotusStealCashHack:
                case game::SpecialPowerType::BoobyTrap:
                    targetAbort = hiddenTarget && preparationIncomplete;
                    break;
                case game::SpecialPowerType::RemoteCharges:
                case game::SpecialPowerType::TimedCharges:
                    // RefCode deliberately defers this loss-of-target test
                    // until unpacking has completed.
                    targetAbort =
                        runtime.phase != ObjectSpecialAbilityPhase::Unpacking &&
                        hiddenTarget && preparationIncomplete;
                    break;
                case game::SpecialPowerType::
                    MissileDefenderLaserGuidedMissiles:
                    targetAbort = hasKind(
                        registry, *targetEntity,
                        game::ObjectKindOf::Structure) || hiddenTarget;
                    break;
                case game::SpecialPowerType::
                    BlackLotusDisableVehicleHack:
                    targetAbort = hiddenTarget;
                    break;
                default:
                    break;
                }
            }

            bool preparationFailed = false;
            const bool preparationContinues =
                runtime.phase == ObjectSpecialAbilityPhase::Preparing &&
                confirmedTick < runtime.phaseEndTick;
            if (!targetAbort && preparationContinues && targetEntity &&
                sourceOwner) {
                const bool alliedTarget = relationshipBetweenObjects(
                    registry, players, candidate.entity,
                    *targetEntity) == PlayerRelationship::Allies;
                switch (runtime.specialPowerType) {
                case game::SpecialPowerType::
                    MissileDefenderLaserGuidedMissiles:
                case game::SpecialPowerType::
                    BlackLotusDisableVehicleHack:
                case game::SpecialPowerType::InfantryCaptureBuilding:
                case game::SpecialPowerType::BlackLotusCaptureBuilding:
                    preparationFailed = alliedTarget;
                    break;
                default:
                    break;
                }
            }
            if (!targetAbort &&
                !preparationFailed && preparationContinues &&
                runtime.hasTargetPosition && sourceTransform &&
                rule.abilityAbortRange < math::q32_32{10'000'000}) {
                const LogicFixedVec3 source =
                    readAuthoritativeObjectPosition(
                        registry, candidate.entity, *sourceTransform);
                const math::q32_32 dx{runtime.targetPosition.x - source.x};
                const math::q32_32 dy{runtime.targetPosition.y - source.y};
                const math::q32_32 centerDistance =
                    math::q32_32::sqrt(dx * dx + dy * dy);
                const auto* sourceGeometry =
                    ecs::try_get<ObjectGeometryComponent>(
                        registry, candidate.entity);
                const auto* targetGeometry = targetEntity
                    ? ecs::try_get<ObjectGeometryComponent>(
                          registry, *targetEntity)
                    : nullptr;
                const math::q32_32 sourceRadius = sourceGeometry
                    ? math::q32_32::max(
                          math::q32_32{},
                          sourceGeometry->boundingCircleRadiusFixed)
                    : math::q32_32{};
                const math::q32_32 targetRadius = targetGeometry
                    ? math::q32_32::max(
                          math::q32_32{},
                          targetGeometry->boundingCircleRadiusFixed)
                    : math::q32_32{};
                const math::q32_32 distance = math::q32_32::max(
                    math::q32_32{}, centerDistance - sourceRadius -
                        (runtime.target ? targetRadius : math::q32_32{}));
                preparationFailed = distance > rule.abilityAbortRange;

                // RefCode derives contact-class behavior from the start
                // range, even while validating AbilityAbortRange. A short
                // object-targeted ability therefore aborts preparation if
                // the two actual geometries cease touching.
                const int64_t pathfindCellSizeRaw = navigation &&
                        navigation->grid().transform().cellSizeRaw > 0
                    ? navigation->grid().transform().cellSizeRaw
                    : (int64_t{10} << 32u);
                const math::q32_32 contactClassRange =
                    math::q32_32::max(
                        math::q32_32{}, rule.startAbilityRange -
                            math::q32_32::from_raw(pathfindCellSizeRaw) /
                                math::q32_32{int32_t{4}});
                if (!preparationFailed && runtime.target && targetEntity &&
                    targetTransform && sourceGeometry && targetGeometry &&
                    contactClassRange == math::q32_32{}) {
                    ObjectCollisionContact contact;
                    preparationFailed = !computeObjectCollisionContact(
                        source,
                        readAuthoritativeObjectYaw(
                            registry, candidate.entity, *sourceTransform),
                        *sourceGeometry, runtime.targetPosition,
                        readAuthoritativeObjectYaw(
                            registry, *targetEntity, *targetTransform),
                        *targetGeometry, contact);
                }
            }

            const auto queueDestroySpecialObjects = [&]() {
                if (runtime.specialObjects.empty()) return;
                ObjectSpecialAbilityEffectRequest request{
                    .kind = ObjectSpecialAbilityEffectKind::DestroySpecialObjects,
                    .source = candidate.id,
                    .specialPower = runtime.specialPower,
                    .specialPowerTemplate = rule.specialPowerTemplate,
                    .specialPowerType = runtime.specialPowerType,
                    .ruleIndex = static_cast<uint32_t>(index),
                    .authoredOrder = rule.authoredOrder,
                    .activationSequence = runtime.activationSequence,
                    .submissionOrdinal = claimGameplayOrdinal(),
                    .confirmedTick = confirmedTick,
                };
                request.objects.reserve(runtime.specialObjects.size());
                for (const ObjectSpecialAbilityObject& special :
                     runtime.specialObjects) request.objects.push_back(special.object);
                effectRequests.push_back(std::move(request));
                runtime.specialObjects.clear();
            };
            const auto finishAbility = [&](bool allowFlee,
                                           bool packingCompleted) {
                if (!rule.specialObjectsPersistent ||
                    specialAbilityEndPreparationDestroysObjects(
                        runtime.specialPowerType)) {
                    queueDestroySpecialObjects();
                }
                if (runtime.specialPowerType == game::SpecialPowerType::
                        MissileDefenderLaserGuidedMissiles) {
                    // SpecialAbilityUpdate::killSpecialObjects restores the
                    // primary weapon as soon as the guidance laser leaves.
                    if (ObjectWeaponComponent* weapons =
                            ecs::try_get<ObjectWeaponComponent>(
                                registry, candidate.entity)) {
                        weapons->lockedSlot = game::WeaponSlot::Primary;
                        weapons->lockType =
                            ObjectWeaponLockType::Temporary;
                    }
                }
                if (queue) {
                    const size_t oldSize = queue->orders.size();
                    queue->orders.erase(std::remove_if(
                        queue->orders.begin(), queue->orders.end(),
                        [&](const ObjectOrderIntent& pending) {
                            if (pending.source != ObjectOrderSource::System ||
                                pending.systemPurpose !=
                                    ObjectOrderSystemPurpose::SpecialAbility) {
                                return false;
                            }
                            return pending.sourceSequence ==
                                    rule.authoredOrder ||
                                pending.systemPurposeInstance ==
                                    static_cast<uint32_t>(index) ||
                                pending.systemPurposeInstance ==
                                    rule.authoredOrder;
                        }), queue->orders.end());
                    if (queue->orders.size() != oldSize) ++queue->revision;
                }
                const bool shouldFlee = allowFlee &&
                    rule.fleeRangeAfterCompletion > math::q32_32{} &&
                    runtime.hasTargetPosition;
                runtime.active = false;
                runtime.deferredRechargePending = false;
                runtime.phase = ObjectSpecialAbilityPhase::Inactive;
                runtime.finishTick = confirmedTick;
                runtime.fleeAfterPacking = false;
                setModelCondition(registry, candidate.entity, game::ModelConditionFlag::Unpacking, false,
                                  confirmedTick);
                setModelCondition(registry, candidate.entity, game::ModelConditionFlag::Packing, false,
                                  confirmedTick);
                clearSpecialAbilityPreparationConditions(
                    registry, candidate.entity, confirmedTick);
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, candidate.entity, {
                        .clearMask = statusBit(
                            game::ObjectStatusFlag::IsUsingAbility),
                        .confirmedTick = confirmedTick,
                    }));
                if (packingCompleted && sourceTransform &&
                    rule.flipOwnerAfterPacking) {
                    writeAuthoritativeObjectYaw(
                        registry, candidate.entity,
                        readAuthoritativeObjectYaw(
                            registry, candidate.entity,
                            *sourceTransform) +
                            game::kTopplePi);
                }
                if (shouldFlee && sourceTransform && sourceOwner) {
                    const math::q32_32 direction =
                        (rule.flipOwnerAfterPacking ||
                         rule.flipOwnerAfterUnpacking)
                        ? math::q32_32{int32_t{1}}
                        : math::q32_32{int32_t{-1}};
                    const math::q32_32_sincos heading =
                        math::fixed_sincos(
                            readAuthoritativeObjectYaw(
                                registry, candidate.entity,
                                *sourceTransform));
                    const LogicFixedVec3 sourcePosition =
                        readAuthoritativeObjectPosition(
                            registry, candidate.entity, *sourceTransform);
                    const LogicFixedVec3 fleeTarget{
                        sourcePosition.x + heading.cosine *
                            rule.fleeRangeAfterCompletion * direction,
                        sourcePosition.y + heading.sine *
                            rule.fleeRangeAfterCompletion * direction,
                        sourcePosition.z};
                    if (!queue) queue = &ecs::emplace<ObjectOrderQueueComponent>(
                        registry, candidate.entity);
                    insertSystemOrder(*queue, {
                        .kind = ObjectOrderKind::Move,
                        .source = ObjectOrderSource::System,
                        .contextPlayer = sourceOwner->player,
                        .issuedTick = confirmedTick,
                        .sourceSequence = rule.authoredOrder,
                        .targetX = fleeTarget.x,
                        .targetY = fleeTarget.y,
                        .targetZ = fleeTarget.z,
                        .hasTargetPosition = true,
                    }, ObjectOrderSystemPurpose::SpecialAbility,
                       rule.authoredOrder);
                }
            };

            if (externalInterrupted) {
                // This is the modern equivalent of
                // getLastCommandSource()!=CMD_FROM_AI. Do not pack, flee, or
                // clear the newer external command.
                finishAbility(false, false);
                continue;
            }
            if (runtime.phase == ObjectSpecialAbilityPhase::Packing) {
                if (confirmedTick >= runtime.phaseEndTick)
                    finishAbility(runtime.fleeAfterPacking, true);
                continue;
            }
            if (targetAbort) {
                // Dead/same-team/hidden targets take the direct onExit(false)
                // path in RefCode. Packing is reserved for a preparation
                // failure such as leaving AbilityAbortRange.
                finishAbility(false, false);
                continue;
            }
            if (preparationFailed) {
                if (rule.packMilliseconds != 0 &&
                    !(runtime.noTargetCommand &&
                      rule.skipPackingWithNoTarget)) {
                    // startPacking(false) changes completion audio only. Its
                    // eventual finishAbility still performs the authored flee
                    // move when a valid target exists.
                    runtime.fleeAfterPacking = runtime.hasTargetPosition;
                    runtime.phase = ObjectSpecialAbilityPhase::Packing;
                    runtime.phaseEndTick = saturatingAdd(
                        confirmedTick, millisecondsToTicks(
                            variedMilliseconds(rule.packMilliseconds,
                                rule.packUnpackVariationFactor, random),
                            rules.logicFramesPerSecond));
                    setModelCondition(registry, candidate.entity,
                                      game::ModelConditionFlag::Unpacking, false, confirmedTick);
                    clearSpecialAbilityPreparationConditions(
                        registry, candidate.entity, confirmedTick);
                    setModelCondition(registry, candidate.entity,
                                      game::ModelConditionFlag::Packing, true, confirmedTick);
                } else {
                    finishAbility(true, false);
                }
                continue;
            }

            if (runtime.phase == ObjectSpecialAbilityPhase::Facing) {
                if (runtime.facingFailed) {
                    finishAbility(false, false);
                    continue;
                }
                if (!runtime.facingComplete) {
                    if (!runtime.facingRequestQueued &&
                        !runtime.facingStateActive) {
                        facingRequests.push_back({
                            .source = candidate.id,
                            .target = runtime.target,
                            .targetPosition = runtime.targetPosition,
                            .hasTargetPosition =
                                !runtime.target &&
                                runtime.hasTargetPosition,
                            .ruleIndex = static_cast<uint32_t>(index),
                            .activationSequence =
                                runtime.activationSequence,
                            .confirmedTick = confirmedTick,
                        });
                        runtime.facingRequestQueued = true;
                    }
                    continue;
                }

                runtime.facingComplete = false;
                runtime.facingRequestIssuedTick = 0;
                runtime.facingRequestSequence = 0;
                const bool skipUnpack = runtime.noTargetCommand &&
                    rule.skipPackingWithNoTarget;
                if (!skipUnpack && rule.unpackMilliseconds != 0) {
                    runtime.phase = ObjectSpecialAbilityPhase::Unpacking;
                    runtime.phaseEndTick = saturatingAdd(
                        confirmedTick, millisecondsToTicks(
                            variedMilliseconds(
                                rule.unpackMilliseconds,
                                rule.packUnpackVariationFactor, random),
                            rules.logicFramesPerSecond));
                    runtime.triggerTick = runtime.phaseEndTick;
                    setModelCondition(
                        registry, candidate.entity,
                        game::ModelConditionFlag::Unpacking, true,
                        confirmedTick);
                    continue;
                }

                runtime.phase = ObjectSpecialAbilityPhase::Preparing;
                runtime.phaseEndTick = saturatingAdd(
                    confirmedTick, millisecondsToTicks(
                        rule.preparationMilliseconds,
                        rules.logicFramesPerSecond));
                runtime.triggerTick = runtime.phaseEndTick;
                if (const std::optional<game::ModelConditionFlag> pose =
                        specialAbilityPreparationCondition(
                            runtime.specialPowerType)) {
                    setModelCondition(
                        registry, candidate.entity, *pose, true,
                        confirmedTick);
                }
            }

            const uint64_t revealLead = millisecondsToTicks(
                rule.preTriggerUnstealthMilliseconds,
                rules.logicFramesPerSecond);
            if (rule.loseStealthOnTrigger &&
                !runtime.preTriggerRevealApplied &&
                runtime.phase == ObjectSpecialAbilityPhase::Unpacking &&
                revealLead != 0 && runtime.phaseEndTick > confirmedTick &&
                runtime.phaseEndTick - confirmedTick < revealLead) {
                runtime.preTriggerRevealApplied = true;
                effectRequests.push_back({
                    .kind = ObjectSpecialAbilityEffectKind::MarkDetected,
                    .source = candidate.id,
                    .specialPower = runtime.specialPower,
                    .specialPowerTemplate = rule.specialPowerTemplate,
                    .specialPowerType = runtime.specialPowerType,
                    .ruleIndex = static_cast<uint32_t>(index),
                    .authoredOrder = rule.authoredOrder,
                    .activationSequence = runtime.activationSequence,
                    .submissionOrdinal = claimGameplayOrdinal(),
                    .confirmedTick = confirmedTick,
                });
            }

            if (runtime.phase == ObjectSpecialAbilityPhase::Unpacking &&
                confirmedTick >= runtime.phaseEndTick) {
                setModelCondition(registry, candidate.entity, game::ModelConditionFlag::Unpacking,
                                  false, confirmedTick);
                if (sourceTransform && rule.flipOwnerAfterUnpacking) {
                    writeAuthoritativeObjectYaw(
                        registry, candidate.entity,
                        readAuthoritativeObjectYaw(
                            registry, candidate.entity,
                            *sourceTransform) +
                            game::kTopplePi);
                }
                runtime.phase = ObjectSpecialAbilityPhase::Preparing;
                runtime.phaseEndTick = saturatingAdd(
                    confirmedTick, millisecondsToTicks(
                        rule.preparationMilliseconds,
                        rules.logicFramesPerSecond));
                runtime.triggerTick = runtime.phaseEndTick;
                // RefCode SpecialAbilityUpdate::startPreparation. The pose must
                // cover the whole preparation window (the Ranger's capture is
                // authored at PreparationTime = 20000), so it belongs here and
                // not at the effect trigger below.
                if (const std::optional<game::ModelConditionFlag> pose =
                        specialAbilityPreparationCondition(
                            runtime.specialPowerType)) {
                    setModelCondition(registry, candidate.entity, *pose, true,
                                      confirmedTick);
                }
            }
            if (runtime.phase != ObjectSpecialAbilityPhase::Preparing)
                continue;

            if (!runtime.preparationObjectAttempted &&
                specialAbilityCreatesPreparationObject(
                    runtime.specialPowerType)) {
                runtime.preparationObjectAttempted = true;
                const bool atObjectLimit = rule.maximumSpecialObjects != 0 &&
                    runtime.specialObjects.size() >=
                        rule.maximumSpecialObjects;
                if (atObjectLimit && !rule.specialObjectsPersistent) {
                    queueDestroySpecialObjects();
                }
                if (runtime.target && !rule.specialObject.empty() &&
                    rule.maximumSpecialObjects != 0 &&
                    (!atObjectLimit || !rule.specialObjectsPersistent)) {
                    effectRequests.push_back({
                        .kind = ObjectSpecialAbilityEffectKind::
                            SpawnSpecialObject,
                        .source = candidate.id,
                        .target = runtime.target,
                        .specialPower = runtime.specialPower,
                        .specialPowerTemplate =
                            rule.specialPowerTemplate,
                        .specialPowerType = runtime.specialPowerType,
                        .objectTemplate = rule.specialObject,
                        .attachToBone =
                            rule.specialObjectAttachToBone,
                        .position = runtime.targetPosition,
                        .hasPosition = runtime.hasTargetPosition,
                        .attachStickyBomb = false,
                        .ruleIndex = static_cast<uint32_t>(index),
                        .authoredOrder = rule.authoredOrder,
                        .activationSequence =
                            runtime.activationSequence,
                        .submissionOrdinal = claimGameplayOrdinal(),
                        .confirmedTick = confirmedTick,
                    });
                }
            }

            if (rule.doCaptureFx && runtime.target) {
                const uint64_t totalTicks = std::max<uint64_t>(
                    1u, millisecondsToTicks(
                        rule.preparationMilliseconds,
                        rules.logicFramesPerSecond));
                const uint64_t remainingTicks = runtime.phaseEndTick >
                        confirmedTick
                    ? runtime.phaseEndTick - confirmedTick : 0u;
                const math::q32_32 progress = math::q32_32{int32_t{1}} -
                    math::q32_32::from_fraction(
                        static_cast<int64_t>(std::min(
                            remainingTicks, totalTicks)),
                        static_cast<int64_t>(totalTicks));
                const bool previousOdd =
                    (runtime.captureFlashPhase.to_int() & 1) != 0;
                runtime.captureFlashPhase += progress /
                    math::q32_32{int32_t{3}};
                const bool currentOdd =
                    (runtime.captureFlashPhase.to_int() & 1) != 0;
                if (previousOdd && !currentOdd) {
                    m_tacticalPresentationEvents.push_back({
                        .kind = ObjectTacticalPresentationEventKind::CapturePulse,
                        .source = candidate.id,
                        .target = runtime.target,
                        .specialPowerType = runtime.specialPowerType,
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                }
            }

            // RefCode calls markSpecialPowerTriggered() from
            // SpecialAbilityUpdate::startPreparation(), after approach,
            // facing and unpacking have succeeded but before the preparation
            // countdown advances.  The existing typed restart request keeps
            // recharge ownership in ObjectSpecialPowerSystem.
            const bool infantryCapture = runtime.specialPowerType ==
                game::SpecialPowerType::InfantryCaptureBuilding;
            if (runtime.deferredRechargePending ||
                (infantryCapture && !runtime.effectTriggered)) {
                const bool marksSpecialPowerTriggered =
                    runtime.deferredRechargePending;
                runtime.deferredRechargePending = false;
                effectRequests.push_back({
                    .kind = ObjectSpecialAbilityEffectKind::RestartRecharge,
                    .source = candidate.id,
                    .specialPower = runtime.specialPower,
                    .specialPowerTemplate = rule.specialPowerTemplate,
                    .specialPowerType = runtime.specialPowerType,
                    .ruleIndex = static_cast<uint32_t>(index),
                    .authoredOrder = rule.authoredOrder,
                    .activationSequence = runtime.activationSequence,
                    .marksSpecialPowerTriggered =
                        marksSpecialPowerTriggered,
                    .submissionOrdinal = claimGameplayOrdinal(),
                    .confirmedTick = confirmedTick,
                });
            }
            const bool persistentRechargeGate =
                rule.persistentPrepMilliseconds != 0 &&
                rule.persistenceRequiresRecharge &&
                (runtime.effectTriggered ||
                 rule.preparationMilliseconds != 0) &&
                confirmedTick > runtime.activationTick;
            if (persistentRechargeGate) {
                const auto* powers = ecs::try_get<ObjectSpecialPowerComponent>(
                    registry, candidate.entity);
                bool ready = false;
                if (runtime.specialPower && powers) {
                    for (const ObjectSpecialPowerRuntime& power :
                         powers->instances) {
                        if (power.content == runtime.specialPower) {
                            // RefCode requires getReadyFrame() < currentFrame,
                            // not equality.
                            ready = confirmedTick > power.readyTick;
                            break;
                        }
                    }
                }
                if (!ready) {
                    // Persistent preparation decrements only on frames where
                    // the SpecialPower module is ready. Shift the absolute
                    // deadline while blocked so the complete authored prep
                    // interval still runs after recharge, rather than running
                    // concurrently with it and triggering immediately.
                    runtime.phaseEndTick = saturatingAdd(
                        runtime.phaseEndTick, 1);
                    runtime.triggerTick = runtime.phaseEndTick;
                    continue;
                }
            }
            if (confirmedTick < runtime.phaseEndTick) continue;

            const bool checksBoobyTrap = checksBoobyTrapOnTarget(
                runtime.specialPowerType, runtime.noTargetCommand);
            if (checksBoobyTrap && runtime.target && targetStatus &&
                targetStatus->hasAny(statusBit(
                    game::ObjectStatusFlag::BoobyTrapped)) &&
                !runtime.boobyTrapTriggered) {
                runtime.boobyTrapTriggered = true;
                runtime.phaseEndTick = saturatingAdd(confirmedTick, 1);
                runtime.triggerTick = runtime.phaseEndTick;
                effectRequests.push_back({
                    .kind = ObjectSpecialAbilityEffectKind::TriggerTargetBoobyTrap,
                    .source = candidate.id,
                    .target = runtime.target,
                    .specialPower = runtime.specialPower,
                    .specialPowerTemplate = rule.specialPowerTemplate,
                    .specialPowerType = runtime.specialPowerType,
                    .ruleIndex = static_cast<uint32_t>(index),
                    .authoredOrder = rule.authoredOrder,
                    .activationSequence = runtime.activationSequence,
                    .submissionOrdinal = claimGameplayOrdinal(),
                    .confirmedTick = confirmedTick,
                });
                continue;
            }

            runtime.effectTriggered = true;
            if (rule.loseStealthOnTrigger &&
                !runtime.preTriggerRevealApplied &&
                !(runtime.specialPowerType ==
                      game::SpecialPowerType::RemoteCharges &&
                  runtime.noTargetCommand)) {
                runtime.preTriggerRevealApplied = true;
                effectRequests.push_back({
                    .kind = ObjectSpecialAbilityEffectKind::MarkDetected,
                    .source = candidate.id,
                    .specialPower = runtime.specialPower,
                    .specialPowerTemplate = rule.specialPowerTemplate,
                    .specialPowerType = runtime.specialPowerType,
                    .ruleIndex = static_cast<uint32_t>(index),
                    .authoredOrder = rule.authoredOrder,
                    .activationSequence = runtime.activationSequence,
                    .submissionOrdinal = claimGameplayOrdinal(),
                    .confirmedTick = confirmedTick,
                });
            }
            // RefCode triggerAbilityEffect() sets no model condition at all;
            // startPreparation() already selected the pose. Re-assert it here so
            // a persistent ability (PersistentPrepTime) that loops back through
            // trigger keeps its looping pose, but never invent FIRING_A for the
            // abilities RefCode leaves alone.
            if (const std::optional<game::ModelConditionFlag> pose =
                    specialAbilityPreparationCondition(
                        runtime.specialPowerType)) {
                setModelCondition(registry, candidate.entity, *pose, true,
                                  confirmedTick);
            }
            if (targetEntity && sourceOwner) {
                if (isDisableHack(runtime.specialPowerType)) {
                    if (relationshipBetweenObjects(
                            registry, players, candidate.entity,
                            *targetEntity) != PlayerRelationship::Allies) {
                        static_cast<void>(ObjectDisabledSystem::setUntil(
                            registry, *targetEntity,
                            ObjectDisabledReason::Hacked,
                            saturatingAdd(confirmedTick,
                                millisecondsToTicks(
                                    rule.effectDurationMilliseconds,
                                    rules.logicFramesPerSecond)),
                            confirmedTick));
                    }
                    m_tacticalPresentationEvents.push_back({
                        .kind = ObjectTacticalPresentationEventKind::
                            SpecialAbilityCompleted,
                        .source = candidate.id,
                        .target = runtime.target,
                        .specialPowerType = runtime.specialPowerType,
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                } else if (isCaptureBuilding(runtime.specialPowerType)) {
                    if (std::find(captureClaimedTargets.begin(),
                                  captureClaimedTargets.end(),
                                  runtime.target) !=
                        captureClaimedTargets.end()) {
                        finishAbility(false, false);
                        continue;
                    }
                    captureClaimedTargets.push_back(runtime.target);
                    defectionRequests.push_back({
                        .source = candidate.id,
                        .target = runtime.target,
                        .newOwner = sourceOwner->player,
                        .authoredOrder = rule.authoredOrder,
                        .submissionOrdinal =
                            nextGameplaySubmissionOrdinal++,
                        .confirmedTick = confirmedTick,
                    });
                    if (nextGameplaySubmissionOrdinal == 0) {
                        ++nextGameplaySubmissionOrdinal;
                    }
                    m_tacticalPresentationEvents.push_back({
                        .kind = ObjectTacticalPresentationEventKind::CaptureCompleted,
                        .source = candidate.id,
                        .target = runtime.target,
                        .specialPowerType = runtime.specialPowerType,
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                    if (runtime.specialPowerType ==
                        game::SpecialPowerType::BlackLotusCaptureBuilding) {
                        // Unlike infantry capture (which holds the timer on
                        // every preparation frame), Black Lotus starts a
                        // fresh full recharge only when capture completes.
                        effectRequests.push_back({
                            .kind = ObjectSpecialAbilityEffectKind::RestartRecharge,
                            .source = candidate.id,
                            .specialPower = runtime.specialPower,
                            .specialPowerTemplate = rule.specialPowerTemplate,
                            .specialPowerType = runtime.specialPowerType,
                            .ruleIndex = static_cast<uint32_t>(index),
                            .authoredOrder = rule.authoredOrder,
                            .activationSequence = runtime.activationSequence,
                            .submissionOrdinal = claimGameplayOrdinal(),
                            .confirmedTick = confirmedTick,
                        });
                    }
                } else if (runtime.specialPowerType ==
                           game::SpecialPowerType::BlackLotusStealCashHack) {
                    const auto* targetOwner =
                        ecs::try_get<OwnerComponent>(registry, *targetEntity);
                    const PlayerState* victim = targetOwner
                        ? players.get(targetOwner->player) : nullptr;
                    const PlayerState* receiver = players.get(
                        sourceOwner->player);
                    if (victim && receiver) {
                        const int64_t capacity =
                            std::numeric_limits<int64_t>::max() -
                            receiver->cash;
                        const int64_t amount = std::min({
                            victim->cash,
                            static_cast<int64_t>(std::max<int32_t>(
                                0, rule.effectValue)), capacity});
                        if (amount > 0 &&
                            players.trySpend(targetOwner->player, amount) &&
                            players.adjustCash(sourceOwner->player, amount)) {
                            static_cast<void>(players.recordMoneyEarned(
                                sourceOwner->player,
                                static_cast<uint64_t>(amount),
                            confirmedTick));
                        }
                    }
                    m_tacticalPresentationEvents.push_back({
                        .kind = ObjectTacticalPresentationEventKind::
                            SpecialAbilityCompleted,
                        .source = candidate.id,
                        .target = runtime.target,
                        .specialPowerType = runtime.specialPowerType,
                        .authoredOrder = rule.authoredOrder,
                        .confirmedTick = confirmedTick,
                    });
                } else if (runtime.specialPowerType ==
                    game::SpecialPowerType::MissileDefenderLaserGuidedMissiles) {
                    auto* weapons = ecs::try_get<ObjectWeaponComponent>(
                        registry, candidate.entity);
                    if (weapons) {
                        weapons->lockedSlot = game::WeaponSlot::Secondary;
                        weapons->lockType = ObjectWeaponLockType::Temporary;
                    }
                    if (!queue) queue = &ecs::emplace<ObjectOrderQueueComponent>(
                        registry, candidate.entity);
                    insertSystemOrder(*queue, {
                        .kind = ObjectOrderKind::Attack,
                        .source = ObjectOrderSource::System,
                        .contextPlayer = sourceOwner->player,
                        .issuedTick = confirmedTick,
                        .sourceSequence = rule.authoredOrder,
                        .targetObject = runtime.target,
                    }, ObjectOrderSystemPurpose::SpecialAbility,
                       rule.authoredOrder);
                }
            }

            const bool remoteCharge =
                isRemoteCharges(runtime.specialPowerType);
            const bool createsSpecialObject = game::createsSpecialObject(
                runtime.specialPowerType, runtime.noTargetCommand);
            if (remoteCharge && runtime.noTargetCommand) {
                ObjectSpecialAbilityEffectRequest request{
                    .kind = ObjectSpecialAbilityEffectKind::DetonateSpecialObjects,
                    .source = candidate.id,
                    .specialPower = runtime.specialPower,
                    .specialPowerTemplate = rule.specialPowerTemplate,
                    .specialPowerType = runtime.specialPowerType,
                    .ruleIndex = static_cast<uint32_t>(index),
                    .authoredOrder = rule.authoredOrder,
                    .activationSequence = runtime.activationSequence,
                    .submissionOrdinal = claimGameplayOrdinal(),
                    .confirmedTick = confirmedTick,
                };
                for (const ObjectSpecialAbilityObject& special :
                     runtime.specialObjects) request.objects.push_back(special.object);
                effectRequests.push_back(std::move(request));
            } else if (createsSpecialObject && !rule.specialObject.empty() &&
                       rule.maximumSpecialObjects != 0 &&
                       (!rule.specialObjectsPersistent ||
                        runtime.specialObjects.size() <
                            rule.maximumSpecialObjects)) {
                if (!rule.specialObjectsPersistent &&
                    rule.maximumSpecialObjects != 0 &&
                    runtime.specialObjects.size() >=
                        rule.maximumSpecialObjects) {
                    queueDestroySpecialObjects();
                }
                effectRequests.push_back({
                    .kind = ObjectSpecialAbilityEffectKind::SpawnSpecialObject,
                    .source = candidate.id,
                    .target = runtime.target,
                    .specialPower = runtime.specialPower,
                    .specialPowerTemplate = rule.specialPowerTemplate,
                    .specialPowerType = runtime.specialPowerType,
                    .objectTemplate = rule.specialObject,
                    .attachToBone = rule.specialObjectAttachToBone,
                    .position = runtime.targetPosition,
                    .hasPosition = runtime.hasTargetPosition,
                    .attachStickyBomb = specialAbilityUsesStickyBomb(
                        runtime.specialPowerType),
                    .ruleIndex = static_cast<uint32_t>(index),
                    .authoredOrder = rule.authoredOrder,
                    .activationSequence = runtime.activationSequence,
                    .submissionOrdinal = claimGameplayOrdinal(),
                    .confirmedTick = confirmedTick,
                });
            } else if (runtime.specialPowerType ==
                       game::SpecialPowerType::DisguiseAsVehicle) {
                effectRequests.push_back({
                    .kind = ObjectSpecialAbilityEffectKind::DisguiseAsTarget,
                    .source = candidate.id,
                    .target = runtime.target,
                    .specialPower = runtime.specialPower,
                    .specialPowerTemplate = rule.specialPowerTemplate,
                    .specialPowerType = runtime.specialPowerType,
                    .ruleIndex = static_cast<uint32_t>(index),
                    .authoredOrder = rule.authoredOrder,
                    .activationSequence = runtime.activationSequence,
                    .submissionOrdinal = claimGameplayOrdinal(),
                    .confirmedTick = confirmedTick,
                });
            }
            if (rule.awardExperienceForTriggering != 0) {
                effectRequests.push_back({
                    .kind = ObjectSpecialAbilityEffectKind::AwardExperience,
                    .source = candidate.id,
                    .specialPower = runtime.specialPower,
                    .specialPowerTemplate = rule.specialPowerTemplate,
                    .specialPowerType = runtime.specialPowerType,
                    .ruleIndex = static_cast<uint32_t>(index),
                    .authoredOrder = rule.authoredOrder,
                    .activationSequence = runtime.activationSequence,
                    .value = rule.awardExperienceForTriggering,
                    .submissionOrdinal = claimGameplayOrdinal(),
                    .confirmedTick = confirmedTick,
                });
            }
            const int32_t skillPoints = rule.skillPointsForTriggering >= 0
                ? rule.skillPointsForTriggering
                : rule.awardExperienceForTriggering;
            if (sourceOwner && skillPoints > 0) {
                const RankInfoCatalog* ranks = content.rankInfoCatalog();
                if (ranks) {
                    static_cast<void>(players.addSkillPoints(
                        sourceOwner->player, skillPoints, *ranks,
                        rankLevelLimit));
                }
            }

            if (rule.persistentPrepMilliseconds != 0) {
                if (rule.persistenceRequiresRecharge) {
                    effectRequests.push_back({
                        .kind = ObjectSpecialAbilityEffectKind::RestartRecharge,
                        .source = candidate.id,
                        .specialPower = runtime.specialPower,
                        .specialPowerTemplate = rule.specialPowerTemplate,
                        .specialPowerType = runtime.specialPowerType,
                        .ruleIndex = static_cast<uint32_t>(index),
                        .authoredOrder = rule.authoredOrder,
                        .activationSequence = runtime.activationSequence,
                        .submissionOrdinal = claimGameplayOrdinal(),
                        .confirmedTick = confirmedTick,
                    });
                }
                runtime.phaseEndTick = saturatingAdd(
                    confirmedTick, millisecondsToTicks(
                        rule.persistentPrepMilliseconds,
                        rules.logicFramesPerSecond));
                runtime.triggerTick = runtime.phaseEndTick;
                runtime.preTriggerRevealApplied = false;
            } else if (rule.packMilliseconds != 0 &&
                       !(runtime.noTargetCommand &&
                         rule.skipPackingWithNoTarget)) {
                runtime.fleeAfterPacking = runtime.hasTargetPosition;
                runtime.phase = ObjectSpecialAbilityPhase::Packing;
                runtime.phaseEndTick = saturatingAdd(
                    confirmedTick, millisecondsToTicks(
                        variedMilliseconds(rule.packMilliseconds,
                            rule.packUnpackVariationFactor, random),
                        rules.logicFramesPerSecond));
                clearSpecialAbilityPreparationConditions(
                    registry, candidate.entity, confirmedTick);
                setModelCondition(registry, candidate.entity, game::ModelConditionFlag::Packing,
                                  true, confirmedTick);
            } else {
                finishAbility(true, false);
            }
        }

        for (size_t index = 0; index < component.commandButtonHunts.size(); ++index) {
            auto& runtime = component.commandButtonHunts[index];
            const auto& rule = component.plan->commandButtonHunts[index];
            if (runtime.commandButton.empty()) continue;
            if (queue && queue->externalRevision !=
                    runtime.observedExternalRevision) {
                // RefCode stops this module forever after any player/script
                // command.  Do not resume merely because that command later
                // leaves an empty queue.
                runtime.commandButton.clear();
                continue;
            }
            const auto* button = content.findCommandButton(runtime.commandButton);
            if (!button || !commandSetContainsButton(
                    registry, content, candidate.entity,
                    runtime.commandButton)) {
                runtime.commandButton.clear();
                continue;
            }
            const auto* sourceTransform = ecs::try_get<TransformComponent>(registry, candidate.entity);
            const auto* sourceOwner = ecs::try_get<OwnerComponent>(registry, candidate.entity);
            if (!sourceTransform || !sourceOwner) continue;

            if (button->descriptor.kind ==
                    game::CommandButtonKind::FireWeapon ||
                button->descriptor.kind ==
                    game::CommandButtonKind::SwitchWeapon) {
                // CommandButtonHuntUpdate::huntWeapon delegates acquisition to
                // the ordinary Hunt/AttackPriority state.  It never scans a
                // nearest victim or fires the CommandButton at that victim.
                if (!queue) {
                    queue = &ecs::emplace<ObjectOrderQueueComponent>(
                        registry, candidate.entity);
                    runtime.observedExternalRevision =
                        queue->externalRevision;
                }
                if (queue->orders.empty()) {
                    insertSystemOrder(*queue, {
                        .kind = ObjectOrderKind::TacticalAttack,
                        .tacticalAttackSubtype =
                            ObjectTacticalAttackSubtype::Hunt,
                        .source = ObjectOrderSource::System,
                        .contextPlayer = sourceOwner->player,
                        .issuedTick = confirmedTick,
                        .sourceSequence = rule.authoredOrder,
                    }, ObjectOrderSystemPurpose::CommandButtonHunt,
                       rule.authoredOrder);
                }
                if (auto* weapons = ecs::try_get<ObjectWeaponComponent>(
                        registry, candidate.entity)) {
                    weapons->lockedSlot = buttonWeaponSlot(*button);
                    weapons->lockType = ObjectWeaponLockType::Temporary;
                }
                continue;
            }

            // Special-power and enter hunts scan only while AI is idle.
            if ((queue && !queue->orders.empty()) ||
                confirmedTick < runtime.nextScanTick) {
                continue;
            }
            runtime.nextScanTick = saturatingAdd(
                confirmedTick,
                std::max<uint64_t>(1, millisecondsToTicks(
                    rule.scanRateMilliseconds,
                    rules.logicFramesPerSecond)));

            const bool intentionalContact =
                button->descriptor.kind ==
                    game::CommandButtonKind::HijackVehicle ||
                button->descriptor.kind ==
                    game::CommandButtonKind::ConvertToCarBomb ||
                button->descriptor.kind ==
                    game::CommandButtonKind::SabotageBuilding;
            const bool specialPower = button->descriptor.kind ==
                game::CommandButtonKind::SpecialPower;
            if (!intentionalContact && !specialPower) {
                runtime.commandButton.clear();
                continue;
            }

            const SpecialPowerDefinition* specialDefinition = nullptr;
            if (specialPower) {
                specialDefinition = content.findSpecialPower(
                    button->specialPower);
                if (!specialDefinition ||
                    !specialPowerHuntIsReady(
                        registry, candidate.entity, specialDefinition->id,
                        confirmedTick)) {
                    continue;
                }
            }

            ObjectIntentionalContactKind contactKind =
                ObjectIntentionalContactKind::HijackVehicle;
            if (button->descriptor.kind ==
                    game::CommandButtonKind::ConvertToCarBomb) {
                contactKind = ObjectIntentionalContactKind::ConvertToCarBomb;
            } else if (button->descriptor.kind ==
                       game::CommandButtonKind::SabotageBuilding) {
                contactKind = ObjectIntentionalContactKind::SabotageBuilding;
            }
            const math::q32_32 rangeSquared = rule.scanRange * rule.scanRange;
            const LogicFixedVec3 sourcePosition =
                readAuthoritativeObjectPosition(
                    registry, candidate.entity, *sourceTransform);
            ObjectId best = INVALID_OBJECT_ID;
            math::q32_32 bestDistance = math::q32_32::from_raw(
                std::numeric_limits<int64_t>::max());
            int64_t bestEffectivePriority =
                std::numeric_limits<int64_t>::min();
            int32_t bestRawPriority = std::numeric_limits<int32_t>::min();
            const auto targetView = ecs::view<const ObjectIdentityComponent, const TransformComponent>(registry);
            for (const ecs::entity target : targetView) {
                const ObjectId targetId = targetView.template get<const ObjectIdentityComponent>(target).id;
                if (targetId == candidate.id || !alive(registry, lifecycle, targetId, target) ||
                    !sameMapStatus(registry, candidate.entity, target) ||
                    hiddenFromCommandButtonHunter(
                        registry, lifecycle, players, candidate.entity,
                        target)) {
                    continue;
                }
                const PlayerRelationship relationship =
                    relationshipBetweenObjects(
                        registry, players, candidate.entity, target);
                if (intentionalContact) {
                    const PlayerRelationship required =
                        contactKind ==
                                ObjectIntentionalContactKind::ConvertToCarBomb
                            ? PlayerRelationship::Neutral
                            : PlayerRelationship::Enemies;
                    if (relationship != required ||
                        !canObjectPerformIntentionalCrateContact(
                            registry, lifecycle, terrain, players,
                            candidate.id, targetId, contactKind)) {
                        continue;
                    }
                } else if (!specialPowerHuntTargetAllowed(
                               registry, lifecycle, players,
                               candidate.entity, target,
                               specialDefinition->specialPowerType,
                               confirmedTick)) {
                    continue;
                }
                const auto& targetTransform =
                    targetView.template get<const TransformComponent>(target);
                const LogicFixedVec3 targetPosition =
                    readAuthoritativeObjectPosition(
                        registry, target, targetTransform);
                const math::q32_32 dx{
                    targetPosition.x - sourcePosition.x};
                const math::q32_32 dy{
                    targetPosition.y - sourcePosition.y};
                const math::q32_32 distance = dx * dx + dy * dy;
                if (distance > rangeSquared) continue;
                if (specialPower) {
                    if (!seeThroughObstaclesLoaded) {
                        gatherObjectSeeThroughObstacles(
                            registry, seeThroughObstacles);
                        seeThroughObstaclesLoaded = true;
                    }
                    const game::SpecialPowerType powerType =
                        specialDefinition->specialPowerType;
                    if ((powerType ==
                             game::SpecialPowerType::TimedCharges ||
                         powerType ==
                             game::SpecialPowerType::TankHunterTntAttack) &&
                        targetNearFriendlyMine(
                            registry, lifecycle, candidate.entity, target,
                            specialDefinition->viewObjectRange)) {
                        continue;
                    }
                    const ObjectOrderIntent proposed{
                        .kind = ObjectOrderKind::SpecialPower,
                        .source = ObjectOrderSource::System,
                        .contextPlayer = sourceOwner->player,
                        .issuedTick = confirmedTick,
                        .sourceSequence = rule.authoredOrder,
                        .targetObject = targetId,
                        .targetX = targetPosition.x,
                        .targetY = targetPosition.y,
                        .targetZ = targetPosition.z,
                        .hasTargetPosition = true,
                        .contentName = runtime.commandButton,
                    };
                    if (admitSpecialAbility(
                            registry, lifecycle, candidate.id,
                            *specialDefinition, content, proposed, visibility,
                            navigation, rules.ai.attackUsesLineOfSight,
                            seeThroughObstacles).status ==
                        ObjectSpecialAbilityAdmissionStatus::Rejected) {
                        continue;
                    }
                }
                int32_t rawPriority = 1;
                int64_t effectivePriority = 1;
                if (!intentionalContact) {
                    rawPriority = targetPriority(candidate.id, target);
                    if (rawPriority == 0) continue;
                    const int64_t modifierRaw =
                        rules.ai.attackPriorityDistanceModifier.raw();
                    const int64_t distanceRaw =
                        math::q32_32::sqrt(distance).raw();
                    const int64_t penalty = modifierRaw > 0
                        ? distanceRaw / modifierRaw : 0;
                    effectivePriority = std::max<int64_t>(
                        1, static_cast<int64_t>(rawPriority) - penalty);
                }
                const bool better = intentionalContact
                    ? distance < bestDistance ||
                        (distance == bestDistance &&
                         (!best || targetId < best))
                    : effectivePriority > bestEffectivePriority ||
                        (effectivePriority == bestEffectivePriority &&
                         rawPriority > bestRawPriority) ||
                        (effectivePriority == bestEffectivePriority &&
                         rawPriority == bestRawPriority &&
                         (distance < bestDistance ||
                          (distance == bestDistance &&
                           (!best || targetId < best))));
                if (better) {
                    best = targetId;
                    bestDistance = distance;
                    bestEffectivePriority = effectivePriority;
                    bestRawPriority = rawPriority;
                }
            }
            if (!best) continue;
            if (!queue) queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, candidate.entity);
            const std::optional<ecs::entity> bestEntity =
                lifecycle.entityFromId(best);
            const TransformComponent* bestTransform = bestEntity
                ? ecs::try_get<TransformComponent>(registry, *bestEntity)
                : nullptr;
            if (!bestTransform) continue;
            const LogicFixedVec3 bestPosition =
                readAuthoritativeObjectPosition(
                    registry, *bestEntity, *bestTransform);
            if (intentionalContact) {
                insertSystemOrder(*queue, {
                    .kind = ObjectOrderKind::Move,
                    .source = ObjectOrderSource::System,
                    .contextPlayer = sourceOwner->player,
                    .issuedTick = confirmedTick,
                    .sourceSequence = rule.authoredOrder,
                    .targetObject = best,
                    .targetX = bestPosition.x,
                    .targetY = bestPosition.y,
                    .targetZ = bestPosition.z,
                    .hasTargetPosition = true,
                    .contentName = runtime.commandButton,
                }, ObjectOrderSystemPurpose::IntentionalContact,
                   static_cast<uint32_t>(contactKind) + 1u);
            } else {
                insertSystemOrder(*queue, {
                    .kind = ObjectOrderKind::SpecialPower,
                    .source = ObjectOrderSource::System,
                    .contextPlayer = sourceOwner->player,
                    .issuedTick = confirmedTick,
                    .sourceSequence = rule.authoredOrder,
                    .targetObject = best,
                    .targetX = bestPosition.x,
                    .targetY = bestPosition.y,
                    .targetZ = bestPosition.z,
                    .hasTargetPosition = true,
                    .contentName = runtime.commandButton,
                }, ObjectOrderSystemPurpose::CommandButtonHunt,
                   rule.authoredOrder);
            }
        }

        for (size_t index = 0; index < component.wander.size(); ++index) {
            if (!queue) queue = &ecs::emplace<ObjectOrderQueueComponent>(registry, candidate.entity);
            if (!queue->orders.empty()) continue;
            const auto* transform = ecs::try_get<TransformComponent>(registry, candidate.entity);
            const auto* owner = ecs::try_get<OwnerComponent>(registry, candidate.entity);
            if (!transform || !owner) continue;
            const int32_t offsetX = random ? random->integerInclusive(5, 50) : 5;
            const int32_t offsetY = random ? random->integerInclusive(5, 50) : 5;
            const LogicFixedVec3 position =
                readAuthoritativeObjectPosition(
                    registry, candidate.entity, *transform);
            const LogicFixedVec3 wanderTarget{
                position.x + math::q32_32{offsetX},
                position.y + math::q32_32{offsetY},
                position.z};
            insertSystemOrder(*queue, {.kind = ObjectOrderKind::Move,
                .source = ObjectOrderSource::System, .contextPlayer = owner->player,
                .issuedTick = confirmedTick,
                .sourceSequence = component.plan->wanderAuthoredOrders[index],
                .targetX = wanderTarget.x,
                .targetY = wanderTarget.y,
                .targetZ = wanderTarget.z,
                .hasTargetPosition = true,
                },
                ObjectOrderSystemPurpose::Wander,
                component.plan->wanderAuthoredOrders[index]);
        }
    }

    // Rebuild battle-plan projections after every provider transition.
    rebuildBattlePlanProjections(
        registry, lifecycle, &content, rules, random, confirmedTick);
}

container::Vector<ObjectBattlePlanPresentationEvent>
ObjectTacticalSystem::takeBattlePlanPresentationEvents() {
    container::Vector<ObjectBattlePlanPresentationEvent> result;
    result.swap(m_battlePlanPresentationEvents);
    return result;
}

container::Vector<ObjectTacticalPresentationEvent>
ObjectTacticalSystem::takeTacticalPresentationEvents() {
    container::Vector<ObjectTacticalPresentationEvent> result;
    result.swap(m_tacticalPresentationEvents);
    return result;
}

container::Vector<ObjectDeployStyleManualFrameEvent>
ObjectTacticalSystem::takeDeployStyleManualFrameEvents() {
    container::Vector<ObjectDeployStyleManualFrameEvent> result;
    result.swap(m_deployStyleManualFrameEvents);
    return result;
}

container::Vector<ObjectToppleFxEvent>
ObjectTacticalSystem::takeToppleFxEvents() {
    container::Vector<ObjectToppleFxEvent> result;
    result.swap(m_toppleFxEvents);
    return result;
}

container::Vector<ObjectToppleStumpSpawnRequest>
ObjectTacticalSystem::takeToppleStumpSpawnRequests() {
    container::Vector<ObjectToppleStumpSpawnRequest> result;
    result.swap(m_toppleStumpSpawnRequests);
    return result;
}

void ObjectTacticalSystem::drainToppleStumpSpawnRequests(
    container::Vector<ObjectToppleStumpSpawnRequest>& out) {
    out.clear();
    out.reserve(m_toppleStumpSpawnRequests.size());
    for (ObjectToppleStumpSpawnRequest& request :
         m_toppleStumpSpawnRequests) {
        out.push_back(std::move(request));
    }
    m_toppleStumpSpawnRequests.clear();
}

container::Vector<ObjectTopplePathfindRemovalRequest>
ObjectTacticalSystem::takeTopplePathfindRemovalRequests() {
    container::Vector<ObjectTopplePathfindRemovalRequest> result;
    result.swap(m_topplePathfindRemovalRequests);
    return result;
}

void ObjectTacticalSystem::drainTopplePathfindRemovalRequests(
    container::Vector<ObjectTopplePathfindRemovalRequest>& out) {
    out.clear();
    out.reserve(m_topplePathfindRemovalRequests.size());
    for (ObjectTopplePathfindRemovalRequest& request :
         m_topplePathfindRemovalRequests) {
        out.push_back(std::move(request));
    }
    m_topplePathfindRemovalRequests.clear();
}

void ObjectTacticalSystem::discardToppleGameplayRequests() noexcept {
    m_toppleStumpSpawnRequests.clear();
    m_topplePathfindRemovalRequests.clear();
}

void ObjectTacticalSystem::releaseToppleGameplayStorage() noexcept {
    container::Vector<ObjectToppleStumpSpawnRequest>{}.swap(
        m_toppleStumpSpawnRequests);
    container::Vector<ObjectTopplePathfindRemovalRequest>{}.swap(
        m_topplePathfindRemovalRequests);
}

} // namespace engine
