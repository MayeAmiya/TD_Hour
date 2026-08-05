#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/object/simulation/lifecycle/ObjectLifetime.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/contracts/ObjectLifecycle.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <system_error>
#include <utility>

namespace engine {
namespace {

using container::asciiEqualIgnoreCase;

[[nodiscard]] uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max()
        : left + right;
}

[[nodiscard]] uint64_t mixLifetimeRandom(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] uint64_t makeLifetimeRandomKey(uint64_t sessionSeed, ObjectId object,
                                              const game::ObjectLifetimeRule& rule,
                                              uint64_t scheduleGeneration) noexcept {
    uint64_t key = mixLifetimeRandom(sessionSeed);
    key ^= mixLifetimeRandom(static_cast<uint64_t>(object.value));
    key ^= mixLifetimeRandom(rule.stableRuleKey);
    key ^= mixLifetimeRandom(static_cast<uint64_t>(rule.authoredOrder));
    key ^= mixLifetimeRandom(static_cast<uint64_t>(rule.action));
    key ^= mixLifetimeRandom(scheduleGeneration);
    return mixLifetimeRandom(key);
}

[[nodiscard]] uint64_t randomInclusive(uint64_t key, uint64_t minimum,
                                        uint64_t maximum) noexcept {
    // RefCode's GameLogicRandomValue collapses an inverted/equal range to
    // its second endpoint. Keep that odd but observable behavior instead of
    // silently reordering authored mod data.
    if (minimum >= maximum) return maximum;
    const uint64_t width = maximum - minimum + 1u;
    if (width == 0) return mixLifetimeRandom(key); // full uint64_t range
    return minimum + mixLifetimeRandom(key) % width;
}

[[nodiscard]] uint64_t millisecondsToTicks(uint32_t milliseconds,
                                            uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    constexpr uint64_t kRoundUp = 999u;
    if (milliseconds > (std::numeric_limits<uint64_t>::max() - kRoundUp) / rate) {
        return std::numeric_limits<uint64_t>::max();
    }
    return (static_cast<uint64_t>(milliseconds) * rate + kRoundUp) / 1000u;
}

[[nodiscard]] bool templateIsHulk(
    const game::ObjectKindOfMask& kindOfMask) noexcept {
    return game::objectHasKind(kindOfMask, game::ObjectKindOf::Hulk);
}

void armInitialDeadline(ObjectLifetimeRuntime& runtime,
                        const game::ObjectLifetimeRule& rule,
                        ObjectId object, uint64_t createdAtTick,
                        const ObjectSimulationRules& rules,
                        uint64_t sessionSeed, bool isHulk,
                        std::optional<uint32_t> hulkOverrideFramesAtSpawn) noexcept {
    if (runtime.armed) return;
    const bool overrideHulkLifetime = isHulk &&
        rule.action == game::ObjectLifetimeAction::Kill && hulkOverrideFramesAtSpawn.has_value();
    const uint64_t minimum = overrideHulkLifetime ? *hulkOverrideFramesAtSpawn :
        millisecondsToTicks(rule.minimumLifetimeMilliseconds, rules.logicFramesPerSecond);
    const uint64_t maximum = overrideHulkLifetime ? *hulkOverrideFramesAtSpawn :
        millisecondsToTicks(rule.maximumLifetimeMilliseconds, rules.logicFramesPerSecond);
    const uint64_t sampled = randomInclusive(
        makeLifetimeRandomKey(sessionSeed, object, rule, runtime.scheduleGeneration),
        minimum, maximum);
    runtime.dueTick = saturatingAdd(createdAtTick, std::max<uint64_t>(1u, sampled));
    runtime.armed = true;
}

} // namespace

void ObjectLifetimeSystem::initializeObject(ecs::registry& registry, ecs::entity entity,
                                            const ObjectSimulationRules& rules,
                                            uint64_t sessionSeed,
                                            std::optional<uint32_t> hulkLifetimeOverrideFrames) const {
    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!templateComponent || !templateComponent->archetype ||
        !templateComponent->archetype->lifetimePlan ||
        templateComponent->archetype->lifetimePlan->rules.empty()) {
        return;
    }

    ObjectLifetimeComponent component{
        .plan = templateComponent->archetype->lifetimePlan,
        .isHulk = templateIsHulk(templateComponent->archetype->kindOfMask),
    };
    if (component.isHulk) component.hulkOverrideFramesAtSpawn = hulkLifetimeOverrideFrames;
    component.instances.resize(component.plan->rules.size());
    const ObjectIdentityComponent* identity =
        ecs::try_get<ObjectIdentityComponent>(registry, entity);
    const ObjectLifecycleComponent* lifecycle =
        ecs::try_get<ObjectLifecycleComponent>(registry, entity);
    if (identity && identity->id && lifecycle) {
        for (size_t index = 0; index < component.plan->rules.size(); ++index) {
            armInitialDeadline(component.instances[index], component.plan->rules[index], identity->id,
                               lifecycle->createdAtTick, rules, sessionSeed, component.isHulk,
                               component.hulkOverrideFramesAtSpawn);
        }
    }
    if (ObjectLifetimeComponent* existing =
            ecs::try_get<ObjectLifetimeComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectLifetimeComponent>(registry, entity, std::move(component));
    }
}

