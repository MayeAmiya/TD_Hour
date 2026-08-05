#include "game/object/simulation/world/ObjectDynamicShroud.h"
#include "game/object/simulation/runtime/ObjectVisionRange.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "debug/debug.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] uint64_t saturatingAdd(uint64_t left,
                                     uint64_t right) noexcept {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max() : left + right;
}

[[nodiscard]] uint64_t millisecondsToTicks(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t rate = std::max<uint32_t>(1, framesPerSecond);
    const uint64_t product = static_cast<uint64_t>(milliseconds) * rate;
    return product / 1000u + (product % 1000u != 0 ? 1u : 0u);
}

[[nodiscard]] math::q32_32 fixedFromFiniteFloat(float value) noexcept {
    if (!std::isfinite(value)) return {};
    constexpr double kScale = 4294967296.0;
    constexpr double kMinimum = -2147483648.0;
    const double maximum = std::nextafter(2147483648.0, 0.0);
    const double clamped = std::clamp(
        static_cast<double>(value), kMinimum, maximum);
    return math::q32_32::from_raw(
        static_cast<int64_t>(clamped * kScale));
}

[[nodiscard]] math::q32_32 divideFixed(
    math::q32_32 value, uint64_t divisor) noexcept {
    const uint64_t safe = std::max<uint64_t>(1, divisor);
    if (safe > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return {};
    }
    return math::q32_32::from_raw(
        value.raw() / static_cast<int64_t>(safe));
}

[[nodiscard]] int64_t subtractRawMagnitudeSaturating(
    int64_t value, uint64_t magnitude) noexcept {
    const uint64_t bits = std::bit_cast<uint64_t>(value);
    const uint64_t minimumBits =
        std::bit_cast<uint64_t>(std::numeric_limits<int64_t>::min());
    const uint64_t distance = bits - minimumBits;
    if (magnitude > distance) return std::numeric_limits<int64_t>::min();
    return std::bit_cast<int64_t>(bits - magnitude);
}

[[nodiscard]] int64_t addRawMagnitudeSaturating(
    int64_t value, uint64_t magnitude) noexcept {
    const uint64_t bits = std::bit_cast<uint64_t>(value);
    const uint64_t maximumBits =
        std::bit_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    const uint64_t distance = maximumBits - bits;
    if (magnitude > distance) return std::numeric_limits<int64_t>::max();
    return std::bit_cast<int64_t>(bits + magnitude);
}

[[nodiscard]] math::q32_32 advanceShrink(
    math::q32_32 current, math::q32_32 native,
    math::q32_32 finalVision, uint64_t shrinkTime) noexcept {
    const uint64_t divisor = std::max<uint64_t>(1, shrinkTime);
    if (native.raw() >= finalVision.raw()) {
        const uint64_t span = std::bit_cast<uint64_t>(native.raw()) -
            std::bit_cast<uint64_t>(finalVision.raw());
        return math::q32_32::from_raw(subtractRawMagnitudeSaturating(
            current.raw(), span / divisor));
    }
    const uint64_t span = std::bit_cast<uint64_t>(finalVision.raw()) -
        std::bit_cast<uint64_t>(native.raw());
    return math::q32_32::from_raw(addRawMagnitudeSaturating(
        current.raw(), span / divisor));
}

struct Candidate final {
    ObjectId object = INVALID_OBJECT_ID;
    ecs::entity entity = ecs::null;
};

