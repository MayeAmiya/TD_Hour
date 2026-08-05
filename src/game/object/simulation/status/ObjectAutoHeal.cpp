#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/status/ObjectAutoHeal.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOwnershipIndex.h"

namespace engine
{
namespace
{

using container::asciiEqualIgnoreCase;

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept
{
    return right > std::numeric_limits<uint64_t>::max() - left ? std::numeric_limits<uint64_t>::max() : left + right;
}

[[nodiscard]] bool objectMatchesKindExpression(
    const ecs::registry& registry, ecs::entity entity,
    const game::ObjectKindOfMask& expression, bool emptyResult) noexcept {
    if (expression.none()) return emptyResult;
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, entity);
    return kinds && kinds->mask.test_for_any(expression);
}

[[nodiscard]] bool sameMapStatus(const ecs::registry& registry,
                                 ecs::entity left,
                                 ecs::entity right) noexcept {
    const ObjectMapStatusComponent* leftStatus =
        ecs::try_get<ObjectMapStatusComponent>(registry, left);
    const ObjectMapStatusComponent* rightStatus =
        ecs::try_get<ObjectMapStatusComponent>(registry, right);
    return (leftStatus && leftStatus->offMap) ==
           (rightStatus && rightStatus->offMap);
}

[[nodiscard]] bool claimSoleHealingBenefactor(
    ecs::registry& registry, ecs::entity target, ObjectId source,
    uint64_t confirmedTick, uint64_t durationTicks) {
    ObjectSoleHealingBenefactorComponent* lease =
        ecs::try_get<ObjectSoleHealingBenefactorComponent>(registry, target);
    if (!lease) {
        lease = &ecs::emplace<ObjectSoleHealingBenefactorComponent>(
            registry, target);
    }
    // RefCode uses `now > expiration`, so the previous benefactor owns the
    // exact expiration frame too.
    if (lease->source && lease->source != source &&
        confirmedTick <= lease->expiresTick) {
        return false;
    }
    lease->source = source;
    lease->expiresTick = saturatingAdd(confirmedTick, durationTicks);
    return true;
}

void appendParticleEvent(
    container::Vector<ObjectAutoHealParticleEvent>& output,
    ObjectAutoHealParticleEventKind kind, ObjectId source, ObjectId target,
    const container::String& particleSystem, uint32_t authoredOrder,
    uint64_t confirmedTick) {
    if (particleSystem.empty()) return;
    output.push_back({
        .kind = kind,
        .source = source,
        .target = target,
        .particleSystem = particleSystem,
        .authoredOrder = authoredOrder,
        .confirmedTick = confirmedTick,
    });
}

[[nodiscard]] bool upgradeActivationBlocked(const ecs::registry& registry,
                                            ecs::entity entity) noexcept {
    const ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(registry, entity);
    return status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
        game::objectStatusBit(game::ObjectStatusFlag::Destroyed));
}

} // namespace

bool ObjectAutoHealSystem::stopFirstAuthored(
    ecs::registry& registry, ecs::entity entity) noexcept {
    ObjectAutoHealComponent* component =
        ecs::try_get<ObjectAutoHealComponent>(registry, entity);
    if (!component || !component->plan) return false;
    const size_t count =
        std::min(component->plan->rules.size(), component->instances.size());
    if (count == 0) return false;

    // compileObjectAutoHealPlan preserves authored order. RefCode's
    // findUpdateModule("AutoHealBehavior") therefore maps to entry zero,
    // including when that entry is already stopped or upgrade-dormant.
    ObjectAutoHealRuntime& runtime = component->instances.front();
    runtime.stopped = true;
    runtime.nextWakeTick = ObjectAutoHealRuntime::NeverWakeTick;
    runtime.soonestHealTick = ObjectAutoHealRuntime::NeverWakeTick;
    return true;
}

uint64_t ObjectAutoHealSystem::millisecondsToTicks(uint32_t milliseconds, uint32_t framesPerSecond) noexcept
{
    if (milliseconds == 0)
        return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    // RefCode's parseDurationUnsignedInt maps a partial logic frame upward.
    return (static_cast<uint64_t>(milliseconds) * rate + 999u) / 1000u;
}

