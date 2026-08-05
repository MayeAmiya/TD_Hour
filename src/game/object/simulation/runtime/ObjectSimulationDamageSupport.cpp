#include "core/container/container_types.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/ObjectArchetype.h"
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
#include "game/object/simulation/status/ObjectCheckpoint.h"
#include "game/object/simulation/status/ObjectBodyRuntime.h"
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

const PhysicsScalar kPhysicsTwo{int32_t{2}};
const HealthScalar kHealthZero{};
const HealthScalar kHealthOne{int32_t{1}};

[[nodiscard]] PhysicsScalar length3D(
    PhysicsScalar x, PhysicsScalar y, PhysicsScalar z) noexcept {
    return PhysicsScalar::sqrt(x * x + y * y + z * z);
}

} // namespace

namespace object_simulation_detail {

using container::asciiEqualIgnoreCase;

[[nodiscard]] bool hasKind(const ObjectKindOfComponent* kinds,
                           game::ObjectKindOf sought) noexcept {
    return kinds && game::objectHasKind(kinds->mask, sought);
}

[[nodiscard]] ObjectBodyDamageState damageStateFor(HealthScalar health, HealthScalar maximum,
                                                    const ObjectSimulationRules& rules) noexcept {
    if (health <= kHealthZero || maximum <= kHealthZero) return ObjectBodyDamageState::Rubble;
    const HealthScalar ratio = health / maximum;
    // RefCode's calcDamageState uses strict comparisons: exactly 50% is
    // DAMAGED and exactly 10% is REALLYDAMAGED.
    if (ratio > rules.unitDamagedThresholdFixed) {
        return ObjectBodyDamageState::Pristine;
    }
    if (ratio > rules.unitReallyDamagedThresholdFixed) {
        return ObjectBodyDamageState::Damaged;
    }
    return ObjectBodyDamageState::ReallyDamaged;
}

[[nodiscard]] bool isSubdualDamage(game::DamageType type) noexcept {
    switch (type) {
    case game::DamageType::SUBDUAL_MISSILE:
    case game::DamageType::SUBDUAL_VEHICLE:
    case game::DamageType::SUBDUAL_BUILDING:
    case game::DamageType::SUBDUAL_UNRESISTABLE:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool hasDedicatedDamageBehaviour(game::DamageType type) noexcept {
    switch (type) {
    case game::DamageType::KILL_PILOT:
    case game::DamageType::KILL_GARRISONED:
    case game::DamageType::STATUS:
        return true;
    default:
        return isSubdualDamage(type);
    }
}

[[nodiscard]] bool isHealthDamagingDamage(game::DamageType type) noexcept {
    // Match ActiveBody's split between ordinary health damage and damage
    // types delegated to a specialised module.  We deliberately do not make
    // an unimplemented specialised type subtract hit points just because it
    // arrived through the new ECS boundary.
    return type != game::DamageType::HEALING && !hasDedicatedDamageBehaviour(type);
}

[[nodiscard]] bool shouldStartSecondLife(const ObjectHealthComponent& health,
                                         const ObjectDamageRequest& request,
                                         HealthScalar estimatedAmount) noexcept {
    if (!health.hasSecondLife || health.secondLifeActive ||
        request.damageType == game::DamageType::UNRESISTABLE ||
        !isHealthDamagingDamage(request.damageType)) {
        return false;
    }
    // The corrected RefCode path asks ActiveBody::estimateDamage first, so
    // first-life interception is decided from armour-adjusted damage. An
    // explicit Object::kill supplies MaxHealth as its requested amount; use
    // that same value for the value-only force-kill ingress rather than
    // treating every malformed zero-amount force-kill as intrinsically
    // lethal to the first life.
    const HealthScalar lethalEstimate = request.forceKill
        ? health.maximumFixed : estimatedAmount;
    return lethalEstimate >= health.currentFixed;
}

[[nodiscard]] bool isMovementPenaltyState(ObjectBodyDamageState state,
                                          const ObjectSimulationRules& rules) noexcept {
    // RefCode uses IS_CONDITION_BETTER(condition, MovementPenaltyDamageState)
    // (i.e. enum ordering), not a hard-coded REALLYDAMAGED comparison.
    return static_cast<uint8_t>(state) >= static_cast<uint8_t>(rules.movementPenaltyDamageState);
}

[[nodiscard]] const game::ModelConditionMask& bodyDamageConditionMask() {
    static const game::ModelConditionMask mask =
        game::modelConditionMaskOf(game::ModelConditionFlag::Damaged, game::ModelConditionFlag::ReallyDamaged, game::ModelConditionFlag::Rubble);
    return mask;
}

[[nodiscard]] const game::ModelConditionMask& damagedConditionMask() {
    static const game::ModelConditionMask mask = game::modelConditionMaskOf(game::ModelConditionFlag::Damaged);
    return mask;
}

[[nodiscard]] const game::ModelConditionMask& reallyDamagedConditionMask() {
    static const game::ModelConditionMask mask = game::modelConditionMaskOf(game::ModelConditionFlag::ReallyDamaged);
    return mask;
}

[[nodiscard]] const game::ModelConditionMask& rubbleConditionMask() {
    static const game::ModelConditionMask mask = game::modelConditionMaskOf(game::ModelConditionFlag::Rubble);
    return mask;
}

[[nodiscard]] const game::ModelConditionMask& postCollapseConditionMask() {
    static const game::ModelConditionMask mask =
        game::modelConditionMaskOf(game::ModelConditionFlag::PostCollapse);
    return mask;
}

void projectBodyDamageVisual(ObjectBodyDamageState state,
                             RenderModelComponent& visual) noexcept {
    // This is the modern equivalent of Drawable::reactToBodyDamageStateChange:
    // body state owns only DAMAGED/REALLY_DAMAGED/RUBBLE and must leave every
    // independent condition (MOVING, FLOODED, weapon state, etc.) untouched.
    visual.modelConditionFlags.clear(bodyDamageConditionMask());
    const game::ModelConditionMask* selected = nullptr;
    switch (state) {
    case ObjectBodyDamageState::Pristine: break;
    case ObjectBodyDamageState::Damaged: selected = &damagedConditionMask(); break;
    case ObjectBodyDamageState::ReallyDamaged: selected = &reallyDamagedConditionMask(); break;
    case ObjectBodyDamageState::Rubble:
        // StructureTopple/Collapse explicitly replaces RUBBLE with the
        // terminal POST_COLLAPSE presentation. Later no-op damage barriers
        // must not resurrect the transient rubble model condition.
        if (visual.modelConditionFlags.intersectionCount(
                postCollapseConditionMask()) == 0) {
            selected = &rubbleConditionMask();
        }
        break;
    }
    if (!selected) return;
    visual.modelConditionFlags.words[0] |= selected->words[0];
    visual.modelConditionFlags.words[1] |= selected->words[1];
}

void applyStructureRubbleGameplayState(
    ecs::registry& registry, ecs::entity entity,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) {
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, entity);
    if (!hasKind(kinds, game::ObjectKindOf::Structure)) return;

    PhysicsScalar rubbleHeight = rules.defaultStructureRubbleHeight;
    if (const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(registry, entity);
        type && type->archetype &&
        type->archetype->templateData.structureRubbleHeightFixed >
            PhysicsScalar{}) {
        rubbleHeight =
            type->archetype->templateData.structureRubbleHeightFixed;
    }
    rubbleHeight = PhysicsScalar::max(PhysicsScalar{}, rubbleHeight);
    if (ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, entity)) {
        // GeometryInfo::setGeometryInfoZ preserves the XY footprint and
        // recalculates only the derived 3D bound.
        geometry->heightFixed = rubbleHeight;
        switch (geometry->shape) {
        case ObjectGeometryShape::Sphere:
            geometry->boundingSphereRadiusFixed = geometry->majorRadiusFixed;
            break;
        case ObjectGeometryShape::Cylinder:
            geometry->boundingSphereRadiusFixed = PhysicsScalar::max(
                geometry->majorRadiusFixed, rubbleHeight / kPhysicsTwo);
            break;
        case ObjectGeometryShape::Box:
            {
                const PhysicsScalar halfHeight = rubbleHeight / kPhysicsTwo;
                geometry->boundingSphereRadiusFixed = length3D(
                    geometry->majorRadiusFixed,
                    geometry->minorRadiusFixed, halfHeight);
            }
            break;
        }
    }
    // The status transition invalidates Collision/Spatial consumers. The
    // navigation obstacle projection therefore observes both the new rubble
    // Z extent and the permanent non-colliding state at its next barrier.
    static_cast<void>(ObjectStatusSystem::apply(
        registry, entity,
        {.setMask = game::objectStatusBit(
             game::ObjectStatusFlag::NoCollisions),
         .confirmedTick = confirmedTick}));

}

void updateBodyDamageVisuals(ecs::registry& registry) {
    const auto view = ecs::view<const ObjectHealthComponent, RenderModelComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectHealthComponent& health = view.template get<const ObjectHealthComponent>(entity);
        RenderModelComponent& visual = view.template get<RenderModelComponent>(entity);
        projectBodyDamageVisual(
            objectBodyDamagePresentationState(
                registry, entity, health.damageState),
            visual);
    }
}

