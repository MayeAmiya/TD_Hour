#include "game/object/simulation/status/ObjectStealth.h"

#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/combat/ObjectWeaponBonusUpdate.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace engine {

bool objectHiddenFromObserverForAcquisition(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ecs::entity observer,
    ecs::entity target) noexcept {
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, target);
    if (status && status->hasAny(game::objectStatusBit(
                      game::ObjectStatusFlag::Stealthed)) &&
        !status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::Detected))) {
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, target);
        const bool disguiser = kinds && game::objectHasKind(
            kinds->mask, game::ObjectKindOf::Disguiser);
        if (!disguiser) return true;
        if (!status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::Disguised))) {
            return false;
        }
        const ObjectDisguiseComponent* disguise =
            ecs::try_get<ObjectDisguiseComponent>(registry, target);
        const OwnerComponent* observerOwner =
            ecs::try_get<OwnerComponent>(registry, observer);
        if (!disguise || !observerOwner ||
            disguise->apparentPlayer == INVALID_PLAYER_ID) {
            return false;
        }
        return players.relationship(
                   observerOwner->player, disguise->apparentPlayer) !=
            PlayerRelationship::Enemies;
    }

    const ObjectContainmentComponent* contents =
        ecs::try_get<ObjectContainmentComponent>(registry, target);
    if (!contents || contents->objects.empty()) return false;
    std::optional<ecs::entity> firstOccupant;
    for (const ObjectContainedObjectRecord& record : contents->objects) {
        const std::optional<ecs::entity> occupant =
            lifecycle.entityFromId(record.object);
        const ObjectStatusComponent* occupantStatus = occupant
            ? ecs::try_get<ObjectStatusComponent>(registry, *occupant)
            : nullptr;
        if (!occupant || !occupantStatus ||
            !occupantStatus->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::Stealthed))) {
            return false;
        }
        if (!firstOccupant) firstOccupant = occupant;
    }
    const ObjectStatusComponent* firstStatus = firstOccupant
        ? ecs::try_get<ObjectStatusComponent>(registry, *firstOccupant)
        : nullptr;
    return firstOccupant && firstStatus &&
        !firstStatus->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::Detected)) &&
        relationshipBetweenObjects(
            registry, players, observer, *firstOccupant) ==
            PlayerRelationship::Enemies;
}

namespace {

using container::asciiEqualIgnoreCase;

[[nodiscard]] uint64_t millisecondsToFrames(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == std::numeric_limits<uint32_t>::max()) {
        // Constructor sentinel lives in already-converted frame units in the
        // source module; preserve its practical "never automatically arm"
        // meaning instead of treating it as only ~49 days of milliseconds.
        return std::numeric_limits<uint32_t>::max();
    }
    if (milliseconds == 0 || framesPerSecond == 0) return 0;
    const uint64_t product = static_cast<uint64_t>(milliseconds) *
        framesPerSecond;
    return (product + 999u) / 1000u;
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t value,
                                     uint64_t delta) noexcept {
    return delta > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max() : value + delta;
}

[[nodiscard]] uint64_t mixDetectorSeed(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] bool hasKind(
    const ObjectKindOfComponent* kinds,
    game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] bool matchesAnyKind(
    const ObjectKindOfComponent* kinds,
    const game::ObjectKindOfMask& filters) noexcept {
    return kinds && kinds->mask.test_for_any(filters);
}

[[nodiscard]] bool mapStatusMatches(
    const ecs::registry& registry, ecs::entity left,
    ecs::entity right) noexcept {
    const ObjectMapStatusComponent* leftStatus =
        ecs::try_get<ObjectMapStatusComponent>(registry, left);
    const ObjectMapStatusComponent* rightStatus =
        ecs::try_get<ObjectMapStatusComponent>(registry, right);
    return (leftStatus && leftStatus->offMap) ==
           (rightStatus && rightStatus->offMap);
}

[[nodiscard]] bool isGarrisonableContainer(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ThingTemplateComponent* source =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!source || !source->archetype ||
        !source->archetype->containmentPlan) return false;
    return (source->archetype->containmentPlan->kindMask &
            objectContainmentKindBit(ObjectContainmentKind::Garrison)) != 0;
}

[[nodiscard]] bool hasContainmentKind(
    const ecs::registry& registry, ecs::entity entity,
    ObjectContainmentKind kind) noexcept {
    const ThingTemplateComponent* source =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    return source && source->archetype &&
        source->archetype->containmentPlan &&
        (source->archetype->containmentPlan->kindMask &
         objectContainmentKindBit(kind)) != 0;
}

[[nodiscard]] bool effectivelyDead(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    return health && health->effectivelyDead;
}

[[nodiscard]] LogicFixedVec3 authoritativePosition(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, entity);
    return transform
        ? readAuthoritativeObjectPosition(registry, entity, *transform)
        : LogicFixedVec3{};
}

[[nodiscard]] bool withinPlanarRange(
    const LogicFixedVec3& left, const LogicFixedVec3& right,
    math::q32_32 range) noexcept {
    const math::q32_32 deltaX = left.x - right.x;
    const math::q32_32 deltaY = left.y - right.y;
    return deltaX * deltaX + deltaY * deltaY <= range * range;
}

struct StealthRuleOwner final {
    ecs::entity entity = ecs::null;
    ObjectStealthComponent* stealth = nullptr;
};

[[nodiscard]] StealthRuleOwner resolveStealthRuleOwner(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity self, const ObjectStealthComponent& selfStealth) noexcept {
    if (!selfStealth.plan || !selfStealth.plan->useRiderStealth) {
        return {self, ecs::try_get<ObjectStealthComponent>(registry, self)};
    }
    const ObjectContainmentComponent* containment =
        ecs::try_get<ObjectContainmentComponent>(registry, self);
    if (!containment) {
        return {self, ecs::try_get<ObjectStealthComponent>(registry, self)};
    }
    // ObjectContainmentComponent owns an ObjectId-sorted roster.  Ignore
    // stale structural edges, but otherwise preserve RefCode's first-rider
    // rule instead of selecting an arbitrary EnTT storage element.
    for (const ObjectContainedObjectRecord& record : containment->objects) {
        const std::optional<ecs::entity> rider =
            lifecycle.entityFromId(record.object);
        if (!rider) continue;
        return {*rider,
                ecs::try_get<ObjectStealthComponent>(registry, *rider)};
    }
    return {self, ecs::try_get<ObjectStealthComponent>(registry, self)};
}

[[nodiscard]] uint32_t effectiveStealthDelayMilliseconds(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity self, const ObjectStealthComponent& selfStealth) noexcept {
    const StealthRuleOwner owner = resolveStealthRuleOwner(
        registry, lifecycle, self, selfStealth);
    return owner.entity != self && owner.stealth && owner.stealth->plan
        ? owner.stealth->plan->stealthDelayMilliseconds
        : selfStealth.plan->stealthDelayMilliseconds;
}

[[nodiscard]] bool effectiveRevealOrderEnabled(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity self, const ObjectStealthComponent& selfStealth) noexcept {
    const StealthRuleOwner owner = resolveStealthRuleOwner(
        registry, lifecycle, self, selfStealth);
    return owner.entity != self && owner.stealth && owner.stealth->plan
        ? owner.stealth->plan->orderIdleEnemiesToAttackOnReveal
        : selfStealth.plan->orderIdleEnemiesToAttackOnReveal;
}