uint64_t ObjectAutoHealSystem::initialPhaseDelay(uint64_t delayTicks, SimulationRandom& random) noexcept
{
    // GameLogicRandomValue(1, HealingDelay) phases StartsActive instances.
    // The legacy random helper is 32-bit, so a content value beyond that
    // domain is deliberately clamped rather than overflowing a signed range.
    if (delayTicks == 0)
        return 0;
    const uint64_t maximum =
        std::min<uint64_t>(delayTicks, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
    return 1u + static_cast<uint64_t>(random.nextUInt32()) % maximum;
}

void ObjectAutoHealSystem::activateEligible(ObjectAutoHealComponent& component,
                                            const UpgradeMask& completedUpgrades,
                                            uint64_t confirmedTick,
                                            const UpgradeCatalog* catalog) noexcept
{
    if (!component.plan)
        return;
    const size_t count = std::min(component.plan->rules.size(), component.instances.size());
    for (size_t index = 0; index < count; ++index)
    {
        const game::ObjectAutoHealParameters& parameters = component.plan->rules[index];
        ObjectAutoHealRuntime& runtime = component.instances[index];
        if (runtime.upgradeActivated || runtime.stopped)
            continue;
        if (!game::objectAutoHealUpgradeMatches(parameters, completedUpgrades, catalog))
            continue;
        // UpgradeMux::upgradeImplementation() wakes the module immediately;
        // unlike StartsActive, an upgrade transition has no random phase.
        runtime.upgradeActivated = true;
        runtime.active = true;
        runtime.nextWakeTick = confirmedTick;
    }
}

void ObjectAutoHealSystem::initializeObject(ecs::registry& registry,
                                            ecs::entity entity,
                                            const UpgradeMask& ownerCompletedUpgrades,
                                            SimulationRandom& random,
                                            const ObjectSimulationRules& rules,
                                            uint64_t confirmedTick) const
{
    const ThingTemplateComponent* templateComponent = ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!templateComponent || !templateComponent->archetype || !templateComponent->archetype->autoHealPlan ||
        templateComponent->archetype->autoHealPlan->rules.empty())
    {
        return;
    }

    ObjectAutoHealComponent component;
    component.plan = templateComponent->archetype->autoHealPlan;
    component.instances.resize(component.plan->rules.size());
    for (size_t index = 0; index < component.plan->rules.size(); ++index)
    {
        const game::ObjectAutoHealParameters& parameters = component.plan->rules[index];
        ObjectAutoHealRuntime& runtime = component.instances[index];
        if (!parameters.startsActive)
            continue;
        // The legacy constructor consumes GameLogicRandomValue for every
        // StartsActive AutoHeal module, including radius and whole-player
        // variants. Preserve that shared simulation-RNG ordering.
        const uint64_t phaseDelay = initialPhaseDelay(
            millisecondsToTicks(parameters.healingDelayMilliseconds, rules.logicFramesPerSecond), random);
        runtime.upgradeActivated = true;
        runtime.active = true;
        runtime.nextWakeTick = saturatingAdd(confirmedTick, phaseDelay);
    }
    // Structural spawn has no sealed catalog pointer yet; StartsActive rules
    // already activated above. Mux wake for player tech waits for fan-out /
    // owner-change paths that carry the catalog.
    activateEligible(component, ownerCompletedUpgrades, confirmedTick, nullptr);

    if (ObjectAutoHealComponent* existing = ecs::try_get<ObjectAutoHealComponent>(registry, entity))
    {
        *existing = std::move(component);
    }
    else
    {
        ecs::emplace<ObjectAutoHealComponent>(registry, entity, std::move(component));
    }
}

