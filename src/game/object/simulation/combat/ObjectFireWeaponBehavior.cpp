#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/object/simulation/combat/ObjectFireWeaponBehavior.h"

#include "game/base/SimulationRandom.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectHealthEvents.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
namespace engine {
namespace {

[[nodiscard]] size_t bodyStateIndex(ObjectBodyDamageState state) noexcept {
    return std::min<size_t>(static_cast<size_t>(state), 3u);
}

void advanceEmissionSequence(uint64_t& sequence) noexcept {
    ++sequence;
    if (sequence == 0) ++sequence;
}

} // namespace

void ObjectFireWeaponBehaviorSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const GameContentSnapshot& content, uint32_t logicFramesPerSecond,
    uint64_t confirmedTick) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!templateComponent || !templateComponent->archetype) return;

    if (const auto& plan =
            templateComponent->archetype->fireWeaponWhenDamagedPlan;
        plan && !plan->rules.empty()) {
        ObjectFireWeaponWhenDamagedComponent component;
        component.plan = plan;
        component.instances.resize(plan->rules.size());
        for (size_t ruleIndex = 0; ruleIndex < plan->rules.size(); ++ruleIndex) {
            const game::ObjectFireWeaponWhenDamagedParameters& parameters =
                plan->rules[ruleIndex];
            ObjectFireWeaponWhenDamagedRuntime& runtime =
                component.instances[ruleIndex];
            runtime.upgradeActivated = parameters.startsActive;
            for (size_t state = 0; state < 4; ++state) {
                if (!parameters.reactionWeapons[state].empty()) {
                    static_cast<void>(initializeObjectSystemWeaponRuntime(
                        runtime.reactionWeapons[state],
                        parameters.reactionWeapons[state], registry, entity,
                        content, logicFramesPerSecond, confirmedTick));
                }
                if (!parameters.continuousWeapons[state].empty()) {
                    static_cast<void>(initializeObjectSystemWeaponRuntime(
                        runtime.continuousWeapons[state],
                        parameters.continuousWeapons[state], registry, entity,
                        content, logicFramesPerSecond, confirmedTick));
                }
            }
        }
        ecs::emplace<ObjectFireWeaponWhenDamagedComponent>(
            registry, entity, std::move(component));
    }

    const ObjectDeathReactionComponent* reactions =
        ecs::try_get<ObjectDeathReactionComponent>(registry, entity);
    if (!reactions || !reactions->plan) return;
    ObjectFireWeaponWhenDeadRuntimeComponent dead;
    dead.rules.resize(reactions->plan->rules.size());
    bool any = false;
    for (size_t index = 0; index < reactions->plan->rules.size(); ++index) {
        const game::ObjectDeathReactionRule& rule =
            reactions->plan->rules[index];
        if (rule.kind != game::ObjectDeathReactionKind::FireWeaponWhenDead ||
            !rule.fireWeaponWhenDead) continue;
        any = true;
        ObjectFireWeaponWhenDeadRuleRuntime& runtime = dead.rules[index];
        runtime.content = content.findWeaponId(
            rule.fireWeaponWhenDead->deathWeapon);
        runtime.upgradeActivated = rule.fireWeaponWhenDead->startsActive;
    }
    if (any) {
        ecs::emplace<ObjectFireWeaponWhenDeadRuntimeComponent>(
            registry, entity, std::move(dead));
    }
}

void ObjectFireWeaponBehaviorSystem::onHealthEvent(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const ObjectHealthEvent& event,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint64_t& nextEmissionSequence,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands) const {
    if (event.kind != ObjectHealthEventKind::Damaged ||
        !event.healthDecreased || !event.object) return;
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromIdIncludingPending(event.object);
    if (!entity) return;
    ObjectFireWeaponWhenDamagedComponent* component =
        ecs::try_get<ObjectFireWeaponWhenDamagedComponent>(registry, *entity);
    if (!component || !component->plan) return;
    const uint8_t damageIndex = static_cast<uint8_t>(event.damageType);
    const uint64_t damageBit = damageIndex < 64 ? uint64_t{1} << damageIndex : 0;
    const size_t state = bodyStateIndex(event.currentState);
    const size_t count = std::min(component->plan->rules.size(),
                                  component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectFireWeaponWhenDamagedParameters& parameters =
            component->plan->rules[index];
        ObjectFireWeaponWhenDamagedRuntime& runtime =
            component->instances[index];
        if (!runtime.upgradeActivated ||
            (parameters.damageTypeMask & damageBit) == 0 ||
            event.actualDamageDealtFixed < parameters.damageAmount ||
            !runtime.reactionWeapons[state].content) continue;
        const uint64_t emission = nextEmissionSequence;
        if (tryQueueObjectSystemWeaponFire(
                runtime.reactionWeapons[state], registry, *entity,
                event.object, content, random, logicFramesPerSecond,
                parameters.authoredOrder, emission, event.confirmedTick,
                outCommands)) {
            advanceEmissionSequence(nextEmissionSequence);
        }
    }
}