void initializeResolvedArmor(ecs::registry& registry, ecs::entity entity,
                             const GameContentSnapshot& content) {
    ObjectCombatProfileComponent* combat =
        ecs::try_get<ObjectCombatProfileComponent>(registry, entity);
    if (!combat || !combat->profile || combat->profile->armorSets().empty()) return;

    ObjectArmorComponent resolved;
    const container::Span<const game::ArmorSetProfile> authoredSets = combat->profile->armorSets();
    resolved.sets.reserve(authoredSets.size());
    for (const game::ArmorSetProfile& authored : authoredSets) {
        ObjectArmorSetRuntime set;
        set.conditions = authored.conditions;
        set.armorTemplateName = authored.armorTemplateName;
        set.damageFxName = authored.damageFxName;
        if (const game::ArmorTemplate* templateData = content.findArmor(authored.armorTemplateName)) {
            for (size_t index = 0;
                 index < set.damageMultipliersFixed.size(); ++index) {
                set.damageMultipliersFixed[index] =
                    templateData->armor[index];
            }
        }
        resolved.sets.push_back(std::move(set));
    }
    refreshResolvedObjectArmor(*combat, resolved);

    if (ObjectArmorComponent* existing = ecs::try_get<ObjectArmorComponent>(registry, entity)) {
        *existing = std::move(resolved);
    } else {
        ecs::emplace<ObjectArmorComponent>(registry, entity, std::move(resolved));
    }
}