void emitDecalEvent(
    container::Vector<ObjectDynamicShroudDecalEvent>& output,
    ObjectDynamicShroudDecalEventKind kind, ObjectId object, PlayerId owner,
    const game::ObjectDynamicShroudRule& rule,
    const ObjectDynamicShroudRuntime& runtime,
    const ObjectFixedTransformComponent* transform,
    uint64_t confirmedTick) {
    if (rule.gridDecal.texture.empty() || !owner) return;
    output.push_back({
        .kind = kind,
        .object = object,
        .owner = owner,
        .authoredOrder = rule.authoredOrder,
        .position = transform && transform->authoritative
            ? transform->position
            : LogicFixedVec3{},
        .nativeClearingRange = runtime.nativeClearingRange,
        .currentClearingRange = runtime.currentClearingRange,
        .stateCountdown = runtime.stateCountdown,
        .totalTicks = runtime.totalTicks,
        .growStartDeadline = runtime.growStartDeadline,
        .sustainDeadline = runtime.sustainDeadline,
        .shrinkStartDeadline = runtime.shrinkStartDeadline,
        .opacityThrobTicks = runtime.opacityThrobTicks,
        .recipe = rule.gridDecal,
        .confirmedTick = confirmedTick,
    });
}

} // namespace

void ObjectDynamicShroudSystem::initializeObject(
    ecs::registry& registry, ecs::entity entity,
    const ObjectSimulationRules& rules, uint64_t createdAtTick) const {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (!type || !type->archetype ||
        !type->archetype->dynamicShroudPlan ||
        type->archetype->dynamicShroudPlan->rules.empty()) {
        return;
    }

    ObjectDynamicShroudComponent component{
        .plan = type->archetype->dynamicShroudPlan,
        .projectedRadius = effectiveObjectShroudClearingRangeFixed(
            registry, entity),
        .hasProjectedRadius = true,
    };
    component.instances.reserve(component.plan->rules.size());
    for (const game::ObjectDynamicShroudRule& rule : component.plan->rules) {
        const uint64_t shrinkDelay = millisecondsToTicks(
            rule.shrinkDelayMilliseconds, rules.logicFramesPerSecond);
        const uint64_t shrinkTime = millisecondsToTicks(
            rule.shrinkTimeMilliseconds, rules.logicFramesPerSecond);
        const uint64_t growDelay = millisecondsToTicks(
            rule.growDelayMilliseconds, rules.logicFramesPerSecond);
        const uint64_t growTime = millisecondsToTicks(
            rule.growTimeMilliseconds, rules.logicFramesPerSecond);
        const uint64_t countdown = saturatingAdd(shrinkDelay, shrinkTime);
        const bool tickScheduleValid =
            saturatingAdd(growDelay, growTime) <= shrinkDelay;
        const uint64_t growStart = countdown >= growDelay
            ? countdown - growDelay : 0;
        const uint64_t sustain = growStart >= growTime
            ? growStart - growTime : 0;
        if (!tickScheduleValid) {
            TD_LOG_ERROR(
                "[ObjectDynamicShroud] Object recipe '{}' occurrence {} becomes invalid at {} FPS: ceil(GrowDelay)+ceil(GrowTime) exceeds ceil(ShrinkDelay); occurrence disabled",
                type->archetype->name, rule.authoredOrder,
                rules.logicFramesPerSecond);
        }
        component.instances.push_back({
            .phase = tickScheduleValid
                ? ObjectDynamicShroudPhase::NotStarted
                : ObjectDynamicShroudPhase::Sleeping,
            .stateCountdown = countdown,
            .totalTicks = std::max<uint64_t>(1, countdown),
            .growStartDeadline = growStart,
            .sustainDeadline = sustain,
            .shrinkStartDeadline = shrinkTime,
            .doneForeverTick = saturatingAdd(createdAtTick, countdown),
            .changeIntervalTicks = millisecondsToTicks(
                rule.changeIntervalMilliseconds, rules.logicFramesPerSecond),
            .growIntervalTicks = millisecondsToTicks(
                rule.growIntervalMilliseconds, rules.logicFramesPerSecond),
            .growTimeTicks = growTime,
            .shrinkTimeTicks = shrinkTime,
            .opacityThrobTicks = millisecondsToTicks(
                rule.gridDecal.opacityThrobMilliseconds,
                rules.logicFramesPerSecond),
            .nativeClearingRange = component.projectedRadius,
        });
    }
    if (ObjectDynamicShroudComponent* existing =
            ecs::try_get<ObjectDynamicShroudComponent>(registry, entity)) {
        *existing = std::move(component);
    } else {
        ecs::emplace<ObjectDynamicShroudComponent>(
            registry, entity, std::move(component));
    }
}