[[nodiscard]] uint64_t transitionMidpoint(
    uint64_t start, uint64_t duration) noexcept {
    return saturatingAdd(start, duration / 2u);
}

// RefCode StealthUpdate sets OBJECT_STATUS_DISGUISED and
// MODELCONDITION_DISGUISED on adjacent lines (:1037-1038) and clears them
// together (:1107-1108). Without the model condition a Bomb Truck disguised as
// an Avenger renders the Avenger mesh with TURRET01 still hidden, because the
// authored DISGUISED ConditionState is never selected.
void publishDisguiseModelCondition(
    ecs::registry& registry, ecs::entity entity, bool disguised,
    uint64_t confirmedTick) {
    const game::ModelConditionMask disguisedCondition =
        game::modelConditionMaskOf(game::ModelConditionFlag::Disguised);
    publishObjectModelConditionContribution(
        registry, entity, ObjectModelConditionContributionSource::Stealth,
        disguised ? game::ModelConditionMask{} : disguisedCondition,
        disguised ? disguisedCondition : game::ModelConditionMask{},
        confirmedTick);
}

void advanceDisguise(
    ecs::registry& registry, ecs::entity entity,
    ObjectStealthComponent& stealth, ObjectDisguiseComponent& disguise,
    ObjectId object,
    container::Vector<ObjectDisguisePresentationEvent>& presentationEvents,
    uint64_t confirmedTick) {
    const game::ObjectStatusMask disguisedStatus =
        game::objectStatusBit(game::ObjectStatusFlag::Disguised);
    if (disguise.phase == ObjectDisguisePhase::Disguising &&
        confirmedTick >= disguise.transitionMidpointTick &&
        !disguise.transitionMidpointApplied) {
        disguise.apparentTemplateName = disguise.requestedTemplateName;
        disguise.apparentPlayer = disguise.requestedPlayer;
        disguise.transitionMidpointApplied = true;
        static_cast<void>(ObjectStatusSystem::apply(
            registry, entity,
            {.setMask = disguisedStatus, .confirmedTick = confirmedTick}));
        publishDisguiseModelCondition(registry, entity, true, confirmedTick);
        // StealthUpdate changes the visual disguise and plays this sound in
        // the same transition.  Keep the confirmed edge together with the
        // existing FX event; presentation only resolves the authored cue.
        presentationEvents.push_back({
            .kind = ObjectDisguisePresentationEventKind::DisguiseStarted,
            .object = object,
            .fxList = stealth.plan ? stealth.plan->disguiseFx
                                   : container::String{},
            .confirmedTick = confirmedTick,
        });
    } else if (disguise.phase == ObjectDisguisePhase::Revealing &&
               confirmedTick >= disguise.transitionMidpointTick &&
               !disguise.transitionMidpointApplied) {
        disguise.apparentTemplateName.clear();
        disguise.apparentPlayer = INVALID_PLAYER_ID;
        disguise.transitionMidpointApplied = true;
        static_cast<void>(ObjectStatusSystem::apply(
            registry, entity,
            {.clearMask = disguisedStatus, .confirmedTick = confirmedTick}));
        publishDisguiseModelCondition(registry, entity, false, confirmedTick);
        // RefCode selects the reveal cue from AIUpdateInterface's current
        // victim at the instant the visual disguise is removed.  Combat owns
        // that equivalent fact here, so do not infer success from FX or UI.
        const ObjectWeaponComponent* weapons =
            ecs::try_get<ObjectWeaponComponent>(registry, entity);
        presentationEvents.push_back({
            .kind = weapons && weapons->target
                ? ObjectDisguisePresentationEventKind::
                    DisguiseRevealedSuccess
                : ObjectDisguisePresentationEventKind::
                    DisguiseRevealedFailure,
            .object = object,
            .fxList = stealth.plan ? stealth.plan->disguiseRevealFx
                                   : container::String{},
            .confirmedTick = confirmedTick,
        });
    }

    if (confirmedTick < disguise.transitionCompleteTick) return;
    if (disguise.phase == ObjectDisguisePhase::Disguising) {
        disguise.phase = ObjectDisguisePhase::Disguised;
    } else if (disguise.phase == ObjectDisguisePhase::Revealing) {
        disguise = {};
        stealth.enabled = false;
        stealth.detectionExpiresTick = 0;
        stealth.stealthAllowedTick = std::numeric_limits<uint64_t>::max();
        static_cast<void>(ObjectStatusSystem::apply(
            registry, entity,
            {.clearMask = disguisedStatus |
                          game::objectStatusBit(
                              game::ObjectStatusFlag::Stealthed) |
                          game::objectStatusBit(
                              game::ObjectStatusFlag::Detected),
             .confirmedTick = confirmedTick}));
        publishDisguiseModelCondition(registry, entity, false, confirmedTick);
    }
}

[[nodiscard]] bool playerHasBlackMarket(
    const ecs::registry& registry, PlayerId player) {
    const auto view = ecs::view<
        const OwnerComponent, const ObjectKindOfComponent,
        const ObjectHealthComponent>(registry);
    for (const ecs::entity entity : view) {
        if (view.template get<const OwnerComponent>(entity).player != player ||
            !hasKind(&view.template get<const ObjectKindOfComponent>(entity),
                     game::ObjectKindOf::FsBlackMarket)) {
            continue;
        }
        const ObjectHealthComponent& health =
            view.template get<const ObjectHealthComponent>(entity);
        if (health.effectivelyDead) continue;
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, entity);
        const game::ObjectStatusMask unavailable =
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
            game::objectStatusBit(game::ObjectStatusFlag::Sold) |
            game::objectStatusBit(game::ObjectStatusFlag::Destroyed);
        if (!status || !status->hasAny(unavailable)) return true;
    }
    return false;
}

[[nodiscard]] bool slotFiredRecently(
    const ObjectWeaponComponent* weapons, size_t slot,
    uint64_t confirmedTick) noexcept {
    if (!weapons || slot >= game::kWeaponSlotCount) return false;
    for (const ObjectWeaponSetRuntime& set : weapons->sets) {
        const ObjectWeaponSlotRuntime& runtime = set.slots[slot];
        if (runtime.lastFireSequence == 0) continue;
        if (runtime.lastFireTick == confirmedTick ||
            (confirmedTick != 0 &&
             runtime.lastFireTick == confirmedTick - 1u)) return true;
    }
    return false;
}

