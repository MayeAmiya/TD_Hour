#include "core/container/container_types.h"
#include "game/object/definition/ObjectArchetype.h"
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

void populateTumbleLaunchRates(ObjectProjectileSpawnRequest& request,
                                const GameContentSnapshot& content,
                                SimulationRandom& random) {
    const auto projectile = content.findObjectArchetype(request.projectileTemplate);
    if (!projectile) return;
    // RefCode samples only if both the authored Dumb behavior asks for a
    // tumble and the spawned projectile has PhysicsBehavior. The strict
    // pitch -> yaw -> roll order is part of the session RNG stream.
    if (!projectile->physicsPlan || !projectile->projectilePlan ||
        !projectile->projectilePlan->tumbleRandomly) return;
    request.hasTumbleAngularRates = true;
    request.tumblePitchRate = random.fixedInclusive(
        -kFixedTumbleRateBoundPerLegacyFrame,
        kFixedTumbleRateBoundPerLegacyFrame) *
        kFixedLegacyLogicFramesPerSecond;
    request.tumbleYawRate = random.fixedInclusive(
        -kFixedTumbleRateBoundPerLegacyFrame,
        kFixedTumbleRateBoundPerLegacyFrame) *
        kFixedLegacyLogicFramesPerSecond;
    request.tumbleRollRate = random.fixedInclusive(
        -kFixedTumbleRateBoundPerLegacyFrame,
        kFixedTumbleRateBoundPerLegacyFrame) *
        kFixedLegacyLogicFramesPerSecond;
}

