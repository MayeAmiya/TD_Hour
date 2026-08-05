#include "core/container/container_types.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/component/ObjectDirty.h"

#include <algorithm>

namespace engine {
namespace {

[[nodiscard]] constexpr size_t reasonIndex(
    ObjectDisabledReason reason) noexcept {
    return static_cast<size_t>(reason);
}

[[nodiscard]] bool activeAt(uint64_t untilTick,
                            uint64_t confirmedTick) noexcept {
    return untilTick == OBJECT_DISABLED_FOREVER_TICK ||
           untilTick > confirmedTick;
}

[[nodiscard]] ObjectDisabledMask timedMask(
    const ObjectDisabledComponent* component,
    uint64_t confirmedTick) noexcept {
    if (!component) return 0;
    ObjectDisabledMask result = 0;
    for (size_t index = 0; index < component->untilTicks.size(); ++index) {
        if (activeAt(component->untilTicks[index], confirmedTick)) {
            result |= ObjectDisabledMask{1} << index;
        }
    }
    return result & objectDisabledKnownMask();
}

void incrementRevision(ObjectDisabledComponent& component) noexcept {
    if (component.revision != std::numeric_limits<uint64_t>::max()) {
        ++component.revision;
    }
}

} // namespace

ObjectDisabledMask objectDisabledMask(
    const ecs::registry& registry, ecs::entity entity,
    uint64_t confirmedTick) noexcept {
    if (entity == ecs::null || !registry.valid(entity)) return 0;
    ObjectDisabledMask result = timedMask(
        ecs::try_get<ObjectDisabledComponent>(registry, entity),
        confirmedTick);
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, entity);
    if (health && health->subdued) {
        result |= objectDisabledBit(ObjectDisabledReason::Subdued);
    }
    return result;
}

bool isObjectDisabled(const ecs::registry& registry, ecs::entity entity,
                      uint64_t confirmedTick,
                      ObjectDisabledMask reasonsAllowedToProcess) noexcept {
    const ObjectDisabledMask blocking = objectDisabledMask(
        registry, entity, confirmedTick) &
        ~(reasonsAllowedToProcess & objectDisabledKnownMask());
    return blocking != 0;
}

bool isObjectDisabledBy(const ecs::registry& registry, ecs::entity entity,
                        ObjectDisabledReason reason,
                        uint64_t confirmedTick) noexcept {
    return (objectDisabledMask(registry, entity, confirmedTick) &
            objectDisabledBit(reason)) != 0;
}

uint64_t objectDisabledUntil(const ecs::registry& registry,
                             ecs::entity entity,
                             ObjectDisabledReason reason) noexcept {
    if (entity == ecs::null || !registry.valid(entity) ||
        reason >= ObjectDisabledReason::Count) return 0;
    if (reason == ObjectDisabledReason::Subdued) {
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        return health && health->subdued ? OBJECT_DISABLED_FOREVER_TICK : 0;
    }
    const ObjectDisabledComponent* component =
        ecs::try_get<ObjectDisabledComponent>(registry, entity);
    return component ? component->untilTicks[reasonIndex(reason)] : 0;
}

uint64_t objectDisabledStartedAt(const ecs::registry& registry,
                                 ecs::entity entity,
                                 ObjectDisabledReason reason) noexcept {
    if (entity == ecs::null || !registry.valid(entity) ||
        reason >= ObjectDisabledReason::Count) return 0;
    const ObjectDisabledComponent* component =
        ecs::try_get<ObjectDisabledComponent>(registry, entity);
    return component ? component->startedTicks[reasonIndex(reason)] : 0;
}

ObjectDisabledTransition ObjectDisabledSystem::setUntil(
    ecs::registry& registry, ecs::entity entity,
    ObjectDisabledReason reason, uint64_t untilTick,
    uint64_t confirmedTick) {
    ObjectDisabledTransition transition;
    if (entity == ecs::null || !registry.valid(entity) ||
        reason >= ObjectDisabledReason::Count ||
        reason == ObjectDisabledReason::Subdued) return transition;

    ObjectDisabledComponent* component =
        ecs::try_get<ObjectDisabledComponent>(registry, entity);
    transition.previous = timedMask(component, confirmedTick);
    const size_t index = reasonIndex(reason);
    const bool wasReasonActive =
        component && activeAt(component->untilTicks[index], confirmedTick);
    const uint64_t normalizedUntil = activeAt(untilTick, confirmedTick)
        ? untilTick : 0;
    if (!component && normalizedUntil != 0) {
        component = &ecs::emplace<ObjectDisabledComponent>(registry, entity);
    }
    if (!component) return transition;

    const bool valueChanged =
        component->untilTicks[index] != normalizedUntil;
    if (valueChanged) {
        component->untilTicks[index] = normalizedUntil;
        if (normalizedUntil == 0) {
            component->startedTicks[index] = 0;
        } else if (!wasReasonActive) {
            component->startedTicks[index] = confirmedTick;
        }
        component->lastChangedTick = confirmedTick;
        incrementRevision(*component);
        markObjectDirty(
            registry, entity, ObjectDirtyDomain::RenderExtraction);
    }
    transition.current = timedMask(component, confirmedTick);
    transition.newlySet = transition.current & ~transition.previous;
    transition.newlyCleared = transition.previous & ~transition.current;
    return transition;
}

ObjectDisabledTransition ObjectDisabledSystem::clear(
    ecs::registry& registry, ecs::entity entity,
    ObjectDisabledReason reason, uint64_t confirmedTick) {
    return setUntil(registry, entity, reason, 0, confirmedTick);
}

} // namespace engine