[[nodiscard]] bool allowedToStealth(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ecs::entity entity,
    ObjectStealthComponent& runtime,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    uint32_t relationDepth = 0) {
    const game::ObjectStealthPlan& plan = *runtime.plan;
    const StealthRuleOwner ruleOwner = resolveStealthRuleOwner(
        registry, lifecycle, entity, runtime);
    const game::ObjectStealthPlan& forbiddenPlan =
        ruleOwner.entity != entity && ruleOwner.stealth &&
                ruleOwner.stealth->plan
            ? *ruleOwner.stealth->plan
            : plan;
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    const ObjectStatusComponent* stealthOwnerStatus =
        ecs::try_get<ObjectStatusComponent>(registry, ruleOwner.entity);
    const game::ObjectStatusMask flags = status ? status->flags : 0;
    const game::ObjectStatusMask ownerFlags = stealthOwnerStatus
        ? stealthOwnerStatus->flags : 0;
    if ((ownerFlags & game::objectStatusBit(
            game::ObjectStatusFlag::CanStealth)) == 0 ||
        (flags & game::objectStatusBit(
            game::ObjectStatusFlag::ScriptUnstealthed)) != 0 ||
        (flags & plan.requiredStatuses) != plan.requiredStatuses ||
        (flags & plan.forbiddenStatuses) != 0) return false;

    const auto forbidden = [&](game::ObjectStealthForbiddenCondition value) {
        return (forbiddenPlan.forbiddenConditions &
                game::objectStealthForbiddenBit(value)) != 0;
    };
    if (forbidden(game::ObjectStealthForbiddenCondition::Attacking) &&
        (flags & game::objectStatusBit(
            game::ObjectStatusFlag::IsFiringWeapon)) != 0) return false;
    if (forbidden(game::ObjectStealthForbiddenCondition::UsingAbility) &&
        (flags & game::objectStatusBit(
            game::ObjectStatusFlag::IsUsingAbility)) != 0) return false;
    if (forbidden(game::ObjectStealthForbiddenCondition::Moving)) {
        const ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(registry, entity);
        if (physics) {
            const LogicFixedVec3& velocity =
                physics->velocityUnitsPerSecond;
            const math::q32_32 speedSquared =
                velocity.x * velocity.x + velocity.y * velocity.y +
                velocity.z * velocity.z;
            if (speedSquared > plan.moveThresholdUnitsPerSecond *
                                   plan.moveThresholdUnitsPerSecond) {
                return false;
            }
        }
    }
    if (forbidden(game::ObjectStealthForbiddenCondition::TakingDamage)) {
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        if (health && health->lastDamageTick != 0 &&
            health->lastDamageType != game::DamageType::HEALING &&
            (health->lastDamageTick == confirmedTick ||
             (confirmedTick != 0 &&
              health->lastDamageTick == confirmedTick - 1u))) {
            return false;
        }
    }
    const ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(registry, entity);
    const bool primary = forbidden(
        game::ObjectStealthForbiddenCondition::FiringPrimary);
    const bool secondary = forbidden(
        game::ObjectStealthForbiddenCondition::FiringSecondary);
    const bool tertiary = forbidden(
        game::ObjectStealthForbiddenCondition::FiringTertiary);
    if (primary && secondary && tertiary &&
        (flags & game::objectStatusBit(
            game::ObjectStatusFlag::IsFiringWeapon)) != 0) {
        return false;
    }
    if ((primary && slotFiredRecently(weapons, 0, confirmedTick)) ||
        (secondary && slotFiredRecently(weapons, 1, confirmedTick)) ||
        (tertiary && slotFiredRecently(weapons, 2, confirmedTick))) {
        return false;
    }

    // SPAWNS_ARE_THE_WEAPONS is one stealth unit in RefCode: StealthUpgrade
    // grants CAN_STEALTH to every weapon child, and the host may cloak only
    // while every live child is itself allowed to cloak. A single failing
    // child reveals the complete roster immediately. Resolve this through
    // the stable SpawnBehavior roster rather than render subobjects or the
    // incidental producer index.
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, entity);
    if (hasKind(kinds, game::ObjectKindOf::SpawnsAreTheWeapons)) {
        if (relationDepth >= 64u) return false;
        const ObjectIdentityComponent* identity =
            ecs::try_get<ObjectIdentityComponent>(registry, entity);
        if (!identity || !identity->id) return false;
        const container::Vector<ObjectId> children =
            ObjectSpawnSlaveSystem{}.spawnChildren(
                registry, lifecycle, identity->id);
        bool allChildrenAllowStealth = true;
        for (const ObjectId child : children) {
            const std::optional<ecs::entity> childEntity =
                lifecycle.entityFromId(child);
            ObjectStealthComponent* childStealth = childEntity
                ? ecs::try_get<ObjectStealthComponent>(registry,
                                                        *childEntity)
                : nullptr;
            if (!childEntity || !childStealth || !childStealth->plan) {
                allChildrenAllowStealth = false;
                break;
            }
            static_cast<void>(ObjectStatusSystem::apply(
                registry, *childEntity,
                {.setMask = game::objectStatusBit(
                     game::ObjectStatusFlag::CanStealth),
                 .confirmedTick = confirmedTick}));
            if (!allowedToStealth(
                    registry, lifecycle, *childEntity, *childStealth,
                    rules, confirmedTick, relationDepth + 1u)) {
                allChildrenAllowStealth = false;
                break;
            }
        }
        if (!allChildrenAllowStealth) {
            for (const ObjectId child : children) {
                const std::optional<ecs::entity> childEntity =
                    lifecycle.entityFromIdIncludingPending(child);
                if (!childEntity) continue;
                const bool detected = ObjectStealthSystem{}.markDetected(
                    registry, lifecycle, child, 0u, rules,
                    confirmedTick);
                if (!detected) continue;
                static_cast<void>(ObjectStatusSystem::apply(
                    registry, *childEntity,
                    {.setMask = game::objectStatusBit(
                         game::ObjectStatusFlag::Detected),
                     .clearMask = game::objectStatusBit(
                         game::ObjectStatusFlag::Stealthed),
                     .confirmedTick = confirmedTick}));
            }
            return false;
        }
    }

    if (forbidden(game::ObjectStealthForbiddenCondition::NoBlackMarket)) {
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, entity);
        if (!owner || !owner->player) return false;
        if (confirmedTick >= runtime.nextBlackMarketCheckTick) {
            runtime.blackMarketAvailable =
                playerHasBlackMarket(registry, owner->player);
            const uint64_t interval = std::max<uint64_t>(
                1u, millisecondsToFrames(
                    plan.blackMarketCheckMilliseconds,
                    rules.logicFramesPerSecond));
            const ObjectIdentityComponent* identity =
                ecs::try_get<ObjectIdentityComponent>(registry, entity);
            const uint64_t stagger = identity && identity->id
                ? identity->id.value % 11u : 0u;
            runtime.nextBlackMarketCheckTick = saturatingAdd(
                confirmedTick, interval + stagger);
        }
        if (!runtime.blackMarketAvailable) return false;
    }
    if (forbidden(game::ObjectStealthForbiddenCondition::RidersAttacking)) {
        const ObjectContainmentComponent* containment =
            ecs::try_get<ObjectContainmentComponent>(registry, entity);
        if (containment && containment->passengersAllowedToFire) {
            for (const ObjectContainedObjectRecord& record :
                 containment->objects) {
                const std::optional<ecs::entity> rider =
                    lifecycle.entityFromId(record.object);
                const ObjectStatusComponent* riderStatus = rider
                    ? ecs::try_get<ObjectStatusComponent>(registry, *rider)
                    : nullptr;
                if (riderStatus && riderStatus->hasAny(
                        game::objectStatusBit(
                            game::ObjectStatusFlag::IsAttacking))) {
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace

void ObjectStealthSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const ObjectSimulationRules& rules, uint64_t sessionSeed,
    uint64_t confirmedTick) const {
    const ThingTemplateComponent* source =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectStealthPlan> plan =
        source && source->archetype ? source->archetype->stealthPlan
                                    : container::SharedPtr<const game::ObjectStealthPlan>{};
    if (plan) {
        ObjectStealthComponent component;
        component.plan = plan;
        component.enabled = !plan->disguisesAsTeam &&
            !plan->grantedBySpecialPower;
        component.stealthAllowedTick = saturatingAdd(
            confirmedTick, millisecondsToFrames(
                plan->stealthDelayMilliseconds,
                rules.logicFramesPerSecond));
        const ObjectIdentityComponent* identity =
            ecs::try_get<ObjectIdentityComponent>(registry, entity);
        uint64_t pulseSeed = mixDetectorSeed(sessionSeed);
        pulseSeed ^= mixDetectorSeed(identity && identity->id
            ? identity->id.value : 0u);
        constexpr int64_t kPiRaw = 13493037705ll;
        const uint64_t phase24 = (mixDetectorSeed(pulseSeed) >> 40u) &
                                 0x00ffffffu;
        component.friendlyPulsePhaseRadians = math::q32_32::from_raw(
            static_cast<int64_t>((phase24 * kPiRaw) >> 24u));
        if (ObjectStealthComponent* existing =
                ecs::try_get<ObjectStealthComponent>(registry, entity)) {
            *existing = std::move(component);
        } else {
            ecs::emplace<ObjectStealthComponent>(registry, entity,
                                                 std::move(component));
        }
        if (plan->disguisesAsTeam) {
            if (ObjectDisguiseComponent* existing =
                    ecs::try_get<ObjectDisguiseComponent>(registry,
                                                           entity)) {
                *existing = {};
            } else {
                ecs::emplace<ObjectDisguiseComponent>(registry, entity);
            }
        }
        if (plan->innateStealth) {
            static_cast<void>(ObjectStatusSystem::apply(
                registry, entity,
                {.setMask = game::objectStatusBit(
                     game::ObjectStatusFlag::CanStealth),
                 .confirmedTick = confirmedTick}));
        }
    }

    const container::SharedPtr<const game::ObjectStealthDetectorPlan>
        detectorPlan = source && source->archetype
            ? source->archetype->stealthDetectorPlan
            : container::SharedPtr<const game::ObjectStealthDetectorPlan>{};
    if (detectorPlan) {
        ObjectStealthDetectorComponent detector;
        detector.plan = detectorPlan;
        detector.enabled = !detectorPlan->initiallyDisabled;
        if (detector.enabled) {
            const uint64_t interval = std::max<uint64_t>(
                1u, millisecondsToFrames(
                    detectorPlan->detectionRateMilliseconds,
                    rules.logicFramesPerSecond));
            const ObjectIdentityComponent* identity =
                ecs::try_get<ObjectIdentityComponent>(registry, entity);
            uint64_t key = mixDetectorSeed(sessionSeed);
            key ^= mixDetectorSeed(identity && identity->id
                ? identity->id.value : 0u);
            key ^= mixDetectorSeed(detectorPlan->authoredOrder);
            const uint64_t jitter = 1u + mixDetectorSeed(key) % interval;
            detector.nextScanTick = saturatingAdd(confirmedTick, jitter);
        } else {
            detector.nextScanTick = std::numeric_limits<uint64_t>::max();
        }
        if (ObjectStealthDetectorComponent* existing =
                ecs::try_get<ObjectStealthDetectorComponent>(registry,
                                                              entity)) {
            *existing = std::move(detector);
        } else {
            ecs::emplace<ObjectStealthDetectorComponent>(
                registry, entity, std::move(detector));
        }
    }

    const container::SharedPtr<const game::ObjectGrantStealthPlan>
        grantPlan = source && source->archetype
            ? source->archetype->grantStealthPlan
            : container::SharedPtr<const game::ObjectGrantStealthPlan>{};
    if (grantPlan) {
        ObjectGrantStealthComponent grant;
        grant.plan = grantPlan;
        grant.currentRadius = grantPlan->startRadius;
        if (ObjectGrantStealthComponent* existing =
                ecs::try_get<ObjectGrantStealthComponent>(registry,
                                                           entity)) {
            *existing = std::move(grant);
        } else {
            ecs::emplace<ObjectGrantStealthComponent>(
                registry, entity, std::move(grant));
        }
    }
}

bool ObjectStealthSystem::markDetected(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint32_t frames, const ObjectSimulationRules& rules,
    uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return false;
    ObjectStealthComponent* runtime =
        ecs::try_get<ObjectStealthComponent>(registry, *entity);
    if (!runtime || !runtime->plan) return false;
    if (runtime->plan->disguisesAsTeam) {
        static_cast<void>(disguiseAsObject(
            registry, lifecycle, object, INVALID_OBJECT_ID, rules,
            confirmedTick));
    }
    const uint64_t duration = frames != 0
        ? frames
        : millisecondsToFrames(effectiveStealthDelayMilliseconds(
              registry, lifecycle, *entity, *runtime),
              rules.logicFramesPerSecond);
    const uint64_t requestedExpiry = saturatingAdd(confirmedTick, duration);
    // RefCode's default/zero call replaces the deadline with StealthDelay;
    // an explicit detector pulse may only extend an already longer reveal.
    runtime->detectionExpiresTick = frames == 0
        ? requestedExpiry
        : std::max(runtime->detectionExpiresTick, requestedExpiry);
    runtime->stealthAllowedTick = std::max(
        runtime->stealthAllowedTick, runtime->detectionExpiresTick);
    if (effectiveRevealOrderEnabled(
            registry, lifecycle, *entity, *runtime)) {
        ObjectStealthRevealOrderRequestComponent* request =
            ecs::try_get<ObjectStealthRevealOrderRequestComponent>(
                registry, *entity);
        if (!request) {
            request = &ecs::emplace<
                ObjectStealthRevealOrderRequestComponent>(registry,
                                                           *entity);
        }
        request->requestedTick = confirmedTick;
        ++request->revision;
        if (request->revision == 0) request->revision = 1;
    }
    return true;
}

bool ObjectStealthSystem::receiveGrant(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, bool active, uint32_t frames,
    uint64_t confirmedTick) const {
    container::Vector<ObjectId> pending{object};
    container::Vector<ObjectId> visited;
    bool rootApplied = false;
    for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const ObjectId current = pending[cursor];
        if (!current || std::find(visited.begin(), visited.end(), current) !=
                            visited.end()) {
            continue;
        }
        visited.push_back(current);
        const std::optional<ecs::entity> entity =
            lifecycle.entityFromIdIncludingPending(current);
        if (!entity) continue;
        ObjectStealthComponent* runtime =
            ecs::try_get<ObjectStealthComponent>(registry, *entity);
        // RefCode deliberately refuses GPS-style grants for disguise hosts.
        if (!runtime || !runtime->plan || runtime->plan->disguisesAsTeam) {
            continue;
        }
        runtime->enabled = active;
        runtime->temporaryGrantExpiresTick = active && frames != 0
            ? saturatingAdd(confirmedTick, frames) : 0;
        const ObjectOrderQueueComponent* orders =
            ecs::try_get<ObjectOrderQueueComponent>(registry, *entity);
        runtime->temporaryGrantObservedExternalOrderRevision =
            active && frames != 0 && orders ? orders->externalRevision : 0;
        const game::ObjectStatusMask mask =
            game::objectStatusBit(game::ObjectStatusFlag::CanStealth) |
            game::objectStatusBit(game::ObjectStatusFlag::Stealthed);
        static_cast<void>(ObjectStatusSystem::apply(
            registry, *entity,
            {.setMask = active ? mask : 0,
             .clearMask = active ? 0 : mask,
             .confirmedTick = confirmedTick}));
        runtime->stealthAllowedTick = active
            ? confirmedTick : std::numeric_limits<uint64_t>::max();
        if (current == object) rootApplied = true;

        const ObjectContainmentComponent* containment =
            ecs::try_get<ObjectContainmentComponent>(registry, *entity);
        if (!containment) continue;
        const bool riderChange =
            hasContainmentKind(
                registry, *entity, ObjectContainmentKind::RiderChange);
        for (const ObjectContainedObjectRecord& rider :
             containment->objects) {
            if (!rider.object) continue;
            if (!riderChange) {
                const std::optional<ecs::entity> child =
                    lifecycle.entityFromIdIncludingPending(rider.object);
                const ObjectContainedByComponent* edge = child
                    ? ecs::try_get<ObjectContainedByComponent>(registry,
                                                                *child)
                    : nullptr;
                // Visible structural riders (Overlord/Helix portable
                // structures) inherit both grant and cancellation. Ordinary
                // passengers and station garrisons retain their own stealth.
                if (!edge || edge->container != current || edge->enclosing ||
                    !edge->destroyWithContainer) {
                    continue;
                }
            }
            pending.push_back(rider.object);
        }
    }
    return rootApplied;
}

bool ObjectStealthSystem::disguiseAsObject(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, ObjectId target, const ObjectSimulationRules& rules,
    uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return false;
    ObjectStealthComponent* stealth =
        ecs::try_get<ObjectStealthComponent>(registry, *entity);
    ObjectDisguiseComponent* disguise =
        ecs::try_get<ObjectDisguiseComponent>(registry, *entity);
    if (!stealth || !stealth->plan ||
        !stealth->plan->disguisesAsTeam || !disguise) {
        return false;
    }

    if (target) {
        const std::optional<ecs::entity> targetEntity =
            lifecycle.entityFromId(target);
        if (!targetEntity || effectivelyDead(registry, *targetEntity)) {
            return false;
        }
        container::String templateName;
        PlayerId player = INVALID_PLAYER_ID;
        if (const ObjectDisguiseComponent* targetDisguise =
                ecs::try_get<ObjectDisguiseComponent>(registry,
                                                       *targetEntity);
            targetDisguise &&
            !targetDisguise->requestedTemplateName.empty()) {
            templateName = targetDisguise->requestedTemplateName;
            player = targetDisguise->requestedPlayer;
        } else {
            const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(registry,
                                                      *targetEntity);
            const OwnerComponent* owner =
                ecs::try_get<OwnerComponent>(registry, *targetEntity);
            if (!type || type->name.empty() || !owner || !owner->player) {
                return false;
            }
            templateName = type->name;
            player = owner->player;
        }
        if (templateName.empty() || !player) return false;
        disguise->sourceTarget = target;
        disguise->requestedTemplateName = std::move(templateName);
        disguise->requestedPlayer = player;
        disguise->transitionStartedTick = confirmedTick;
        const uint64_t duration = millisecondsToFrames(
            stealth->plan->disguiseTransitionMilliseconds,
            rules.logicFramesPerSecond);
        disguise->transitionMidpointTick = transitionMidpoint(
            confirmedTick, duration);
        disguise->transitionCompleteTick = saturatingAdd(
            confirmedTick, duration);
        disguise->phase = ObjectDisguisePhase::Disguising;
        disguise->transitionMidpointApplied = false;
        stealth->enabled = true;
        stealth->stealthAllowedTick = confirmedTick;
        advanceDisguise(
            registry, *entity, *stealth, *disguise, object,
            m_disguisePresentationEvents, confirmedTick);
        return true;
    }

    if (disguise->phase == ObjectDisguisePhase::None ||
        disguise->phase == ObjectDisguisePhase::Revealing) {
        return true;
    }
    disguise->sourceTarget = INVALID_OBJECT_ID;
    disguise->requestedTemplateName.clear();
    disguise->requestedPlayer = INVALID_PLAYER_ID;
    disguise->transitionStartedTick = confirmedTick;
    const uint64_t duration = millisecondsToFrames(
        stealth->plan->disguiseRevealTransitionMilliseconds,
        rules.logicFramesPerSecond);
    disguise->transitionMidpointTick = transitionMidpoint(
        confirmedTick, duration);
    disguise->transitionCompleteTick = saturatingAdd(
        confirmedTick, duration);
    disguise->phase = ObjectDisguisePhase::Revealing;
    disguise->transitionMidpointApplied = false;
    advanceDisguise(
        registry, *entity, *stealth, *disguise, object,
        m_disguisePresentationEvents, confirmedTick);
    return true;
}

bool ObjectStealthSystem::setDetectorEnabled(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, bool enabled, uint64_t confirmedTick) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    if (!entity) return false;
    ObjectStealthDetectorComponent* detector =
        ecs::try_get<ObjectStealthDetectorComponent>(registry, *entity);
    if (!detector || !detector->plan) return false;
    detector->enabled = enabled;
    detector->nextScanTick = enabled
        ? confirmedTick : std::numeric_limits<uint64_t>::max();
    return true;
}

void ObjectStealthSystem::updateDetectors(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, const ObjectSpatialIndex& spatialIndex,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Vector<ObjectStealthDetectorPulseEvent>& events) const {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> detectors;
    const auto detectorView = ecs::view<
        const ObjectIdentityComponent, ObjectStealthDetectorComponent>(
            registry);
    detectors.reserve(detectorView.size_hint());
    for (const ecs::entity entity : detectorView) {
        const ObjectId object = detectorView.template get<
            const ObjectIdentityComponent>(entity).id;
        if (object && lifecycle.entityFromId(object)) {
            detectors.push_back({object, entity});
        }
    }
    std::sort(detectors.begin(), detectors.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });
    if (detectors.empty()) return;

    // A garrison container does not own its riders as ECS children. Build
    // one stable reverse relation for all due detector scans this tick.
    container::HashMap<ObjectId, container::Vector<ObjectId>>
        ridersByContainer;
    const auto containedView = ecs::view<
        const ObjectIdentityComponent, const ObjectContainedByComponent>(
            registry);
    for (const ecs::entity entity : containedView) {
        const ObjectId rider = containedView.template get<
            const ObjectIdentityComponent>(entity).id;
        const ObjectId container = containedView.template get<
            const ObjectContainedByComponent>(entity).container;
        if (rider && container && lifecycle.entityFromId(rider)) {
            ridersByContainer[container].push_back(rider);
        }
    }
    for (auto& [container, riders] : ridersByContainer) {
        static_cast<void>(container);
        std::sort(riders.begin(), riders.end());
        riders.erase(std::unique(riders.begin(), riders.end()), riders.end());
    }

    const ObjectDisabledMask held =
        objectDisabledBit(ObjectDisabledReason::Held);
    const auto durationFrames = [&](uint64_t interval,
                                    uint64_t padding) noexcept {
        const uint64_t duration = std::min<uint64_t>(
            interval + padding,
            std::numeric_limits<uint32_t>::max());
        return static_cast<uint32_t>(duration);
    };
    const auto normalizeIds = [](container::Vector<ObjectId>& ids) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    };

    container::Vector<ObjectId> nearby;
    for (const Candidate& detectorCandidate : detectors) {
        ObjectStealthDetectorComponent* runtime =
            ecs::try_get<ObjectStealthDetectorComponent>(
                registry, detectorCandidate.entity);
        if (!runtime || !runtime->plan || !runtime->enabled ||
            confirmedTick < runtime->nextScanTick) {
            continue;
        }
        if (effectivelyDead(registry, detectorCandidate.entity)) {
            runtime->enabled = false;
            runtime->nextScanTick = std::numeric_limits<uint64_t>::max();
            continue;
        }
        const ObjectStatusComponent* detectorStatus =
            ecs::try_get<ObjectStatusComponent>(registry,
                                                detectorCandidate.entity);
        const game::ObjectStatusMask detectorFlags = detectorStatus
            ? detectorStatus->flags : 0;
        if ((detectorFlags & game::objectStatusBit(
                game::ObjectStatusFlag::Sold)) != 0) {
            runtime->enabled = false;
            runtime->nextScanTick = std::numeric_limits<uint64_t>::max();
            continue;
        }
        if ((detectorFlags & game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction)) != 0) {
            runtime->nextScanTick = saturatingAdd(confirmedTick, 1u);
            continue;
        }
        if ((objectDisabledMask(registry, detectorCandidate.entity,
                                confirmedTick) & ~held) != 0) {
            runtime->nextScanTick = saturatingAdd(confirmedTick, 1u);
            continue;
        }

        const game::ObjectStealthDetectorPlan& plan = *runtime->plan;
        const uint64_t interval = std::max<uint64_t>(
            1u, millisecondsToFrames(plan.detectionRateMilliseconds,
                                      rules.logicFramesPerSecond));
        runtime->nextScanTick = saturatingAdd(confirmedTick, interval);

        if (const ObjectContainedByComponent* contained =
                ecs::try_get<ObjectContainedByComponent>(
                    registry, detectorCandidate.entity);
            contained && contained->enclosing && contained->container) {
            const std::optional<ecs::entity> containerEntity =
                lifecycle.entityFromId(contained->container);
            const bool garrisoned = containerEntity &&
                isGarrisonableContainer(registry, *containerEntity);
            if ((garrisoned && !plan.canDetectWhileGarrisoned) ||
                (!garrisoned && !plan.canDetectWhileContained)) {
                continue;
            }
        }

        math::q32_32 range = plan.detectionRange;
        if (range <= math::q32_32{}) {
            range = effectiveObjectVisionRangeFixed(
                registry, detectorCandidate.entity);
        }
        range = math::q32_32::max(math::q32_32{}, range);
        const LogicFixedVec3 detectorPosition = authoritativePosition(
            registry, detectorCandidate.entity);
        const OwnerComponent* detectorOwner =
            ecs::try_get<OwnerComponent>(registry,
                                         detectorCandidate.entity);
        const PlayerId sourcePlayer = detectorOwner
            ? detectorOwner->player : INVALID_PLAYER_ID;

        ObjectStealthDetectorPulseEvent event;
        event.detector = detectorCandidate.object;
        event.authoredOrder = plan.authoredOrder;
        event.beaconParticleSystem = plan.beaconParticleSystem;
        event.gridParticleSystem = plan.gridParticleSystem;
        event.particleBone = plan.particleBone;
        event.confirmedTick = confirmedTick;

        spatialIndex.queryRadiusFixed(
            detectorPosition, range, nearby);
        for (const ObjectId targetObject : nearby) {
            if (!targetObject || targetObject == detectorCandidate.object) {
                continue;
            }
            const std::optional<ecs::entity> targetEntity =
                lifecycle.entityFromId(targetObject);
            if (!targetEntity || effectivelyDead(registry, *targetEntity) ||
                !mapStatusMatches(registry, detectorCandidate.entity,
                                  *targetEntity) ||
                !withinPlanarRange(detectorPosition,
                    authoritativePosition(registry, *targetEntity), range)) {
                continue;
            }
            const OwnerComponent* targetOwner =
                ecs::try_get<OwnerComponent>(registry, *targetEntity);
            const PlayerId targetPlayer = targetOwner
                ? targetOwner->player : INVALID_PLAYER_ID;
            if (relationshipBetweenObjects(
                    registry, players, detectorCandidate.entity,
                    *targetEntity) ==
                PlayerRelationship::Allies) {
                continue;
            }
            const ObjectKindOfComponent* targetKinds =
                ecs::try_get<ObjectKindOfComponent>(registry, *targetEntity);
            if ((plan.extraRequiredKinds.any() &&
                 !matchesAnyKind(targetKinds, plan.extraRequiredKinds)) ||
                matchesAnyKind(targetKinds, plan.extraForbiddenKinds)) {
                continue;
            }

            ObjectStealthComponent* targetStealth =
                ecs::try_get<ObjectStealthComponent>(registry,
                                                      *targetEntity);
            const ObjectStatusComponent* targetStatus =
                ecs::try_get<ObjectStatusComponent>(registry,
                                                     *targetEntity);
            const bool targetIsStealthed = targetStatus &&
                targetStatus->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::Stealthed));
            if (targetStealth && targetIsStealthed) {
                const bool wasDetected =
                    targetStealth->detectionExpiresTick > confirmedTick ||
                    targetStatus->hasAny(game::objectStatusBit(
                        game::ObjectStatusFlag::Detected));
                if (markDetected(
                        registry, lifecycle, targetObject,
                        durationFrames(interval, 1u), rules,
                        confirmedTick)) {
                    targetStealth->heatVisionOpacity = math::q32_32{int32_t{1}};
                    event.detectedTargets.push_back(targetObject);
                    event.gridTargets.push_back(targetObject);
                    if (!wasDetected) {
                        event.newlyDetectedTargets.push_back(targetObject);
                    }
                }
                continue;
            }

            if (!isGarrisonableContainer(registry, *targetEntity)) continue;
            const auto riders = ridersByContainer.find(targetObject);
            if (riders == ridersByContainer.end()) continue;
            for (const ObjectId riderObject : riders->second) {
                const std::optional<ecs::entity> riderEntity =
                    lifecycle.entityFromId(riderObject);
                if (!riderEntity || effectivelyDead(registry, *riderEntity)) {
                    continue;
                }
                const OwnerComponent* riderOwner =
                    ecs::try_get<OwnerComponent>(registry, *riderEntity);
                const PlayerId riderPlayer = riderOwner
                    ? riderOwner->player : INVALID_PLAYER_ID;
                if (relationshipBetweenObjects(
                        registry, players, detectorCandidate.entity,
                        *riderEntity) ==
                    PlayerRelationship::Allies) {
                    continue;
                }
                ObjectStealthComponent* riderStealth =
                    ecs::try_get<ObjectStealthComponent>(registry,
                                                         *riderEntity);
                const ObjectStatusComponent* riderStatus =
                    ecs::try_get<ObjectStatusComponent>(registry,
                                                        *riderEntity);
                if (!riderStealth || !riderStatus ||
                    !riderStatus->hasAny(game::objectStatusBit(
                        game::ObjectStatusFlag::Stealthed))) {
                    continue;
                }
                const bool wasDetected =
                    riderStealth->detectionExpiresTick > confirmedTick ||
                    riderStatus->hasAny(game::objectStatusBit(
                        game::ObjectStatusFlag::Detected));
                if (markDetected(
                        registry, lifecycle, riderObject,
                        durationFrames(interval, 2u), rules,
                        confirmedTick)) {
                    riderStealth->heatVisionOpacity = math::q32_32{int32_t{1}};
                    event.detectedTargets.push_back(riderObject);
                    if (!wasDetected) {
                        event.newlyDetectedTargets.push_back(riderObject);
                    }
                }
            }
        }

        normalizeIds(event.detectedTargets);
        normalizeIds(event.gridTargets);
        normalizeIds(event.newlyDetectedTargets);
        const bool foundSomeone = !event.detectedTargets.empty();
        event.pingSound = foundSomeone
            ? plan.loudPingSound : plan.pingSound;
        event.particleSystem = foundSomeone
            ? plan.brightScanParticleSystem : plan.scanParticleSystem;
        events.push_back(std::move(event));
    }
}