void ObjectDynamicShroudSystem::update(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    uint64_t confirmedTick,
    container::Vector<ObjectDynamicShroudDecalEvent>& outDecalEvents) const {
    container::Vector<Candidate> candidates;
    const auto view = ecs::view<const ObjectIdentityComponent,
                                const ObjectLifecycleComponent,
                                ObjectDynamicShroudComponent>(registry);
    candidates.reserve(view.size_hint());
    for (const ecs::entity entity : view) {
        const ObjectIdentityComponent& identity =
            view.template get<const ObjectIdentityComponent>(entity);
        const ObjectLifecycleComponent& state =
            view.template get<const ObjectLifecycleComponent>(entity);
        if (!identity.id || state.phase != ObjectLifecyclePhase::Alive ||
            !lifecycle.entityFromId(identity.id) ||
            lifecycle.isPendingDestroy(identity.id)) {
            continue;
        }
        candidates.push_back({.object = identity.id, .entity = entity});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.object < right.object;
              });

    for (const Candidate& candidate : candidates) {
        // UpdateModule's default DISABLEDMASK_NONE suppresses callbacks while
        // any Disabled reason is active. The absolute done-forever deadline
        // intentionally keeps advancing, so recovery can jump straight to
        // FinalVision just like the legacy failsafe.
        if (isObjectDisabled(registry, candidate.entity, confirmedTick)) {
            continue;
        }
        ObjectDynamicShroudComponent& component =
            ecs::get<ObjectDynamicShroudComponent>(registry, candidate.entity);
        if (!component.plan) continue;
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, candidate.entity);
        const ObjectFixedTransformComponent* transform =
            ecs::try_get<ObjectFixedTransformComponent>(
                registry, candidate.entity);
        const PlayerId player = owner ? owner->player : INVALID_PLAYER_ID;
        const size_t count = std::min(
            component.plan->rules.size(), component.instances.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectDynamicShroudRule& rule =
                component.plan->rules[index];
            ObjectDynamicShroudRuntime& runtime = component.instances[index];
            if (runtime.phase == ObjectDynamicShroudPhase::Sleeping) continue;

            if (!runtime.decalBeginEmitted) {
                emitDecalEvent(outDecalEvents,
                               ObjectDynamicShroudDecalEventKind::Begin,
                               candidate.object, player, rule, runtime,
                               transform, confirmedTick);
                runtime.decalBeginEmitted = true;
            }

            if (runtime.stateCountdown == 0 ||
                confirmedTick > runtime.doneForeverTick) {
                runtime.phase = ObjectDynamicShroudPhase::DoneForever;
            } else if (runtime.stateCountdown <=
                       runtime.shrinkStartDeadline) {
                runtime.phase = ObjectDynamicShroudPhase::Shrinking;
            } else if (runtime.stateCountdown <= runtime.sustainDeadline) {
                runtime.phase = ObjectDynamicShroudPhase::Sustaining;
            } else if (runtime.stateCountdown <= runtime.growStartDeadline) {
                runtime.phase = ObjectDynamicShroudPhase::Growing;
            }

            // RefCode animates all 30 decals before mutating the growing
            // range for this tick. Publish that exact confirmed value instead
            // of asking presentation to reconstruct a gameplay timeline from
            // deadlines or a newest-only world snapshot.
            if (runtime.phase == ObjectDynamicShroudPhase::NotStarted ||
                runtime.phase == ObjectDynamicShroudPhase::Growing) {
                runtime.decalPresentedPosition =
                    transform && transform->authoritative
                    ? transform->position
                    : LogicFixedVec3{};
                runtime.decalPresentedClearingRange =
                    runtime.currentClearingRange;
                runtime.decalPresentedStateCountdown =
                    runtime.stateCountdown;
                runtime.decalPresentationSampleValid = true;
                emitDecalEvent(outDecalEvents,
                               ObjectDynamicShroudDecalEventKind::Update,
                               candidate.object, player, rule, runtime,
                               transform, confirmedTick);
            }

            switch (runtime.phase) {
            case ObjectDynamicShroudPhase::NotStarted:
                break;
            case ObjectDynamicShroudPhase::Growing:
                runtime.currentClearingRange += divideFixed(
                    runtime.nativeClearingRange, runtime.growTimeTicks);
                if (runtime.currentClearingRange >=
                    runtime.nativeClearingRange) {
                    runtime.phase = ObjectDynamicShroudPhase::Sustaining;
                }
                break;
            case ObjectDynamicShroudPhase::Sustaining:
                runtime.currentClearingRange = runtime.nativeClearingRange;
                if (!runtime.decalEndEmitted) {
                    emitDecalEvent(outDecalEvents,
                                   ObjectDynamicShroudDecalEventKind::End,
                                   candidate.object, player, rule, runtime,
                                   transform, confirmedTick);
                    runtime.decalEndEmitted = true;
                }
                break;
            case ObjectDynamicShroudPhase::Shrinking:
                runtime.currentClearingRange = advanceShrink(
                    runtime.currentClearingRange,
                    runtime.nativeClearingRange, rule.finalVision,
                    runtime.shrinkTimeTicks);
                break;
            case ObjectDynamicShroudPhase::DoneForever:
                runtime.currentClearingRange = rule.finalVision;
                if (!runtime.decalEndEmitted) {
                    emitDecalEvent(outDecalEvents,
                                   ObjectDynamicShroudDecalEventKind::End,
                                   candidate.object, player, rule, runtime,
                                   transform, confirmedTick);
                    runtime.decalEndEmitted = true;
                }
                break;
            case ObjectDynamicShroudPhase::Sleeping:
                break;
            }

            if (runtime.stateCountdown > 0) --runtime.stateCountdown;
            if (runtime.changeIntervalCountdown > 0) {
                --runtime.changeIntervalCountdown;
            } else {
                runtime.changeIntervalCountdown =
                    runtime.phase == ObjectDynamicShroudPhase::Growing
                    ? runtime.growIntervalTicks : runtime.changeIntervalTicks;
                component.projectedRadius = runtime.currentClearingRange;
                component.hasProjectedRadius = true;
                if (runtime.phase == ObjectDynamicShroudPhase::DoneForever) {
                    runtime.phase = ObjectDynamicShroudPhase::Sleeping;
                }
            }
        }
    }
}

