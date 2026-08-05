#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/structure/ObjectSupplyWarehouseCrippling.h"
#include "game/object/simulation/runtime/ObjectHealthEvents.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

using HealthScalar = ObjectHealthComponent::Scalar;

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

void setWarehouseDockCrippled(ecs::registry& registry, ecs::entity entity,
                              bool crippled) {
    ObjectDockCrippleComponent* dock =
        ecs::try_get<ObjectDockCrippleComponent>(registry, entity);
    if (!dock) {
        dock = &ecs::emplace<ObjectDockCrippleComponent>(registry, entity);
    }
    dock->set(ObjectDockCrippleReason::SupplyWarehouseReallyDamaged,
              crippled);
}

} // namespace

uint64_t ObjectSupplyWarehouseCripplingSystem::millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    return (static_cast<uint64_t>(milliseconds) * rate + 999u) / 1000u;
}

void ObjectSupplyWarehouseCripplingSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const ObjectSimulationRules& rules) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    const container::SharedPtr<const game::ObjectSupplyWarehouseCripplingPlan>
        plan = templateComponent && templateComponent->archetype
            ? templateComponent->archetype->supplyWarehouseCripplingPlan
            : nullptr;
    if (!plan || plan->rules.empty()) return;

    ObjectSupplyWarehouseCripplingComponent component;
    component.plan = plan;
    component.instances.reserve(plan->rules.size());
    for (const game::ObjectSupplyWarehouseCripplingParameters& rule :
         plan->rules) {
        component.instances.push_back({
            .selfHealSuppressionTicks = millisecondsToTicks(
                rule.selfHealSuppressionMilliseconds,
                rules.logicFramesPerSecond),
            .selfHealDelayTicks = millisecondsToTicks(
                rule.selfHealDelayMilliseconds, rules.logicFramesPerSecond),
        });
    }

    if (ObjectSupplyWarehouseCripplingComponent* existing =
            ecs::try_get<ObjectSupplyWarehouseCripplingComponent>(registry,
                                                                   entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectSupplyWarehouseCripplingComponent>(
            registry, entity, std::move(component));
    }

    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    setWarehouseDockCrippled(
        registry, entity,
        health && health->damageState == ObjectBodyDamageState::ReallyDamaged);
}

void ObjectSupplyWarehouseCripplingSystem::onHealthEvent(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectHealthEvent& event) const {
    if (!event.object) return;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(event.object);
    if (!entity) return;
    ObjectSupplyWarehouseCripplingComponent* component =
        ecs::try_get<ObjectSupplyWarehouseCripplingComponent>(registry,
                                                               *entity);
    if (!component || !component->plan) return;

    if (event.healthDecreased) {
        for (ObjectSupplyWarehouseCripplingRuntime& runtime :
             component->instances) {
            runtime.nextHealingTick = saturatingAdd(
                event.confirmedTick, runtime.selfHealSuppressionTicks);
        }
    }

    if (event.kind == ObjectHealthEventKind::DamageStateChanged) {
        setWarehouseDockCrippled(
            registry, *entity,
            event.currentState == ObjectBodyDamageState::ReallyDamaged);
    }

    // The source module discovers this after its own healing pulse. Observing
    // the committed Body fact here also avoids one redundant no-op wake when
    // another legitimate healer fills the warehouse first.
    if (event.kind == ObjectHealthEventKind::Healed) {
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *entity);
        if (health && health->currentFixed >= health->maximumFixed) {
            for (ObjectSupplyWarehouseCripplingRuntime& runtime :
                 component->instances) {
                runtime.nextHealingTick =
                    ObjectSupplyWarehouseCripplingRuntime::NeverWakeTick;
            }
        }
    }
}

void ObjectSupplyWarehouseCripplingSystem::update(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage) const {
    struct Candidate final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };

    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectHealthComponent,
                                ObjectSupplyWarehouseCripplingComponent>(
        registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || lifecycle.isPendingDestroy(identity.id)) continue;
        candidates.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.id < right.id;
              });

    const HealthScalar zero{};
    for (const Candidate& candidate : candidates) {
        ObjectHealthComponent& health =
            ecs::get<ObjectHealthComponent>(registry, candidate.entity);
        ObjectSupplyWarehouseCripplingComponent& component =
            ecs::get<ObjectSupplyWarehouseCripplingComponent>(registry,
                                                               candidate.entity);
        if (!component.plan || !health.acceptsDamage ||
            health.effectivelyDead || health.maximumFixed <= zero) {
            for (ObjectSupplyWarehouseCripplingRuntime& runtime :
                 component.instances) {
                runtime.nextHealingTick =
                    ObjectSupplyWarehouseCripplingRuntime::NeverWakeTick;
            }
            continue;
        }
        // SupplyWarehouseCripplingBehavior uses the default UpdateModule
        // disabled policy: every disabled reason pauses callbacks, while the
        // absolute wake deadline remains intact and fires immediately after
        // the object becomes enabled again.
        if (isObjectDisabled(registry, candidate.entity, confirmedTick)) {
            continue;
        }

        const size_t count =
            std::min(component.plan->rules.size(), component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            ObjectSupplyWarehouseCripplingRuntime& runtime =
                component.instances[index];
            if (health.currentFixed >= health.maximumFixed) {
                runtime.nextHealingTick =
                    ObjectSupplyWarehouseCripplingRuntime::NeverWakeTick;
                continue;
            }
            if (runtime.nextHealingTick > confirmedTick) continue;

            const game::ObjectSupplyWarehouseCripplingParameters& rule =
                component.plan->rules[index];
            runtime.nextHealingTick =
                saturatingAdd(confirmedTick, runtime.selfHealDelayTicks);
            if (rule.selfHealAmount <= zero) continue;

            outDamage.push_back({
                .target = candidate.id,
                // RefCode calls attemptHealing(amount, nullptr).
                .source = INVALID_OBJECT_ID,
                .sourceSequence = rule.authoredOrder,
                .amount = rule.selfHealAmount,
                .damageType = game::DamageType::HEALING,
                .confirmedTick = confirmedTick,
            });
        }
    }
}

} // namespace engine
