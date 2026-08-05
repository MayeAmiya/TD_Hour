#include "game/object/simulation/structure/ObjectOvercharge.h"

#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/base/DamageTypes.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] math::q32_32 drainAmountPerFrame(
    ObjectHealthComponent::Scalar maximum,
    math::q32_32 percentPerSecond,
    const ObjectSimulationRules& rules) noexcept {
    const uint32_t frames = std::max<uint32_t>(1, rules.logicFramesPerSecond);
    return (maximum * percentPerSecond) / math::q32_32{static_cast<int32_t>(frames)};
}

[[nodiscard]] ObjectPowerPlantExtensionSourceMask overchargeRodBit() noexcept {
    return objectPowerPlantExtensionSourceBit(
        ObjectPowerPlantExtensionSource::Overcharge);
}

[[nodiscard]] ObjectEnergyBonusSourceMask overchargeEnergyBit() noexcept {
    return objectEnergyBonusSourceBit(ObjectEnergyBonusSource::Overcharge);
}

void setOverchargeBits(ecs::registry& registry, ecs::entity entity,
                       bool active) noexcept {
    if (ObjectEnergyComponent* energy =
            ecs::try_get<ObjectEnergyComponent>(registry, entity)) {
        const ObjectEnergyBonusSourceMask bit = overchargeEnergyBit();
        if (bit != 0 && energy->bonusProduction != 0) {
            energy->bonusProductionSources = active
                ? static_cast<ObjectEnergyBonusSourceMask>(
                      energy->bonusProductionSources | bit)
                : static_cast<ObjectEnergyBonusSourceMask>(
                      energy->bonusProductionSources & ~bit);
        }
    }
    if (ObjectPowerPlantComponent* powerPlant =
            ecs::try_get<ObjectPowerPlantComponent>(registry, entity)) {
        const ObjectPowerPlantExtensionSourceMask bit = overchargeRodBit();
        if (bit != 0) {
            powerPlant->extensionSources = active
                ? static_cast<ObjectPowerPlantExtensionSourceMask>(
                      powerPlant->extensionSources | bit)
                : static_cast<ObjectPowerPlantExtensionSourceMask>(
                      powerPlant->extensionSources & ~bit);
        }
    }
}

} // namespace

void ObjectOverchargeSystem::initializeObject(ecs::registry& registry,
                                              ecs::entity entity) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectOverchargePlan> plan =
        templateComponent && templateComponent->archetype
            ? templateComponent->archetype->overchargePlan
            : nullptr;
    if (!plan || plan->rules.empty()) return;

    ObjectOverchargeComponent component;
    component.plan = plan;
    component.instances.resize(plan->rules.size());
    if (ObjectOverchargeComponent* existing =
            ecs::try_get<ObjectOverchargeComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectOverchargeComponent>(registry, entity,
                                                std::move(component));
    }
}

bool ObjectOverchargeSystem::setActive(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, ObjectId object,
    bool active, const ObjectSimulationRules& rules,
    uint64_t confirmedTick) const {
    if (!object || lifecycle.isPendingDestroy(object)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity) return false;

    ObjectOverchargeComponent* component =
        ecs::try_get<ObjectOverchargeComponent>(registry, *entity);
    if (!component || !component->plan ||
        component->instances.size() != component->plan->rules.size()) {
        return false;
    }
    if (component->instances.empty()) return false;
    if (active && isObjectDisabled(registry, *entity, confirmedTick)) {
        return false;
    }
    if (active) {
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *entity);
        if (!health || health->effectivelyDead) return false;
        for (const game::ObjectOverchargeParameters& parameters :
             component->plan->rules) {
            const ObjectHealthComponent::Scalar threshold =
                health->maximumFixed *
                parameters.notAllowedWhenHealthBelowPercent;
            if (health->currentFixed < threshold) return false;
        }
    }

    bool changed = false;
    for (ObjectOverchargeRuntime& runtime : component->instances) {
        if (runtime.active == active) continue;
        runtime.active = active;
        changed = true;
    }
    setOverchargeBits(registry, *entity, active);
    static_cast<void>(rules);
    return changed;
}

bool ObjectOverchargeSystem::toggle(
    ecs::registry& registry, const ObjectLifecycle& lifecycle, ObjectId object,
    const ObjectSimulationRules& rules, uint64_t confirmedTick) const {
    if (!object || lifecycle.isPendingDestroy(object)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity) return false;
    const ObjectOverchargeComponent* component =
        ecs::try_get<ObjectOverchargeComponent>(registry, *entity);
    const bool active =
        component && std::any_of(component->instances.begin(),
                                 component->instances.end(),
                                 [](const ObjectOverchargeRuntime& runtime) {
                                     return runtime.active;
                                 });
    return setActive(registry, lifecycle, object, !active, rules,
                     confirmedTick);
}

bool ObjectOverchargeSystem::isActive(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object) const noexcept {
    if (!object || lifecycle.isPendingDestroy(object)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity) return false;
    const ObjectOverchargeComponent* component =
        ecs::try_get<ObjectOverchargeComponent>(registry, *entity);
    return component &&
           std::any_of(component->instances.begin(), component->instances.end(),
                       [](const ObjectOverchargeRuntime& runtime) {
                           return runtime.active;
                       });
}

void ObjectOverchargeSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage) const {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectHealthComponent,
                                ObjectOverchargeComponent>(registry);
    candidates.reserve(view.size_hint());
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
        ObjectOverchargeComponent& component =
            ecs::get<ObjectOverchargeComponent>(registry, candidate.entity);
        if (!component.plan ||
            component.instances.size() != component.plan->rules.size()) {
            continue;
        }
        ObjectHealthComponent& health =
            ecs::get<ObjectHealthComponent>(registry, candidate.entity);
        const bool disabled =
            isObjectDisabled(registry, candidate.entity, confirmedTick);
        ObjectHealthComponent::Scalar projectedHealth = health.currentFixed;
        bool anyActive = false;
        bool anyStillActive = false;
        for (size_t index = 0; index < component.plan->rules.size(); ++index) {
            ObjectOverchargeRuntime& runtime = component.instances[index];
            if (!runtime.active) continue;
            anyActive = true;
            const game::ObjectOverchargeParameters& parameters =
                component.plan->rules[index];
            const ObjectHealthComponent::Scalar threshold =
                health.maximumFixed *
                parameters.notAllowedWhenHealthBelowPercent;
            if (disabled || health.effectivelyDead ||
                health.currentFixed < threshold) {
                runtime.active = false;
                continue;
            }

            const math::q32_32 amount = drainAmountPerFrame(
                health.maximumFixed,
                parameters.healthPercentToDrainPerSecond, rules);
            if (amount > math::q32_32{}) {
                outDamage.push_back({
                    .target = candidate.object,
                    .source = candidate.object,
                    .sourceSequence = parameters.authoredOrder,
                    .amount = amount,
                    .damageType = game::DamageType::PENALTY,
                    .deathType = game::DeathType::NORMAL,
                    .confirmedTick = confirmedTick,
                });
            }
            projectedHealth = projectedHealth > amount
                ? projectedHealth - amount
                : ObjectHealthComponent::Scalar{};
            if (projectedHealth < threshold) {
                runtime.active = false;
            } else {
                anyStillActive = true;
            }
        }
        if (anyActive && !anyStillActive) {
            setOverchargeBits(registry, candidate.entity, false);
        } else if (anyStillActive) {
            setOverchargeBits(registry, candidate.entity, true);
        }
    }
}

} // namespace engine