void ObjectAutoHealSystem::onPlayerUpgradeCompleted(ecs::registry& registry,
                                                    ObjectLifecycle& lifecycle,
                                                    const ObjectOwnershipIndex& ownership,
                                                    PlayerId player,
                                                    const UpgradeMask& completedUpgrades,
                                                    uint64_t confirmedTick,
                                                    const UpgradeCatalog* catalog) const
{
    if (!player)
        return;
    for (const ObjectId object : ownership.objects(player))
    {
        const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
        if (!entity || lifecycle.isPendingDestroy(object))
            continue;
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, *entity);
        ObjectAutoHealComponent* component = ecs::try_get<ObjectAutoHealComponent>(registry, *entity);
        if (!owner || owner->player != player || !component ||
            upgradeActivationBlocked(registry, *entity))
            continue;
        activateEligible(*component, completedUpgrades, confirmedTick, catalog);
    }
}

void ObjectAutoHealSystem::onObjectOwnerChanged(
    ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId object,
    const UpgradeMask& completedUpgrades, uint64_t confirmedTick,
    const UpgradeCatalog* catalog) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || lifecycle.isPendingDestroy(object) ||
        upgradeActivationBlocked(registry, *entity)) {
        return;
    }
    ObjectAutoHealComponent* component =
        ecs::try_get<ObjectAutoHealComponent>(registry, *entity);
    if (!component) return;
    activateEligible(*component, completedUpgrades, confirmedTick, catalog);
}

void ObjectAutoHealSystem::onHealthDecreased(ecs::registry& registry,
                                             ObjectLifecycle& lifecycle,
                                             ObjectId object,
                                             uint64_t confirmedTick,
                                             const ObjectSimulationRules& rules) const
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || lifecycle.isPendingDestroy(object))
        return;
    ObjectAutoHealComponent* component = ecs::try_get<ObjectAutoHealComponent>(registry, *entity);
    if (!component || !component->plan)
        return;

    const size_t count = std::min(component->plan->rules.size(), component->instances.size());
    for (size_t index = 0; index < count; ++index)
    {
        const game::ObjectAutoHealParameters& parameters = component->plan->rules[index];
        ObjectAutoHealRuntime& runtime = component->instances[index];
        if (!runtime.upgradeActivated || !runtime.active || runtime.stopped)
            continue;

        const uint64_t startDelay =
            millisecondsToTicks(parameters.startHealingDelayMilliseconds, rules.logicFramesPerSecond);
        if (startDelay > 0)
        {
            // Every real hit resets StartHealingDelay, matching the original
            // damage callback's new wake frame.
            runtime.nextWakeTick = saturatingAdd(confirmedTick, startDelay);
        }
        else if (confirmedTick > runtime.soonestHealTick)
        {
            // Strictly greater is intentional: it prevents repeated damage at
            // the same scheduled heal frame from manufacturing extra pulses.
            runtime.nextWakeTick = confirmedTick;
        }
    }
}

