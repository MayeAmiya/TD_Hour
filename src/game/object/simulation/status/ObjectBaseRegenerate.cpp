#include "core/container/container_types.h"
#include "game/object/simulation/status/ObjectBaseRegenerate.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

using HealthScalar = ObjectHealthComponent::Scalar;

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max()
        : left + right;
}

[[nodiscard]] bool hasStatus(const ecs::registry& registry, ecs::entity entity,
                             game::ObjectStatusFlag flag) noexcept {
    const ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(registry, entity);
    return status && status->hasAny(game::objectStatusBit(flag));
}

} // namespace

uint64_t ObjectBaseRegenerateSystem::millisecondsToTicks(uint32_t milliseconds,
                                                         uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    // INI::parseDurationUnsignedInt rounds the millisecond conversion up.
    return (static_cast<uint64_t>(milliseconds) * rate + 999u) / 1000u;
}

void ObjectBaseRegenerateSystem::initializeObject(ecs::registry& registry, ecs::entity entity,
                                                  const ObjectSimulationRules& rules,
                                                  uint64_t confirmedTick) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!templateComponent || !templateComponent->archetype ||
        !templateComponent->archetype->baseRegeneratePlan ||
        templateComponent->archetype->baseRegeneratePlan->rules.empty()) {
        return;
    }

    ObjectBaseRegenerateComponent component{
        .plan = templateComponent->archetype->baseRegeneratePlan,
    };
    component.instances.resize(component.plan->rules.size());
    if (rules.baseRegeneration.enabled()) {
        for (ObjectBaseRegenerateRuntime& runtime : component.instances) {
            runtime.nextWakeTick = confirmedTick;
        }
    }

    if (ObjectBaseRegenerateComponent* existing =
            ecs::try_get<ObjectBaseRegenerateComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectBaseRegenerateComponent>(registry, entity, std::move(component));
    }
}

void ObjectBaseRegenerateSystem::onHealthDecreased(ecs::registry& registry,
                                                   ObjectLifecycle& lifecycle,
                                                   ObjectId object,
                                                   uint64_t confirmedTick,
                                                   const ObjectSimulationRules& rules) const {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || lifecycle.isPendingDestroy(object)) return;
    ObjectBaseRegenerateComponent* component =
        ecs::try_get<ObjectBaseRegenerateComponent>(registry, *entity);
    if (!component || !component->plan) return;

    const uint64_t wakeTick = rules.baseRegeneration.enabled()
        ? saturatingAdd(confirmedTick, millisecondsToTicks(
              rules.baseRegeneration.damageDelayMilliseconds, rules.logicFramesPerSecond))
        : ObjectBaseRegenerateRuntime::NeverWakeTick;
    for (ObjectBaseRegenerateRuntime& runtime : component->instances) {
        runtime.nextWakeTick = wakeTick;
    }
}

void ObjectBaseRegenerateSystem::update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                                        const ObjectSimulationRules& rules,
                                        uint64_t confirmedTick,
                                        container::Vector<ObjectDamageRequest>& outDamage) const {
    struct Candidate final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };

    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent, ObjectHealthComponent,
                                ObjectBaseRegenerateComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id)) {
            continue;
        }
        candidates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.id < right.id;
    });

    constexpr uint64_t kHealRateFrames = 3;
    const HealthScalar zero{};
    for (const Candidate& candidate : candidates) {
        ObjectHealthComponent& health = ecs::get<ObjectHealthComponent>(registry, candidate.entity);
        ObjectBaseRegenerateComponent& component =
            ecs::get<ObjectBaseRegenerateComponent>(registry, candidate.entity);
        if (!component.plan || !health.acceptsDamage || health.effectivelyDead) continue;
        if (isObjectDisabled(
                registry, candidate.entity, confirmedTick,
                objectDisabledBit(ObjectDisabledReason::Underpowered))) {
            continue;
        }

        const size_t count = std::min(component.plan->rules.size(), component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectBaseRegenerateRule& rule = component.plan->rules[index];
            ObjectBaseRegenerateRuntime& runtime = component.instances[index];
            if (runtime.nextWakeTick > confirmedTick || runtime.lastUpdateTick == confirmedTick) {
                continue;
            }

            // Source update order intentionally checks construction before
            // SOLD. An under-construction object keeps polling and begins
            // healing immediately once construction clears.
            if (hasStatus(registry, candidate.entity, game::ObjectStatusFlag::UnderConstruction)) {
                runtime.nextWakeTick = confirmedTick;
                continue;
            }
            if (hasStatus(registry, candidate.entity, game::ObjectStatusFlag::Sold) ||
                !rules.baseRegeneration.enabled() || health.maximumFixed <= zero ||
                health.currentFixed >= health.maximumFixed) {
                runtime.nextWakeTick = ObjectBaseRegenerateRuntime::NeverWakeTick;
                continue;
            }

            runtime.lastUpdateTick = confirmedTick;
            runtime.nextWakeTick = saturatingAdd(confirmedTick, kHealRateFrames);

            // RefCode heals `3 * maxHealth * percent / LOGICFRAMES_PER_SECOND`
            // every three frames. Retain the exact cadence while calculating
            // solely in Q32.32. When the pulse is at least a full health bar,
            // submitting the missing amount is transaction-equivalent to the
            // legacy oversized attemptHealing call after Body clipping.
            const uint64_t framesPerSecond = std::max<uint32_t>(1, rules.logicFramesPerSecond);
            const uint64_t fullPulseThresholdRaw =
                ((framesPerSecond << 32u) + (kHealRateFrames - 1u)) / kHealRateFrames;
            const HealthScalar missing = health.maximumFixed - health.currentFixed;
            HealthScalar amount{};
            if (rules.baseRegeneration.healthPercentPerSecond.raw() >=
                static_cast<int64_t>(std::min<uint64_t>(
                    fullPulseThresholdRaw,
                    static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))) {
                amount = missing;
            } else {
                const HealthScalar cadence = HealthScalar::from_fraction(
                    static_cast<int64_t>(kHealRateFrames),
                    static_cast<int64_t>(framesPerSecond));
                amount = health.maximumFixed * (rules.baseRegeneration.healthPercentPerSecond * cadence);
            }
            if (amount <= zero) continue;

            outDamage.push_back({
                .target = candidate.id,
                .source = candidate.id,
                .sourceSequence = rule.authoredOrder,
                .amount = amount,
                .damageType = game::DamageType::HEALING,
                .confirmedTick = confirmedTick,
            });
        }
    }
}

} // namespace engine