[[nodiscard]] HealthScalar armorMultiplierFor(const ecs::registry& registry, ecs::entity entity,
                                               game::DamageType type) noexcept {
    // Armor::adjustDamage explicitly leaves both unresistable channels
    // untouched. Keep this invariant even if malformed/modded Armor.ini data
    // happens to publish a multiplier for those enum slots.
    if (type == game::DamageType::UNRESISTABLE ||
        type == game::DamageType::SUBDUAL_UNRESISTABLE) {
        return kHealthOne;
    }
    const ObjectArmorComponent* armor = ecs::try_get<ObjectArmorComponent>(registry, entity);
    const size_t index = static_cast<size_t>(type);
    return !armor || index >= armor->damageMultipliersFixed.size()
        ? kHealthOne
        : HealthScalar::max(
              HealthScalar{}, armor->damageMultipliersFixed[index]);
}

[[nodiscard]] HealthScalar damageMultiplierFor(const ecs::registry& registry,
                                                ecs::entity entity,
                                                game::DamageType type) noexcept {
    HealthScalar multiplier = armorMultiplierFor(registry, entity, type);
    // ActiveBody applies its stacked damage scalar only to ordinary damage;
    // unresistable damage bypasses both armor and this later modifier.
    if (type == game::DamageType::UNRESISTABLE) return multiplier;
    if (const ObjectBattlePlanEffectComponent* battlePlan =
            ecs::try_get<ObjectBattlePlanEffectComponent>(registry, entity)) {
        multiplier *= HealthScalar::max(HealthScalar{},
                                        battlePlan->armorDamageScalar);
    }
    return multiplier;
}

void appendIgnoredEvent(container::Vector<ObjectHealthEvent>& events, const ObjectDamageRequest& request,
                        const ObjectHealthComponent* health) {
    events.push_back({
        .kind = ObjectHealthEventKind::Ignored,
        .object = request.target,
        .source = request.source,
        .damageType = request.damageType,
        .damageFxType = request.damageFxOverride.value_or(request.damageType),
        .deathType = request.deathType,
        .requestedAmount = request.amount,
        .previousHealth = health ? health->currentFixed : HealthScalar{},
        .currentHealth = health ? health->currentFixed : HealthScalar{},
        .previousState = health ? health->damageState : ObjectBodyDamageState::Pristine,
        .currentState = health ? health->damageState : ObjectBodyDamageState::Pristine,
        .confirmedTick = request.confirmedTick,
    });
}