void ObjectLifetimeSystem::update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                                  const ObjectSimulationRules& rules, uint64_t sessionSeed,
                                  uint64_t confirmedTick,
                                  container::Vector<ObjectLifetimeCommand>& outCommands) const {
    struct Candidate final {
        ObjectId object = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectLifecycleComponent,
                                ObjectLifetimeComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        if (!identity.id || !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id)) {
            continue;
        }
        candidates.push_back({.object = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left,
                                                         const Candidate& right) {
        return left.object < right.object;
    });

    for (const Candidate& candidate : candidates) {
        // Withhold the terminal command without shifting its absolute due
        // tick. Recovery therefore catches up immediately when overdue.
        if (isObjectDisabled(registry, candidate.entity, confirmedTick)) {
            continue;
        }
        const ObjectLifecycleComponent& lifecycleState =
            ecs::get<const ObjectLifecycleComponent>(registry, candidate.entity);
        ObjectLifetimeComponent& component =
            ecs::get<ObjectLifetimeComponent>(registry, candidate.entity);
        if (!component.plan) continue;
        const size_t count = std::min(component.plan->rules.size(), component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectLifetimeRule& rule = component.plan->rules[index];
            ObjectLifetimeRuntime& runtime = component.instances[index];
            if (runtime.fired) continue;
            if (!runtime.armed) {
                // Normal spawn assembly armed this before Created. Keep a
                // defensive repair path for test tools or third-party code
                // that manually attaches the component.
                armInitialDeadline(runtime, rule, candidate.object, lifecycleState.createdAtTick,
                                   rules, sessionSeed, component.isHulk,
                                   component.hulkOverrideFramesAtSpawn);
            }
            if (confirmedTick < runtime.dueTick) continue;
            runtime.fired = true;
            outCommands.push_back({
                .object = candidate.object,
                .authoredOrder = rule.authoredOrder,
                .action = rule.action,
            });
        }
    }
}

bool ObjectLifetimeSystem::reschedule(ecs::registry& registry,
                                      const ObjectLifecycle& lifecycle,
                                      const ObjectLifetimeRescheduleRequest& request,
                                      uint64_t sessionSeed) const {
    if (!request.object || lifecycle.isPendingDestroy(request.object)) return false;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(request.object);
    if (!entity) return false;
    ObjectLifetimeComponent* component =
        ecs::try_get<ObjectLifetimeComponent>(registry, *entity);
    if (!component || !component->plan) return false;

    const size_t count = std::min(component->plan->rules.size(), component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectLifetimeRule& rule = component->plan->rules[index];
        if (rule.action != request.action ||
            (request.authoredOrder && rule.authoredOrder != *request.authoredOrder)) {
            continue;
        }

        ObjectLifetimeRuntime& runtime = component->instances[index];
        runtime.scheduleGeneration = saturatingAdd(runtime.scheduleGeneration, 1u);
        const uint64_t sampled = randomInclusive(
            makeLifetimeRandomKey(sessionSeed, request.object, rule, runtime.scheduleGeneration),
            request.minimumLifetimeFrames, request.maximumLifetimeFrames);
        runtime.dueTick = saturatingAdd(request.confirmedTick, std::max<uint64_t>(1u, sampled));
        runtime.armed = true;
        runtime.fired = false;
        return true;
    }
    return false;
}

std::optional<uint64_t> ObjectLifetimeSystem::nextDueTick(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, game::ObjectLifetimeAction action,
    std::optional<uint32_t> authoredOrder) const {
    if (!object) return std::nullopt;
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity) return std::nullopt;
    const ObjectLifetimeComponent* component =
        ecs::try_get<ObjectLifetimeComponent>(registry, *entity);
    if (!component || !component->plan) return std::nullopt;
    const size_t count = std::min(component->plan->rules.size(), component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectLifetimeRule& rule = component->plan->rules[index];
        if (rule.action != action || (authoredOrder && rule.authoredOrder != *authoredOrder)) {
            continue;
        }
        const ObjectLifetimeRuntime& runtime = component->instances[index];
        return runtime.armed ? std::optional<uint64_t>{runtime.dueTick} : std::nullopt;
    }
    return std::nullopt;
}

} // namespace engine