void ObjectFireWeaponBehaviorSystem::updateContinuous(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint32_t logicFramesPerSecond, uint64_t confirmedTick,
    uint64_t& nextEmissionSequence,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands) const {
    struct Candidate final { ObjectId object; ecs::entity entity; };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                ObjectFireWeaponWhenDamagedComponent,
                                const ObjectHealthComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectHealthComponent& health =
            view.template get<const ObjectHealthComponent>(entity);
        if (!identity.id || lifecycle.isPendingDestroy(identity.id) ||
            health.effectivelyDead ||
            isObjectDisabled(registry, entity, confirmedTick)) continue;
        candidates.push_back({identity.id, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });
    for (const Candidate& candidate : candidates) {
        ObjectFireWeaponWhenDamagedComponent& component =
            ecs::get<ObjectFireWeaponWhenDamagedComponent>(registry,
                                                            candidate.entity);
        const ObjectHealthComponent& health =
            ecs::get<const ObjectHealthComponent>(registry, candidate.entity);
        const size_t state = bodyStateIndex(health.damageState);
        const size_t count = std::min(component.plan->rules.size(),
                                      component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const auto& parameters = component.plan->rules[index];
            auto& runtime = component.instances[index];
            if (!runtime.upgradeActivated ||
                !runtime.continuousWeapons[state].content) continue;
            const uint64_t emission = nextEmissionSequence;
            if (tryQueueObjectSystemWeaponFire(
                    runtime.continuousWeapons[state], registry,
                    candidate.entity, candidate.object, content, random,
                    logicFramesPerSecond, parameters.authoredOrder,
                    emission, confirmedTick, outCommands)) {
                advanceEmissionSequence(nextEmissionSequence);
            }
        }
    }
}

bool ObjectFireWeaponBehaviorSystem::tryFireWhenDead(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ecs::entity entity, ObjectId object, uint32_t ruleIndex,
    const game::ObjectDeathReactionRule& rule,
    const UpgradeMask& ownerCompletedUpgrades,
    const GameContentSnapshot& content, SimulationRandom& random,
    uint64_t confirmedTick, uint64_t& nextEmissionSequence,
    container::Vector<ObjectSystemWeaponFireCommand>& outCommands) const {
    if (!rule.fireWeaponWhenDead ||
        rule.kind != game::ObjectDeathReactionKind::FireWeaponWhenDead) {
        return false;
    }
    ObjectFireWeaponWhenDeadRuntimeComponent* runtime =
        ecs::try_get<ObjectFireWeaponWhenDeadRuntimeComponent>(registry, entity);
    if (!runtime || ruleIndex >= runtime->rules.size()) return false;
    ObjectFireWeaponWhenDeadRuleRuntime& state = runtime->rules[ruleIndex];
    if (!state.upgradeActivated || !state.content) {
        return false;
    }
    const ObjectUpgradeInventoryComponent* inventory =
        ecs::try_get<ObjectUpgradeInventoryComponent>(registry, entity);
    const UpgradeMask localCompleted = inventory
        ? inventory->completed : UpgradeMask{};
    // RefCode deliberately rechecks both masks inside onDie. A cached owner
    // projection is useful for diagnostics, but cannot be the authority after
    // a same-tick local removal or ownership/technology change.
    if (game::objectFireWeaponUpgradeHasConflict(
            rule.fireWeaponWhenDead->upgradeMux,
            ownerCompletedUpgrades, localCompleted,
            content.upgradeCatalog())) {
        return false;
    }
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    if (status && status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction))) {
        return false;
    }
    const uint32_t shotSequence = state.nextShotSequence++;
    if (state.nextShotSequence == 0) ++state.nextShotSequence;
    const uint64_t emission = nextEmissionSequence;
    if (!queueObjectTransientWeaponFire(
            state.content, registry, entity, object, content, random,
            shotSequence, rule.authoredOrder, emission, confirmedTick,
            outCommands)) return false;
    advanceEmissionSequence(nextEmissionSequence);
    return true;
}

} // namespace engine