void rememberPreferredBodyDamageInfo(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectHealthComponent& health, const ObjectDamageRequest& request) {
    const auto rememberSource = [&]() {
        health.lastDamageSource = request.source;
        health.lastDamageSourcePlayer = INVALID_PLAYER_ID;
        health.lastDamageSourceArchetype.reset();
        const std::optional<ecs::entity> source =
            lifecycle.entityFromIdIncludingPending(request.source);
        if (!source) return;
        if (const OwnerComponent* owner =
                ecs::try_get<OwnerComponent>(registry, *source)) {
            health.lastDamageSourcePlayer = owner->player;
        }
        if (const ThingTemplateComponent* type =
                ecs::try_get<ThingTemplateComponent>(registry, *source)) {
            health.lastDamageSourceArchetype = type->archetype;
        }
    };
    if (request.damageType == game::DamageType::HEALING) {
        rememberSource();
        health.lastDamageType = request.damageType;
        health.lastDamageTick = request.confirmedTick;
        health.hasLastDamageInfo = true;
        return;
    }

    const bool recent = health.hasLastDamageInfo &&
        (health.lastDamageTick == request.confirmedTick ||
         (request.confirmedTick > 0 &&
          health.lastDamageTick == request.confirmedTick - 1u));
    bool replace = !recent;
    if (recent) {
        const std::optional<ecs::entity> currentSource =
            lifecycle.entityFromIdIncludingPending(request.source);
        if (currentSource) {
            const std::optional<ecs::entity> previousSource =
                lifecycle.entityFromIdIncludingPending(health.lastDamageSource);
            if (!previousSource) {
                replace = true;
            } else {
                const ObjectKindOfComponent* kinds =
                    ecs::try_get<ObjectKindOfComponent>(
                        registry, *currentSource);
                replace = hasKind(kinds, game::ObjectKindOf::Vehicle) ||
                    hasKind(kinds, game::ObjectKindOf::Infantry) ||
                    hasKind(kinds, game::ObjectKindOf::Structure);
            }
        }
    }
    if (!replace) return;
    rememberSource();
    health.lastDamageType = request.damageType;
    health.lastDamageTick = request.confirmedTick;
    health.hasLastDamageInfo = true;
}

[[nodiscard]] uint64_t subdualRecoveryIntervalTicks(const ObjectHealthComponent& health,
                                                     const ObjectSimulationRules& rules) noexcept {
    const uint64_t milliseconds = health.subdualDamageHealIntervalMilliseconds;
    const uint64_t framesPerSecond = std::max<uint32_t>(1, rules.logicFramesPerSecond);
    // RefCode parses duration values with ceil(ms * fps / 1000). Its unsigned
    // countdown underflows for an authored zero rate; modern content instead
    // gives zero an explicit, safe one-tick recovery interval.
    if (milliseconds == 0) return 1;
    const uint64_t ticks = (milliseconds * framesPerSecond + 999u) / 1000u;
    return std::max<uint64_t>(1, ticks);
}

void appendSubdualEvent(container::Vector<ObjectHealthEvent>& events, ObjectHealthEventKind kind,
                        const ObjectDamageRequest& request, const ObjectHealthComponent& health,
                        HealthScalar previousSubdual, bool wasSubdued) {
    events.push_back({
        .kind = kind,
        .object = request.target,
        .source = request.source,
        .damageType = request.damageType,
        .damageFxType = request.damageFxOverride.value_or(request.damageType),
        .deathType = request.deathType,
        .requestedAmount = request.amount,
        .appliedAmountFixed = health.subdualDamageFixed - previousSubdual,
        .actualDamageDealtFixed = health.subdualDamageFixed - previousSubdual,
        // Subdual never changes ordinary HP. Do not leak the previous
        // unrelated HP transaction through this event's observation fields.
        .previousHealth = health.currentFixed,
        .currentHealth = health.currentFixed,
        .previousSubdualDamage = previousSubdual,
        .currentSubdualDamage = health.subdualDamageFixed,
        .wasSubdued = wasSubdued,
        .isSubdued = health.subdued,
        .previousState = health.damageState,
        .currentState = health.damageState,
        .confirmedTick = request.confirmedTick,
    });
}