void ObjectDynamicShroudSystem::terminateObject(
    ecs::registry& registry, ecs::entity entity, ObjectId object,
    uint64_t confirmedTick,
    container::Vector<ObjectDynamicShroudDecalEvent>& outDecalEvents) const {
    ObjectDynamicShroudComponent* component =
        ecs::try_get<ObjectDynamicShroudComponent>(registry, entity);
    if (!component || !component->plan) return;
    const OwnerComponent* owner = ecs::try_get<OwnerComponent>(registry, entity);
    const PlayerId player = owner ? owner->player : INVALID_PLAYER_ID;
    const ObjectFixedTransformComponent* transform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, entity);
    const size_t count = std::min(
        component->plan->rules.size(), component->instances.size());
    for (size_t index = 0; index < count; ++index) {
        const game::ObjectDynamicShroudRule& rule =
            component->plan->rules[index];
        ObjectDynamicShroudRuntime& runtime = component->instances[index];
        if (runtime.decalBeginEmitted && !runtime.decalEndEmitted) {
            emitDecalEvent(outDecalEvents,
                           ObjectDynamicShroudDecalEventKind::End,
                           object, player, rule, runtime, transform,
                           confirmedTick);
            runtime.decalEndEmitted = true;
        }
        runtime.phase = ObjectDynamicShroudPhase::Sleeping;
    }
}

} // namespace engine