[[nodiscard]] bool containsKind(const ObjectKindOfComponent* kinds,
                                game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

void applyProjectileScatter(ObjectProjectileSpawnRequest& request,
                            const game::WeaponTemplate& weapon,
                            const ObjectKindOfComponent* targetKinds,
                            uint32_t targetPathfindLayer,
                            SimulationRandom& random,
                            container::Vector<uint32_t>* scatterTargetsUnused) {
    if (!weapon.scatterTargets.empty()) {
        size_t targetIndex = 0;
        bool hasPatternTarget = false;
        if (scatterTargetsUnused) {
            if (!scatterTargetsUnused->empty()) {
                const size_t pick = static_cast<size_t>(
                    random.integerInclusive(
                        0, static_cast<int32_t>(
                               scatterTargetsUnused->size() - 1)));
                targetIndex = (*scatterTargetsUnused)[pick];
                (*scatterTargetsUnused)[pick] =
                    scatterTargetsUnused->back();
                scatterTargetsUnused->pop_back();
                hasPatternTarget = targetIndex < weapon.scatterTargets.size();
            }
        } else {
            targetIndex = static_cast<size_t>(random.integerInclusive(
                0, static_cast<int32_t>(weapon.scatterTargets.size() - 1)));
            hasPatternTarget = true;
        }
        if (hasPatternTarget) {
            const game::WeaponScatterTarget& offset =
                weapon.scatterTargets[targetIndex];
            request.targetPosition.x +=
                offset.x * weapon.fixed.scatterTargetScalar;
            request.targetPosition.y +=
                offset.y * weapon.fixed.scatterTargetScalar;
            request.intendedTarget = INVALID_OBJECT_ID;
            request.hasIntendedTargetBasePosition = false;
            request.targetWasScattered = true;
            request.scatteredTargetPathfindLayer = targetPathfindLayer;
        }
    }

    Fixed radius = Fixed::max(kFixedZero, weapon.fixed.scatterRadius);
    if (containsKind(targetKinds, game::ObjectKindOf::Infantry)) {
        radius += Fixed::max(kFixedZero,
                             weapon.fixed.scatterRadiusVsInfantry);
    }
    if (radius <= kFixedZero) return;

    // WeaponTemplate::fireWeaponTemplate consumes radius then angle from the
    // session stream.  Preserve that order before DumbProjectile samples its
    // optional pitch/yaw/roll tumble rates.
    const Fixed sampledRadius = random.fixedInclusive(kFixedZero, radius);
    const Fixed angle = random.fixedInclusive(kFixedZero, kFixedFullTurn);
    if (sampledRadius <= kFixedZero) return;
    const math::q32_32_sincos direction = math::fixed_sincos(angle);
    request.targetPosition.x += sampledRadius * direction.cosine;
    request.targetPosition.y += sampledRadius * direction.sine;
    request.intendedTarget = INVALID_OBJECT_ID;
    request.hasIntendedTargetBasePosition = false;
    request.targetWasScattered = true;
    request.scatteredTargetPathfindLayer = targetPathfindLayer;
}

[[nodiscard]] bool matchesPreferredAgainst(const game::WeaponSlotProfile& slot,
                                           const ObjectKindOfComponent* kinds) noexcept {
    return kinds && slot.preferredAgainstKinds.any() &&
           kinds->mask.test_for_all(slot.preferredAgainstKinds);
}

void rebuildScatterTargets(
    ObjectWeaponSlotRuntime& slot,
    const game::WeaponTemplate& definition) {
    slot.scatterTargetsUnused.clear();
    slot.scatterTargetsUnused.reserve(definition.scatterTargets.size());
    for (size_t index = 0; index < definition.scatterTargets.size(); ++index) {
        slot.scatterTargetsUnused.push_back(static_cast<uint32_t>(index));
    }
}

[[nodiscard]] uint64_t millisecondsToFrames(uint32_t milliseconds,
                                             uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t numerator = static_cast<uint64_t>(milliseconds) * rate;
    // Legacy parseDuration values round a partial logic frame upward.  Avoid
    // floating point so a replay/client cannot disagree on the boundary.
    return (numerator + 999u) / 1000u;
}

[[nodiscard]] uint64_t fixedFloorToFrames(math::q32_32 value) noexcept {
    return value.raw() <= 0 ? 0 : static_cast<uint64_t>(value.raw() >> 32);
}

[[nodiscard]] uint64_t saturatingTickAdd(uint64_t tick, uint64_t delay) noexcept {
    return delay > std::numeric_limits<uint64_t>::max() - tick
        ? std::numeric_limits<uint64_t>::max()
        : tick + delay;
}

[[nodiscard]] uint64_t divideFramesByRateOfFire(
    uint64_t frames, const game::WeaponBonus& bonus) noexcept {
    if (frames == 0) return 0;
    constexpr uint64_t kMaximumFixedWholeFrames =
        static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    const uint64_t clampedFrames = std::min(frames, kMaximumFixedWholeFrames);
    const math::q32_32 multiplier =
        bonus.multiplier(game::WeaponBonusField::RateOfFire);
    if (multiplier <= math::q32_32{}) return clampedFrames;

    // floor(frames / (raw / 2^32)) can be evaluated directly in uint64_t.
    // Keeping frames in the positive Q32.32 whole-number range makes the
    // shifted numerator fit, and avoids MSVC _div128 quotient overflow for a
    // positive multiplier as small as one raw unit.
    const uint64_t numerator = clampedFrames << 32u;
    const uint64_t quotient = numerator / static_cast<uint64_t>(multiplier.raw());
    return std::min(quotient, kMaximumFixedWholeFrames);
}

[[nodiscard]] uint64_t multiplyFramesByPreAttack(
    uint64_t frames, const game::WeaponBonus& bonus) noexcept {
    if (frames == 0) return 0;
    const uint64_t clamped = std::min<uint64_t>(
        frames, static_cast<uint64_t>(std::numeric_limits<int32_t>::max()));
    return fixedFloorToFrames(
        bonus.scale(math::q32_32{static_cast<int32_t>(clamped)},
                    game::WeaponBonusField::PreAttack));
}

[[nodiscard]] uint64_t clipReloadFrames(const game::WeaponTemplate& definition,
                                        const game::WeaponBonus& bonus,
                                        uint32_t framesPerSecond) noexcept {
    return divideFramesByRateOfFire(
        millisecondsToFrames(definition.clipReloadTimeMilliseconds, framesPerSecond), bonus);
}

[[nodiscard]] uint64_t chooseShotDelayFrames(const game::WeaponTemplate& definition,
                                              uint32_t framesPerSecond,
                                              SimulationRandom& random,
                                              const game::WeaponBonus& bonus) noexcept {
    const uint64_t minimum = millisecondsToFrames(
        definition.minimumDelayBetweenShotsMilliseconds, framesPerSecond);
    const uint64_t maximum = std::max(minimum, millisecondsToFrames(
        definition.maximumDelayBetweenShotsMilliseconds, framesPerSecond));
    if (minimum >= maximum) return divideFramesByRateOfFire(minimum, bonus);
    const uint64_t clampedMaximum = std::min<uint64_t>(
        maximum, static_cast<uint64_t>(std::numeric_limits<int32_t>::max()));
    const uint64_t clampedMinimum = std::min(minimum, clampedMaximum);
    const uint64_t sampled = static_cast<uint64_t>(random.integerInclusive(
        static_cast<int32_t>(clampedMinimum), static_cast<int32_t>(clampedMaximum)));
    return divideFramesByRateOfFire(sampled, bonus);
}

[[nodiscard]] bool isReloading(const ObjectWeaponSlotRuntime& slot,
                               uint64_t tick) noexcept {
    return slot.reloadCompleteTick != 0 && tick < slot.reloadCompleteTick;
}

[[nodiscard]] bool hasFiniteEmptyClip(const ObjectWeaponSlotRuntime& slot,
                                      const game::WeaponTemplate& definition) noexcept {
    return definition.clipSize > 0 && slot.ammoInClip == 0;
}

void advanceSlot(ObjectWeaponSlotRuntime& slot, const game::WeaponTemplate& definition,
                 uint64_t tick) noexcept {
    if (slot.reloadCompleteTick == 0 || tick < slot.reloadCompleteTick) {
        return;
    }
    slot.reloadCompleteTick = 0;
    const bool replenish = slot.reloadReplenishesClip;
    slot.reloadReplenishesClip = false;
    if (replenish && definition.reloadType == game::WeaponReloadType::Auto && definition.clipSize > 0) {
        slot.ammoInClip = static_cast<uint32_t>(definition.clipSize);
        rebuildScatterTargets(slot, definition);
        ++slot.clipGeneration;
        if (slot.clipGeneration == 0) slot.clipGeneration = 1;
    }
}

void advanceWeaponSet(ObjectWeaponSetRuntime& set, const GameContentSnapshot& content,
                      uint64_t tick) noexcept {
    if (set.sharedReloadCompleteTick != 0 && tick >= set.sharedReloadCompleteTick) {
        set.sharedReloadCompleteTick = 0;
    }
    for (ObjectWeaponSlotRuntime& slot : set.slots) {
        if (const game::WeaponTemplate* definition = content.findWeapon(slot.content)) {
            advanceSlot(slot, *definition, tick);
        }
    }
}

void resetWeaponSet(ObjectWeaponSetRuntime& set,
                    const GameContentSnapshot& content,
                    uint32_t logicFramesPerSecond,
                    uint64_t confirmedTick) noexcept {
    set.sharedReloadCompleteTick = 0;
    for (ObjectWeaponSlotRuntime& slot : set.slots) {
        const game::WeaponContentId contentId = slot.content;
        slot = {};
        slot.content = contentId;
        slot.clipGeneration = 1;
        if (const game::WeaponTemplate* definition = content.findWeapon(contentId);
            definition) {
            rebuildScatterTargets(slot, *definition);
            slot.suspendFxUntilTick = saturatingTickAdd(
                confirmedTick,
                millisecondsToFrames(
                    definition->suspendFxDelayMilliseconds,
                    logicFramesPerSecond));
            if (definition->clipSize > 0) {
                slot.ammoInClip = static_cast<uint32_t>(definition->clipSize);
            }
        }
    }
}

[[nodiscard]] bool releaseWeaponLock(ObjectWeaponComponent& weapons,
                                     ObjectWeaponLockType type) noexcept {
    if (type == ObjectWeaponLockType::None ||
        weapons.lockType == ObjectWeaponLockType::None) {
        return false;
    }
    if (type == ObjectWeaponLockType::Temporary &&
        weapons.lockType == ObjectWeaponLockType::Permanent) {
        return false;
    }
    weapons.lockedSlot.reset();
    weapons.lockType = ObjectWeaponLockType::None;
    return true;
}

[[nodiscard]] bool activateWeaponSetRuntime(
    ObjectWeaponComponent& weapons, size_t setIndex,
    const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick) noexcept {
    if (setIndex >= weapons.sets.size()) return false;
    ObjectWeaponSetRuntime& runtimeSet = weapons.sets[setIndex];
    const bool changed = !weapons.activeWeaponSetIndex ||
        *weapons.activeWeaponSetIndex != setIndex;
    if (!changed) return false;

    // WeaponSet::updateWeaponSet destroys/recreates the selected Weapon
    // instances. A lock survives only when the incoming set explicitly opts
    // into sharing it across sets.
    if (weapons.activeWeaponSetIndex &&
        !runtimeSet.weaponLockSharedAcrossSets) {
        static_cast<void>(releaseWeaponLock(
            weapons, ObjectWeaponLockType::Permanent));
        weapons.currentSlot = game::WeaponSlot::Primary;
    }
    resetWeaponSet(runtimeSet, content, logicFramesPerSecond, confirmedTick);
    ++weapons.weaponSetGeneration;
    if (weapons.weaponSetGeneration == 0) ++weapons.weaponSetGeneration;
    weapons.activeWeaponSetIndex = static_cast<uint32_t>(setIndex);
    return true;
}

} // namespace engine::object_combat_detail

