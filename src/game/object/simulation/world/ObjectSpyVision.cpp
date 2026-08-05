#include "game/object/simulation/world/ObjectSpyVision.h"
#include "core/container/string_utils.h"

#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/player/PlayerRegistry.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace engine {
namespace {

struct Candidate final {
    ObjectId object = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t logicFramesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t fps = std::max<uint32_t>(1u, logicFramesPerSecond);
    const uint64_t scaled = static_cast<uint64_t>(milliseconds) * fps;
    return scaled / 1000u + (scaled % 1000u != 0 ? 1u : 0u);
}

[[nodiscard]] uint64_t saturatingAdd(uint64_t left,
                                     uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

[[nodiscard]] bool upgradeMatches(
    const game::ObjectSpyVisionRule& rule, const PlayerState* player,
    const ObjectUpgradeInventoryComponent* object,
    const UpgradeCatalog* catalog) noexcept {
    static_cast<void>(catalog);
    if (!rule.upgradeMasksCompiled || rule.triggeredByMask.none())
        return false;
    const UpgradeMask completed =
        (player ? player->upgrades.completed : UpgradeMask{}) |
        (object ? object->completed : UpgradeMask{});
    if (completed.test_for_any(rule.conflictsWithMask)) return false;
    return rule.requiresAllTriggers
        ? completed.test_for_all(rule.triggeredByMask)
        : completed.test_for_any(rule.triggeredByMask);
}

void activate(ObjectSpyVisionRuntime& runtime,
              const game::ObjectSpyVisionRule& rule,
              const ObjectSimulationRules& rules,
              uint64_t confirmedTick) noexcept {
    runtime.active = true;
    runtime.cycleInitialized = true;
    runtime.nextActivationTick = 0;
    const uint64_t duration = millisecondsToTicks(
        rule.selfPoweredDurationMilliseconds, rules.logicFramesPerSecond);
    runtime.deactivateTick = duration == 0
        ? std::numeric_limits<uint64_t>::max()
        : saturatingAdd(confirmedTick, duration);
}

} // namespace

bool objectSpyVisionMatchesKinds(
    const game::ObjectSpyVisionRule& rule,
    const ObjectKindOfComponent* kinds) noexcept {
    if (rule.spyOnNone) return false;
    if (kinds && kinds->mask.test_for_any(rule.excludedKinds)) return false;
    if (rule.spyOnAll) return true;
    return kinds && kinds->mask.test_for_any(rule.includedKinds);
}

void ObjectSpyVisionSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype || !type->archetype->spyVisionPlan) return;
    ObjectSpyVisionComponent value{.plan = type->archetype->spyVisionPlan};
    value.instances.resize(value.plan->rules.size());
    if (ObjectSpyVisionComponent* existing =
            ecs::try_get<ObjectSpyVisionComponent>(registry, entity)) {
        *existing = std::move(value);
    } else {
        ecs::emplace<ObjectSpyVisionComponent>(registry, entity,
                                               std::move(value));
    }
}

bool ObjectSpyVisionSystem::activateForTicks(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t durationTicks,
    uint64_t confirmedTick) const {
    if (!object || lifecycle.isPendingDestroy(object)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity) return false;
    ObjectSpyVisionComponent* component =
        ecs::try_get<ObjectSpyVisionComponent>(registry, *entity);
    if (!component || !component->plan || component->instances.empty() ||
        component->plan->rules.empty()) {
        return false;
    }
    ObjectSpyVisionRuntime& runtime = component->instances.front();
    runtime.active = true;
    runtime.cycleInitialized = true;
    runtime.nextActivationTick = 0;
    runtime.deactivateTick = durationTicks == 0
        ? std::numeric_limits<uint64_t>::max()
        : saturatingAdd(confirmedTick, durationTicks);
    return true;
}

void ObjectSpyVisionSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, const ObjectSimulationRules& rules,
    uint64_t confirmedTick, const UpgradeCatalog* upgradeCatalog) const {
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const OwnerComponent,
                                ObjectSpyVisionComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (identity.id) candidates.push_back({identity.id, entity});
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });

    for (const Candidate& candidate : candidates) {
        ObjectSpyVisionComponent& component =
            ecs::get<ObjectSpyVisionComponent>(registry, candidate.entity);
        if (!component.plan ||
            component.instances.size() != component.plan->rules.size()) {
            continue;
        }
        const OwnerComponent& owner =
            ecs::get<OwnerComponent>(registry, candidate.entity);
        const PlayerState* player = players.get(owner.player);
        const ObjectUpgradeInventoryComponent* objectUpgrades =
            ecs::try_get<ObjectUpgradeInventoryComponent>(registry,
                                                           candidate.entity);
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, candidate.entity);
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, candidate.entity);
        const ObjectMapStatusComponent* map =
            ecs::try_get<ObjectMapStatusComponent>(registry, candidate.entity);
        const bool unavailable = !player ||
            lifecycle.isPendingDestroy(candidate.object) ||
            (health && health->effectivelyDead) || (map && map->offMap) ||
            (status && status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction)));
        const bool generallyDisabled = !unavailable &&
            isObjectDisabled(registry, candidate.entity, confirmedTick);

        for (size_t index = 0; index < component.instances.size(); ++index) {
            const game::ObjectSpyVisionRule& rule =
                component.plan->rules[index];
            ObjectSpyVisionRuntime& runtime = component.instances[index];
            if (unavailable) {
                runtime.active = false;
                continue;
            }

            const bool newlyUpgraded = rule.needsUpgrade &&
                !runtime.upgradeActivated &&
                upgradeMatches(rule, player, objectUpgrades, upgradeCatalog);
            if (newlyUpgraded) runtime.upgradeActivated = true;
            const bool eligible = !rule.needsUpgrade ||
                                  runtime.upgradeActivated;
            if (!eligible) {
                runtime.active = false;
                continue;
            }

            if (generallyDisabled) {
                runtime.wasGenerallyDisabled = true;
                runtime.resetTimersAfterDisabled = true;
                runtime.active = false;
                continue;
            }
            if (runtime.wasGenerallyDisabled) {
                runtime.wasGenerallyDisabled = false;
                runtime.resetTimersAfterDisabled = true;
            }
            if (runtime.sabotageDisabledUntilTick != 0 &&
                confirmedTick < runtime.sabotageDisabledUntilTick) {
                runtime.resetTimersAfterDisabled = true;
                runtime.active = false;
                continue;
            }
            if (runtime.sabotageDisabledUntilTick != 0) {
                runtime.sabotageDisabledUntilTick = 0;
                runtime.resetTimersAfterDisabled = true;
            }

            if (runtime.resetTimersAfterDisabled) {
                runtime.resetTimersAfterDisabled = false;
                runtime.cycleInitialized = true;
                runtime.active = false;
                runtime.deactivateTick = 0;
                if (rule.selfPowered) {
                    const uint64_t interval = millisecondsToTicks(
                        rule.selfPoweredIntervalMilliseconds,
                        rules.logicFramesPerSecond);
                    if (interval == 0) {
                        runtime.active = true;
                        runtime.deactivateTick =
                            std::numeric_limits<uint64_t>::max();
                    } else {
                        runtime.nextActivationTick = saturatingAdd(
                            confirmedTick, interval);
                    }
                }
                continue;
            }

            if (newlyUpgraded) {
                activate(runtime, rule, rules, confirmedTick);
                continue;
            }
            if (!runtime.cycleInitialized) {
                runtime.cycleInitialized = true;
                if (rule.selfPowered) {
                    activate(runtime, rule, rules, confirmedTick);
                }
                continue;
            }
            if (runtime.active && confirmedTick >= runtime.deactivateTick) {
                runtime.active = false;
                runtime.deactivateTick = 0;
                if (rule.selfPowered) {
                    runtime.nextActivationTick = saturatingAdd(
                        confirmedTick, millisecondsToTicks(
                            rule.selfPoweredIntervalMilliseconds,
                            rules.logicFramesPerSecond));
                }
                continue;
            }
            if (!runtime.active && rule.selfPowered &&
                confirmedTick >= runtime.nextActivationTick) {
                activate(runtime, rule, rules, confirmedTick);
            }
        }
    }
}

bool ObjectSpyVisionSystem::setPlayerDisabledUntil(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerId player, uint64_t untilTick, uint64_t confirmedTick) const {
    if (!player) return false;
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const OwnerComponent,
                                ObjectSpyVisionComponent>(registry);
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const OwnerComponent& owner =
            view.template get<const OwnerComponent>(entity);
        if (identity.id && owner.player == player &&
            !lifecycle.isPendingDestroy(identity.id)) {
            candidates.push_back({identity.id, entity});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) {
            return left.object < right.object;
        });
    const uint64_t normalized = untilTick > confirmedTick ? untilTick : 0;
    bool changed = false;
    for (const Candidate& candidate : candidates) {
        ObjectSpyVisionComponent& component =
            ecs::get<ObjectSpyVisionComponent>(registry, candidate.entity);
        for (ObjectSpyVisionRuntime& runtime : component.instances) {
            if (runtime.sabotageDisabledUntilTick != normalized ||
                runtime.active || !runtime.resetTimersAfterDisabled) {
                changed = true;
            }
            runtime.sabotageDisabledUntilTick = normalized;
            runtime.active = false;
            runtime.resetTimersAfterDisabled = true;
        }
    }
    return changed;
}

} // namespace engine