// Applies the common Body-side portion of the original ActiveBody subdual
// transaction.  It deliberately does not change ordinary HP, BodyDamageState
// or lifecycle: a microwave/subdual hit disables a unit rather than killing
// it.  Projectile-specific `projectileNowJammed()` remains the responsibility
// of the future MissileAI/Projectile controller; the state fact is still
// preserved here so it cannot be lost before that controller is migrated.
void applySubdualDamage(ecs::registry& registry, ecs::entity entity,
                        ObjectHealthComponent& health, const ObjectDamageRequest& request,
                        HealthScalar requestedAmount, const ObjectSimulationRules& rules,
                        container::Vector<ObjectHealthEvent>& events) {
    if (health.subdualDamageCapFixed <= kHealthZero) {
        appendIgnoredEvent(events, request, &health);
        return;
    }

    HealthScalar amount = requestedAmount;
    if (request.damageType != game::DamageType::SUBDUAL_UNRESISTABLE) {
        // RefCode's normal subdual damage is armor-adjusted and rejects a
        // negative authored/input amount. SUBDUAL_UNRESISTABLE is also the
        // helper's recovery channel, so it must retain a negative delta and
        // bypass armor exactly like DAMAGE_UNRESISTABLE.
        amount = HealthScalar::max(kHealthZero, amount);
        amount *= armorMultiplierFor(registry, entity, request.damageType);
    }

    const HealthScalar previousSubdual = health.subdualDamageFixed;
    const bool wasSubdued = health.subdued;
    health.subdualDamageFixed = HealthScalar::clamp(previousSubdual + amount,
                                                     kHealthZero,
                                                     health.subdualDamageCapFixed);
    health.subdued = health.subdualDamageFixed >= health.maximumFixed;

    // ActiveBody dispatches the disabled/jammed transition before it notifies
    // the generic subdual helper/visual listeners. Preserve that observable
    // order in the value event stream.
    if (health.subdued != wasSubdued) {
        appendSubdualEvent(events, ObjectHealthEventKind::SubdualStateChanged,
                           request, health, previousSubdual, wasSubdued);
    }
    appendSubdualEvent(events,
                       amount < kHealthZero ? ObjectHealthEventKind::SubdualRecovered
                                             : ObjectHealthEventKind::SubdualDamaged,
                       request, health, previousSubdual, wasSubdued);

    if (amount > kHealthZero) {
        health.nextSubdualRecoveryTick = request.confirmedTick +
            subdualRecoveryIntervalTicks(health, rules);
    }
}

void updateSubdualRecovery(ecs::registry& registry, ObjectLifecycle& lifecycle,
                           const ObjectSimulationRules& rules, uint64_t confirmedTick,
                           container::Vector<ObjectHealthEvent>& events) {
    struct Candidate final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent, ObjectHealthComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id)) continue;
        candidates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.id < right.id;
    });

    for (const Candidate& candidate : candidates) {
        ObjectHealthComponent& health =
            ecs::get<ObjectHealthComponent>(registry, candidate.entity);
        if (!health.acceptsDamage || health.effectivelyDead ||
            health.nextSubdualRecoveryTick == 0 ||
            confirmedTick < health.nextSubdualRecoveryTick) {
            continue;
        }
        if (health.subdualDamageFixed <= kHealthZero ||
            health.subdualDamageHealAmountFixed <= kHealthZero ||
            health.subdualDamageCapFixed <= kHealthZero) {
            health.nextSubdualRecoveryTick = 0;
            continue;
        }

        ObjectDamageRequest recovery{
            .target = candidate.id,
            .amount = -health.subdualDamageHealAmountFixed,
            .damageType = game::DamageType::SUBDUAL_UNRESISTABLE,
            .confirmedTick = confirmedTick,
        };
        applySubdualDamage(registry, candidate.entity, health, recovery,
                           -health.subdualDamageHealAmountFixed, rules, events);
        health.nextSubdualRecoveryTick = health.subdualDamageFixed > kHealthZero
            ? confirmedTick + subdualRecoveryIntervalTicks(health, rules)
            : 0;
    }
}