namespace engine {

game::WeaponFxPolicy resolveObjectWeaponFxPolicy(
    const ecs::registry& registry, ecs::entity sourceEntity,
    const ObjectLifecycle* lifecycle,
    const PlayerRegistry* players, const game::WeaponTemplate& weapon,
    bool suspendedByDelay) noexcept {
    if (suspendedByDelay) {
        return game::WeaponFxPolicy::SuppressedBySuspendDelay;
    }
    if (weapon.playFxWhenStealthed || sourceEntity == ecs::null ||
        !registry.valid(sourceEntity)) {
        return game::WeaponFxPolicy::Play;
    }

    ecs::entity visibilityEntity = sourceEntity;
    if (lifecycle) {
        // Object::isLogicallyVisible queries getOuterObject(). Follow the
        // typed containment chain without retaining an Object pointer.
        for (uint32_t depth = 0; depth < 8; ++depth) {
            const ObjectContainedByComponent* contained =
                ecs::try_get<ObjectContainedByComponent>(
                    registry, visibilityEntity);
            if (!contained || !contained->container) break;
            const std::optional<ecs::entity> outer =
                lifecycle->entityFromIdIncludingPending(
                    contained->container);
            if (!outer || *outer == visibilityEntity) break;
            visibilityEntity = *outer;
        }
    }
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, visibilityEntity);
    const bool hiddenStealth = status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::Stealthed)) &&
        !status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Detected));
    if (!hiddenStealth) return game::WeaponFxPolicy::Play;

    const ObjectKindOfComponent* visibilityKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, visibilityEntity);
    const ObjectKindOfComponent* sourceKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, sourceEntity);
    // RefCode treats disguisers as logically visible and mines as an explicit
    // compatibility exception even when the weapon did not opt in.
    if (object_combat_detail::containsKind(
            visibilityKinds, game::ObjectKindOf::Disguiser) ||
        object_combat_detail::containsKind(
            sourceKinds, game::ObjectKindOf::Mine)) {
        return game::WeaponFxPolicy::Play;
    }
    if (!players) return game::WeaponFxPolicy::Play;
    const PlayerState* viewer = players->localPlayer();
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, sourceEntity);
    if (!viewer || viewer->life != PlayerLifeState::Active || !owner ||
        players->relationship(viewer->id, owner->player) ==
            PlayerRelationship::Allies) {
        return game::WeaponFxPolicy::Play;
    }
    return game::WeaponFxPolicy::SuppressedByStealth;
}

} // namespace engine