void ObjectStealthSystem::updateGrantors(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, const ObjectSpatialIndex& spatialIndex,
    uint64_t confirmedTick,
    container::Vector<ObjectGrantStealthPulseEvent>& events) const {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> grantors;
    const auto view = ecs::view<
        const ObjectIdentityComponent, ObjectGrantStealthComponent>(registry);
    grantors.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectId object = view.template get<
            const ObjectIdentityComponent>(entity).id;
        if (object && lifecycle.entityFromId(object)) {
            grantors.push_back({object, entity});
        }
    }
    std::sort(grantors.begin(), grantors.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });
    if (grantors.empty()) return;

    container::HashMap<ObjectId, container::Vector<ObjectId>>
        ridersByContainer;
    const auto containedView = ecs::view<
        const ObjectIdentityComponent, const ObjectContainedByComponent>(
            registry);
    for (const ecs::entity entity : containedView) {
        const ObjectId rider = containedView.template get<
            const ObjectIdentityComponent>(entity).id;
        const ObjectId container = containedView.template get<
            const ObjectContainedByComponent>(entity).container;
        if (rider && container && lifecycle.entityFromId(rider)) {
            ridersByContainer[container].push_back(rider);
        }
    }
    for (auto& [container, riders] : ridersByContainer) {
        static_cast<void>(container);
        std::sort(riders.begin(), riders.end());
        riders.erase(std::unique(riders.begin(), riders.end()), riders.end());
    }

    container::Vector<ObjectId> nearby;
    for (const Candidate& candidate : grantors) {
        ObjectGrantStealthComponent* runtime =
            ecs::try_get<ObjectGrantStealthComponent>(registry,
                                                       candidate.entity);
        if (!runtime || !runtime->plan ||
            effectivelyDead(registry, candidate.entity) ||
            objectDisabledMask(registry, candidate.entity,
                               confirmedTick) != 0) {
            continue;
        }
        const game::ObjectGrantStealthPlan& plan = *runtime->plan;
        runtime->currentRadius += plan.radiusGrowPerFrame;
        bool finalScan = false;
        if (runtime->currentRadius >= plan.finalRadius) {
            runtime->currentRadius = plan.finalRadius;
            finalScan = true;
        }

        ObjectGrantStealthPulseEvent event;
        event.grantor = candidate.object;
        event.authoredOrder = plan.authoredOrder;
        event.currentRadius = runtime->currentRadius;
        event.finalScan = finalScan;
        event.confirmedTick = confirmedTick;
        if (!runtime->presentationStarted) {
            runtime->presentationStarted = true;
            event.radiusParticleSystem = plan.radiusParticleSystem;
            uint64_t lifetime = 1;
            const int64_t delta = plan.finalRadius.raw() -
                                  plan.startRadius.raw();
            const int64_t growth = plan.radiusGrowPerFrame.raw();
            if (delta > 0 && growth > 0) {
                lifetime = static_cast<uint64_t>(delta / growth) +
                    static_cast<uint64_t>((delta % growth) != 0);
            }
            event.particleLifetimeFrames = static_cast<uint32_t>(
                std::min<uint64_t>(
                    lifetime, std::numeric_limits<uint32_t>::max()));
        }

        const LogicFixedVec3 origin = authoritativePosition(
            registry, candidate.entity);
        const OwnerComponent* sourceOwner =
            ecs::try_get<OwnerComponent>(registry, candidate.entity);
        const PlayerId sourcePlayer = sourceOwner
            ? sourceOwner->player : INVALID_PLAYER_ID;
        spatialIndex.queryRadiusFixed(
            origin, runtime->currentRadius, nearby);
        for (const ObjectId targetObject : nearby) {
            if (!targetObject || targetObject == candidate.object) continue;
            const std::optional<ecs::entity> targetEntity =
                lifecycle.entityFromId(targetObject);
            if (!targetEntity || effectivelyDead(registry, *targetEntity) ||
                !mapStatusMatches(registry, candidate.entity, *targetEntity) ||
                !withinPlanarRange(origin,
                    authoritativePosition(registry, *targetEntity),
                    runtime->currentRadius)) {
                continue;
            }
            const OwnerComponent* targetOwner =
                ecs::try_get<OwnerComponent>(registry, *targetEntity);
            const PlayerId targetPlayer = targetOwner
                ? targetOwner->player : INVALID_PLAYER_ID;
            if (relationshipBetweenObjects(
                    registry, players, candidate.entity, *targetEntity) !=
                PlayerRelationship::Allies) {
                continue;
            }
            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(registry, *targetEntity);
            if ((!plan.allKindsAllowed &&
                 !matchesAnyKind(kinds, plan.allowedKinds)) ||
                matchesAnyKind(kinds, plan.forbiddenKinds)) {
                continue;
            }
            if (receiveGrant(registry, lifecycle, targetObject, true, 0,
                             confirmedTick)) {
                event.grantedTargets.push_back(targetObject);
            }

            // RiderChangeContain exposes one visible rider which must follow
            // its chassis' grant or it appears to float unstealthed. The
            // value relation may contain more than one entry for malformed
            // MOD data; stable iteration grants each instead of picking an
            // unordered survivor.
            if (!hasContainmentKind(
                    registry, *targetEntity,
                    ObjectContainmentKind::RiderChange)) {
                continue;
            }
            const auto riders = ridersByContainer.find(targetObject);
            if (riders == ridersByContainer.end()) continue;
            for (const ObjectId rider : riders->second) {
                if (receiveGrant(registry, lifecycle, rider, true, 0,
                                 confirmedTick)) {
                    event.grantedTargets.push_back(rider);
                }
            }
        }
        std::sort(event.grantedTargets.begin(),
                  event.grantedTargets.end());
        event.grantedTargets.erase(
            std::unique(event.grantedTargets.begin(),
                        event.grantedTargets.end()),
            event.grantedTargets.end());
        events.push_back(std::move(event));
        if (finalScan) {
            static_cast<void>(lifecycle.requestDestroy(
                candidate.object, ObjectDestroyReason::System,
                confirmedTick));
        }
    }
}

void ObjectStealthSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) const {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectStealthComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectId object =
            view.template get<const ObjectIdentityComponent>(entity).id;
        if (object && lifecycle.entityFromId(object)) {
            candidates.push_back({object, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });
    for (const Candidate& candidate : candidates) {
        ObjectStealthComponent* runtime =
            ecs::try_get<ObjectStealthComponent>(registry, candidate.entity);
        if (!runtime || !runtime->plan) continue;
        const math::q32_32 previousHeatVisionOpacity =
            runtime->heatVisionOpacity;
        const bool previousEnabled = runtime->enabled;
        ObjectDisguiseComponent* disguise =
            ecs::try_get<ObjectDisguiseComponent>(registry,
                                                   candidate.entity);
        if (disguise) {
            advanceDisguise(
                registry, candidate.entity, *runtime, *disguise,
                candidate.object, m_disguisePresentationEvents,
                confirmedTick);
        }
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, candidate.entity);
        if (health && health->effectivelyDead) {
            runtime->enabled = false;
            runtime->heatVisionOpacity = {};
        }
        const ObjectOrderQueueComponent* orders =
            ecs::try_get<ObjectOrderQueueComponent>(registry,
                                                     candidate.entity);
        const bool temporaryGrantOverridden =
            runtime->temporaryGrantExpiresTick != 0 && orders &&
            orders->externalRevision !=
                runtime->temporaryGrantObservedExternalOrderRevision;
        if (runtime->temporaryGrantExpiresTick != 0 &&
            (confirmedTick >= runtime->temporaryGrantExpiresTick ||
             temporaryGrantOverridden)) {
            static_cast<void>(receiveGrant(
                registry, lifecycle, candidate.object, false, 0,
                confirmedTick));
        }
        const ObjectDisabledMask disabled = objectDisabledMask(
            registry, candidate.entity, confirmedTick);
        const ObjectDisabledMask held =
            objectDisabledBit(ObjectDisabledReason::Held);
        if ((disabled & ~held) != 0) continue;

        // Bomb-truck style disguises reveal once their active combat victim
        // enters the authored center-to-center radius. ObjectWeaponComponent
        // is the authoritative Combat owner of that victim; no AI vtable or
        // renderer target is consulted here.
        if (runtime->enabled &&
            runtime->plan->revealDistanceFromTarget > math::q32_32{}) {
            const ObjectWeaponComponent* weapons =
                ecs::try_get<ObjectWeaponComponent>(registry,
                                                     candidate.entity);
            const std::optional<ecs::entity> target =
                weapons && weapons->target
                    ? lifecycle.entityFromId(weapons->target)
                    : std::nullopt;
            if (target && !effectivelyDead(registry, *target) &&
                withinPlanarRange(
                    authoritativePosition(registry, candidate.entity),
                    authoritativePosition(registry, *target),
                    runtime->plan->revealDistanceFromTarget)) {
                static_cast<void>(markDetected(
                    registry, lifecycle, candidate.object, 0u, rules,
                    confirmedTick));
                // A zero-duration reveal changes appearance and disables the
                // controller in this same confirmed tick.
                if (disguise) {
                    advanceDisguise(
                        registry, candidate.entity, *runtime, *disguise,
                        candidate.object, m_disguisePresentationEvents,
                        confirmedTick);
                }
            }
        }

        game::ObjectStatusMask setMask = 0;
        game::ObjectStatusMask clearMask = 0;
        const game::ObjectStatusMask stealthed =
            game::objectStatusBit(game::ObjectStatusFlag::Stealthed);
        const game::ObjectStatusMask detected =
            game::objectStatusBit(game::ObjectStatusFlag::Detected);
        if (runtime->enabled && allowedToStealth(
                registry, lifecycle, candidate.entity, *runtime, rules,
                confirmedTick)) {
            if (confirmedTick >= runtime->stealthAllowedTick) {
                setMask |= stealthed;
            }
        } else {
            clearMask |= stealthed;
            const uint64_t delay = millisecondsToFrames(
                effectiveStealthDelayMilliseconds(
                    registry, lifecycle, candidate.entity, *runtime),
                rules.logicFramesPerSecond);
            runtime->stealthAllowedTick = saturatingAdd(
                confirmedTick, delay);
        }
        if (runtime->detectionExpiresTick > confirmedTick) {
            setMask |= detected;
        } else {
            clearMask |= detected;
        }

        const ObjectStatusComponent* currentStatus =
            ecs::try_get<ObjectStatusComponent>(registry, candidate.entity);
        const game::ObjectStatusMask projectedStatus =
            ((currentStatus ? currentStatus->flags : 0u) | setMask) &
            ~clearMask;
        // hintDetectableWhileUnstealthed() refreshes the local-owner heat
        // pass while any authored hint bit is present.  Extraction applies
        // the observer-relative owner test; the confirmed component only
        // retains the object-local envelope.
        if (!(health && health->effectivelyDead) &&
            (projectedStatus & stealthed) == 0 &&
            (projectedStatus & runtime->plan->hintDetectableStatuses) != 0) {
            runtime->heatVisionOpacity = math::q32_32{int32_t{1}};
        }

        bool frenzyActive = false;
        if (const ObjectTemporaryWeaponBonusComponent* temporary =
                ecs::try_get<ObjectTemporaryWeaponBonusComponent>(
                    registry, candidate.entity);
            temporary && temporary->current) {
            frenzyActive =
                *temporary->current == game::WeaponBonusCondition::FrenzyOne ||
                *temporary->current == game::WeaponBonusCondition::FrenzyTwo ||
                *temporary->current == game::WeaponBonusCondition::FrenzyThree;
        }
        if (!frenzyActive) {
            constexpr math::q32_32 kHeatVisionFadeScalar =
                math::q32_32::from_fraction(4, 5);
            constexpr math::q32_32 kMinimumVisibleOpacity =
                math::q32_32::from_fraction(1, 1000);
            if (runtime->heatVisionOpacity > kMinimumVisibleOpacity) {
                runtime->heatVisionOpacity *= kHeatVisionFadeScalar;
            } else {
                runtime->heatVisionOpacity = {};
            }
        }
        if (runtime->enabled) {
            constexpr int64_t kPulseStepRaw = 858993459ll;
            constexpr int64_t kTwoPiRaw = 26986075409ll;
            runtime->friendlyPulsePhaseRadians +=
                math::q32_32::from_raw(kPulseStepRaw);
            if (runtime->friendlyPulsePhaseRadians.raw() >= kTwoPiRaw) {
                runtime->friendlyPulsePhaseRadians = math::q32_32::from_raw(
                    runtime->friendlyPulsePhaseRadians.raw() % kTwoPiRaw);
            }
        }
        static_cast<void>(ObjectStatusSystem::apply(
            registry, candidate.entity,
            {.setMask = setMask, .clearMask = clearMask,
             .confirmedTick = confirmedTick}));
        if (runtime->heatVisionOpacity != previousHeatVisionOpacity ||
            runtime->enabled != previousEnabled) {
            markObjectDirty(
                registry, candidate.entity,
                ObjectDirtyDomain::RenderExtraction);
        }
    }
}

container::Vector<ObjectDisguisePresentationEvent>
ObjectStealthSystem::takeDisguisePresentationEvents() {
    container::Vector<ObjectDisguisePresentationEvent> result;
    result.swap(m_disguisePresentationEvents);
    return result;
}

void ObjectStealthSystem::resetPresentationEvents() noexcept {
    m_disguisePresentationEvents.clear();
}

} // namespace engine