void applyTimedStatusDamage(
    ecs::registry& registry, ecs::entity entity,
    ObjectHealthComponent& health, const ObjectDamageRequest& request,
    HealthScalar requestedAmount, const ObjectSimulationRules& rules,
    container::Vector<ObjectHealthEvent>& events) {
    const uint64_t statusMask = request.damageStatusMask &
        game::objectStatusKnownMask();
    if (statusMask == 0 || (statusMask & (statusMask - 1u)) != 0 ||
        requestedAmount <= kHealthZero) {
        appendIgnoredEvent(events, request, &health);
        return;
    }
    HealthScalar adjusted = requestedAmount *
        armorMultiplierFor(registry, entity, game::DamageType::STATUS);
    if (adjusted <= kHealthZero) {
        appendIgnoredEvent(events, request, &health);
        return;
    }
    const HealthScalar frameDuration =
        adjusted * HealthScalar{static_cast<int32_t>(
            std::max<uint32_t>(1u, rules.logicFramesPerSecond))} /
        HealthScalar{int32_t{1000}};
    const uint64_t rawFrames = static_cast<uint64_t>(
        std::max<int64_t>(0, frameDuration.raw()));
    const uint64_t durationTicks = std::max<uint64_t>(
        1u, (rawFrames + UINT64_C(0xffffffff)) >> 32u);

    ObjectTimedStatusDamageComponent* timed =
        ecs::try_get<ObjectTimedStatusDamageComponent>(registry, entity);
    if (!timed) {
        timed = &ecs::emplace<ObjectTimedStatusDamageComponent>(registry,
                                                               entity);
    } else if (timed->statusMask != statusMask) {
        static_cast<void>(ObjectStatusSystem::apply(
            registry, entity,
            {.clearMask = timed->statusMask,
             .confirmedTick = request.confirmedTick}));
    }
    timed->statusMask = statusMask;
    timed->clearAtTick = request.confirmedTick >
            std::numeric_limits<uint64_t>::max() - durationTicks
        ? std::numeric_limits<uint64_t>::max()
        : request.confirmedTick + durationTicks;
    static_cast<void>(ObjectStatusSystem::apply(
        registry, entity,
        {.setMask = statusMask, .confirmedTick = request.confirmedTick}));
    events.push_back({
        .kind = ObjectHealthEventKind::StatusApplied,
        .object = request.target,
        .source = request.source,
        .damageType = request.damageType,
        .damageFxType = request.damageFxOverride.value_or(request.damageType),
        .deathType = request.deathType,
        .requestedAmount = request.amount,
        .appliedAmountFixed = adjusted,
        .actualDamageDealtFixed = adjusted,
        .previousHealth = health.currentFixed,
        .currentHealth = health.currentFixed,
        .previousState = health.damageState,
        .currentState = health.damageState,
        .confirmedTick = request.confirmedTick,
    });
}

void updateTimedStatusDamage(ecs::registry& registry,
                             uint64_t confirmedTick) {
    container::Vector<ecs::entity> expired;
    const auto view = ecs::view<const ObjectTimedStatusDamageComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectTimedStatusDamageComponent& timed =
            view.template get<const ObjectTimedStatusDamageComponent>(entity);
        if (timed.clearAtTick <= confirmedTick) expired.push_back(entity);
    }
    for (const ecs::entity entity : expired) {
        const ObjectTimedStatusDamageComponent* timed =
            ecs::try_get<ObjectTimedStatusDamageComponent>(registry, entity);
        if (!timed || timed->clearAtTick > confirmedTick) continue;
        static_cast<void>(ObjectStatusSystem::apply(
            registry, entity,
            {.clearMask = timed->statusMask,
             .confirmedTick = confirmedTick}));
        ecs::remove<ObjectTimedStatusDamageComponent>(registry, entity);
    }

    expired.clear();
    const auto repulsors =
        ecs::view<const ObjectRepulsorExpiryComponent>(registry);
    for (const ecs::entity entity : repulsors) {
        const ObjectRepulsorExpiryComponent& expiry =
            repulsors.template get<const ObjectRepulsorExpiryComponent>(
                entity);
        if (expiry.clearAtTick <= confirmedTick) expired.push_back(entity);
    }
    for (const ecs::entity entity : expired) {
        const ObjectRepulsorExpiryComponent* expiry =
            ecs::try_get<ObjectRepulsorExpiryComponent>(registry, entity);
        if (!expiry || expiry->clearAtTick > confirmedTick) continue;
        static_cast<void>(ObjectStatusSystem::apply(
            registry, entity,
            {.clearMask = game::objectStatusBit(
                 game::ObjectStatusFlag::Repulsor),
             .confirmedTick = confirmedTick}));
        ecs::remove<ObjectRepulsorExpiryComponent>(registry, entity);
    }
}

} // namespace object_simulation_detail

} // namespace engine
