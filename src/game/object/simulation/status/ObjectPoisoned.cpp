#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/status/ObjectPoisoned.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/runtime/ObjectHealthEvents.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <optional>
namespace engine {
namespace {

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

void clearPoison(ObjectPoisonedRuntime& runtime) noexcept {
    runtime = {};
}

} // namespace

bool ObjectPoisonedComponent::hasActivePoison() const noexcept {
    return std::any_of(instances.begin(), instances.end(),
                       [](const ObjectPoisonedRuntime& runtime) {
                           return runtime.active;
                       });
}

uint64_t ObjectPoisonedSystem::millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * rate + 999u) / 1000u;
}

void ObjectPoisonedSystem::initializeObject(ecs::registry& registry,
                                            ecs::entity entity) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectPoisonedPlan> plan =
        templateComponent && templateComponent->archetype
            ? templateComponent->archetype->poisonedPlan : nullptr;
    if (!plan || plan->rules.empty()) return;

    ObjectPoisonedComponent component;
    component.plan = plan;
    component.instances.resize(plan->rules.size());
    if (ObjectPoisonedComponent* existing =
            ecs::try_get<ObjectPoisonedComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectPoisonedComponent>(registry, entity,
                                              std::move(component));
    }
}

void ObjectPoisonedSystem::onHealthEvent(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectHealthEvent& event, uint32_t logicFramesPerSecond) const {
    if (!event.object) return;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(event.object);
    if (!entity) return;
    ObjectPoisonedComponent* component =
        ecs::try_get<ObjectPoisonedComponent>(registry, *entity);
    if (!component || !component->plan ||
        component->instances.size() != component->plan->rules.size()) return;

    if (event.kind == ObjectHealthEventKind::Healed &&
        event.appliedAmountFixed > math::q32_32{}) {
        for (ObjectPoisonedRuntime& runtime : component->instances) {
            clearPoison(runtime);
        }
        return;
    }
    if (event.kind != ObjectHealthEventKind::Damaged ||
        event.damageType != game::DamageType::POISON ||
        !event.healthDecreased ||
        event.actualDamageDealtFixed <= math::q32_32{}) {
        return;
    }

    for (size_t index = 0; index < component->plan->rules.size(); ++index) {
        const game::ObjectPoisonedParameters& parameters =
            component->plan->rules[index];
        ObjectPoisonedRuntime& runtime = component->instances[index];
        const uint64_t next = saturatingAdd(
            event.confirmedTick,
            millisecondsToTicks(parameters.poisonDamageIntervalMilliseconds,
                                logicFramesPerSecond));
        runtime.nextDamageTick = runtime.active
            ? std::min(runtime.nextDamageTick, next) : next;
        runtime.stopTick = saturatingAdd(
            event.confirmedTick,
            millisecondsToTicks(parameters.poisonDurationMilliseconds,
                                logicFramesPerSecond));
        runtime.damageAmount = event.actualDamageDealtFixed;
        runtime.source = event.source;
        runtime.deathType = event.deathType;
        runtime.active = true;
    }
}

void ObjectPoisonedSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage) const {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectPoisonedComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id && !lifecycle.isPendingDestroy(identity.id)) {
            candidates.push_back({.object = identity.id, .entity = entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    for (const Candidate& candidate : candidates) {
        ObjectPoisonedComponent& component =
            ecs::get<ObjectPoisonedComponent>(registry, candidate.entity);
        if (!component.plan ||
            component.instances.size() != component.plan->rules.size()) continue;
        for (size_t index = 0; index < component.plan->rules.size(); ++index) {
            ObjectPoisonedRuntime& runtime = component.instances[index];
            if (!runtime.active || runtime.lastDamageTick == confirmedTick ||
                confirmedTick < runtime.nextDamageTick) continue;
            const game::ObjectPoisonedParameters& parameters =
                component.plan->rules[index];
            outDamage.push_back({
                .target = candidate.object,
                .source = runtime.source,
                .sourceSequence = parameters.authoredOrder,
                .amount = runtime.damageAmount,
                .damageType = game::DamageType::UNRESISTABLE,
                .damageFxOverride = game::DamageType::POISON,
                .deathType = runtime.deathType,
                .confirmedTick = confirmedTick,
            });
            runtime.lastDamageTick = confirmedTick;
            runtime.nextDamageTick = saturatingAdd(
                confirmedTick,
                millisecondsToTicks(
                    parameters.poisonDamageIntervalMilliseconds,
                    logicFramesPerSecond));
        }
    }
}

void ObjectPoisonedSystem::finishUpdate(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) const {
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectPoisonedComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        ObjectPoisonedComponent& component =
            view.template get<ObjectPoisonedComponent>(entity);
        for (ObjectPoisonedRuntime& runtime : component.instances) {
            if (!runtime.active || confirmedTick < runtime.stopTick) continue;
            // RefCode deliberately keeps the poisoned tint/state after a
            // lethal final pulse. A living object clears at the same boundary.
            if (health && health->effectivelyDead) continue;
            clearPoison(runtime);
        }
    }
}

} // namespace engine