void ObjectAutoHealSystem::update(ecs::registry& registry,
                                  ObjectLifecycle& lifecycle,
                                  const PlayerRegistry* players,
                                  const ObjectSimulationRules& rules,
                                  uint64_t confirmedTick,
                                  container::Vector<ObjectDamageRequest>& outDamage,
                                  container::Vector<ObjectAutoHealParticleEvent>& outParticles) const
{
    struct Candidate final
    {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view =
        ecs::view<const ObjectIdentityComponent, ObjectHealthComponent, ObjectAutoHealComponent>(registry);
    for (const ecs::entity entity : view)
    {
        const ObjectIdentityComponent& identity = view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) || lifecycle.isPendingDestroy(identity.id))
        {
            continue;
        }
        candidates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(),
              candidates.end(),
              [](const Candidate& left, const Candidate& right) { return left.id < right.id; });

    container::Vector<Candidate> targets;
    const auto targetView =
        ecs::view<const ObjectIdentityComponent, ObjectHealthComponent>(registry);
    targets.reserve(targetView.size_hint());
    for (const ecs::entity entity : targetView) {
        const ObjectId id = targetView
            .template get<const ObjectIdentityComponent>(entity).id;
        if (!id || !lifecycle.entityFromId(id) ||
            lifecycle.isPendingDestroy(id)) {
            continue;
        }
        targets.push_back({.id = id, .entity = entity});
    }
    std::sort(targets.begin(), targets.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.id < right.id;
              });

    for (const Candidate& candidate : candidates)
    {
        ObjectHealthComponent& health = ecs::get<ObjectHealthComponent>(registry, candidate.entity);
        ObjectAutoHealComponent& component = ecs::get<ObjectAutoHealComponent>(registry, candidate.entity);
        if (!component.plan || !health.acceptsDamage || health.effectivelyDead)
            continue;
        const ObjectDisabledMask disabled =
            objectDisabledMask(registry, candidate.entity, confirmedTick);
        // RefCode explicitly allows AutoHeal to process DISABLED_HELD, while
        // every other disabled reason still suspends the update.
        if ((disabled & ~objectDisabledBit(ObjectDisabledReason::Held)) != 0) {
            continue;
        }

        const OwnerComponent* healerOwner =
            ecs::try_get<OwnerComponent>(registry, candidate.entity);
        const TransformComponent* healerTransform =
            ecs::try_get<TransformComponent>(registry, candidate.entity);
        const LogicFixedVec3 healerPosition = healerTransform
            ? readAuthoritativeObjectPosition(
                  registry, candidate.entity, *healerTransform)
            : LogicFixedVec3{};

        const size_t count = std::min(component.plan->rules.size(), component.instances.size());
        for (size_t index = 0; index < count; ++index)
        {
            const game::ObjectAutoHealParameters& parameters = component.plan->rules[index];
            ObjectAutoHealRuntime& runtime = component.instances[index];
            if (!runtime.upgradeActivated || !runtime.active || runtime.stopped ||
                runtime.nextWakeTick > confirmedTick || runtime.lastUpdateTick == confirmedTick)
            {
                continue;
            }
            runtime.lastUpdateTick = confirmedTick;

            if (!runtime.radiusEmitterActive &&
                !parameters.radiusParticleSystemName.empty()) {
                runtime.radiusEmitterActive = true;
                appendParticleEvent(
                    outParticles,
                    ObjectAutoHealParticleEventKind::RadiusBegin,
                    candidate.id, candidate.id,
                    parameters.radiusParticleSystemName,
                    parameters.authoredOrder, confirmedTick);
            }

            const uint64_t healingDelay =
                millisecondsToTicks(parameters.healingDelayMilliseconds, rules.logicFramesPerSecond);
            const uint64_t nextPulseTick =
                saturatingAdd(confirmedTick, healingDelay);

            const auto pulse = [&](const Candidate& target,
                                   bool useSoleBenefactor) {
                ObjectHealthComponent& targetHealth =
                    ecs::get<ObjectHealthComponent>(registry, target.entity);
                if (!targetHealth.acceptsDamage ||
                    targetHealth.effectivelyDead ||
                    targetHealth.currentFixed >= targetHealth.maximumFixed) {
                    return;
                }
                const bool accepted = !useSoleBenefactor ||
                    claimSoleHealingBenefactor(
                        registry, target.entity, candidate.id,
                        confirmedTick, healingDelay);
                if (accepted && parameters.healingAmount > math::q32_32{}) {
                    outDamage.push_back({
                        .target = target.id,
                        .source = candidate.id,
                        .sourceSequence = parameters.authoredOrder,
                        .amount = parameters.healingAmount,
                        .damageType = game::DamageType::HEALING,
                        .confirmedTick = confirmedTick,
                    });
                }
                // RefCode creates the per-unit pulse after attempting the
                // heal, including a rejected sole-benefactor attempt.
                appendParticleEvent(
                    outParticles,
                    ObjectAutoHealParticleEventKind::UnitPulse,
                    candidate.id, target.id,
                    parameters.unitHealPulseParticleSystemName,
                    parameters.authoredOrder, confirmedTick);
                runtime.soonestHealTick = nextPulseTick;
            };

            if (!parameters.affectsWholePlayer &&
                parameters.radius == math::q32_32{}) {
                if (health.currentFixed >= health.maximumFixed) {
                    // Original self-heal sleeps forever at full health and
                    // wakes only on the next actual damage callback.
                    runtime.nextWakeTick = ObjectAutoHealRuntime::NeverWakeTick;
                    continue;
                }
                pulse(candidate, false);
                runtime.nextWakeTick = nextPulseTick;
                continue;
            }

            if (parameters.affectsWholePlayer) {
                if (healerOwner && healerOwner->player) {
                    for (const Candidate& target : targets) {
                        const OwnerComponent* targetOwner =
                            ecs::try_get<OwnerComponent>(registry,
                                                         target.entity);
                        const ObjectMapStatusComponent* mapStatus =
                            ecs::try_get<ObjectMapStatusComponent>(
                                registry, target.entity);
                        if (!targetOwner ||
                            targetOwner->player != healerOwner->player ||
                            (mapStatus && mapStatus->offMap) ||
                            (parameters.skipSelfForHealing &&
                             target.id == candidate.id) ||
                            !objectMatchesKindExpression(
                                registry, target.entity,
                                parameters.kindOfMask, true) ||
                            objectMatchesKindExpression(
                                registry, target.entity,
                                parameters.forbiddenKindOfMask, false)) {
                            continue;
                        }
                        pulse(target,
                              parameters.radius != math::q32_32{});
                    }
                }
                // RefCode ignores SingleBurst for AffectsWholePlayer.
                runtime.nextWakeTick = nextPulseTick;
                continue;
            }

            if (healerTransform &&
                parameters.radius > math::q32_32{}) {
                const math::q32_32 radiusSquared =
                    parameters.radius * parameters.radius;
                for (const Candidate& target : targets) {
                    const TransformComponent* targetTransform =
                        ecs::try_get<TransformComponent>(registry,
                                                         target.entity);
                    if (!targetTransform ||
                        !sameMapStatus(registry, candidate.entity,
                                       target.entity) ||
                        (parameters.skipSelfForHealing &&
                         target.id == candidate.id) ||
                        !objectMatchesKindExpression(
                            registry, target.entity,
                            parameters.kindOfMask, true) ||
                        objectMatchesKindExpression(
                            registry, target.entity,
                            parameters.forbiddenKindOfMask, false)) {
                        continue;
                    }
                    if (players) {
                        if (relationshipBetweenObjects(
                                registry, *players, candidate.entity,
                                target.entity) !=
                            PlayerRelationship::Allies) {
                            continue;
                        }
                    } else {
                        const OwnerComponent* targetOwner =
                            ecs::try_get<OwnerComponent>(registry,
                                                         target.entity);
                        if (!healerOwner || !targetOwner ||
                            healerOwner->player != targetOwner->player) {
                            continue;
                        }
                    }
                    const LogicFixedVec3 targetPosition =
                        readAuthoritativeObjectPosition(
                            registry, target.entity, *targetTransform);
                    const math::q32_32 dx =
                        targetPosition.x - healerPosition.x;
                    const math::q32_32 dy =
                        targetPosition.y - healerPosition.y;
                    if (dx * dx + dy * dy > radiusSquared) continue;
                    pulse(target, true);
                }
            }
            runtime.nextWakeTick = parameters.singleBurst
                ? ObjectAutoHealRuntime::NeverWakeTick
                : nextPulseTick;
        }
    }
}

void ObjectAutoHealSystem::onObjectReclaim(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick,
    container::Vector<ObjectAutoHealParticleEvent>& outParticles) const {
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(object);
    ObjectAutoHealComponent* component = entity
        ? ecs::try_get<ObjectAutoHealComponent>(registry, *entity)
        : nullptr;
    if (!component || !component->plan) return;
    const size_t count =
        std::min(component->plan->rules.size(), component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectAutoHealRuntime& runtime = component->instances[index];
        if (!runtime.radiusEmitterActive) continue;
        runtime.radiusEmitterActive = false;
        const game::ObjectAutoHealParameters& parameters =
            component->plan->rules[index];
        appendParticleEvent(
            outParticles, ObjectAutoHealParticleEventKind::RadiusEnd,
            object, object, parameters.radiusParticleSystemName,
            parameters.authoredOrder, confirmedTick);
    }
}

} // namespace engine
