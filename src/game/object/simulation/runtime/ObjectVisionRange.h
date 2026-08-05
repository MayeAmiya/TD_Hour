#pragma once

#include "core/ecs/registry.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "math/fixed/q32_32.h"

namespace engine {

namespace object_vision_detail {

[[nodiscard]] inline const game::ObjectArchetype* archetypeFor(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    return type && type->archetype ? type->archetype.get() : nullptr;
}

} // namespace object_vision_detail

// These helpers are the sole gameplay read path for the two mutable legacy
// Object ranges.  Keeping the fixed-point form available prevents AI and
// other deterministic consumers from round-tripping an instance override
// through float.
[[nodiscard]] inline math::q32_32 effectiveObjectVisionRangeFixed(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    if (const ObjectVisionRangeOverrideComponent* overrideValue =
            ecs::try_get<ObjectVisionRangeOverrideComponent>(registry, entity)) {
        return math::q32_32::max(
            math::q32_32{}, overrideValue->visionRange);
    }
    const game::ObjectArchetype* archetype =
        object_vision_detail::archetypeFor(registry, entity);
    return archetype
        ? archetype->sightRangeFixed
        : math::q32_32{};
}

[[nodiscard]] inline math::q32_32 effectiveObjectShroudClearingRangeFixed(
    const ecs::registry& registry, ecs::entity entity) noexcept {
    if (const ObjectVisionRangeOverrideComponent* overrideValue =
            ecs::try_get<ObjectVisionRangeOverrideComponent>(registry, entity)) {
        return math::q32_32::max(
            math::q32_32{}, overrideValue->shroudClearingRange);
    }
    const game::ObjectArchetype* archetype =
        object_vision_detail::archetypeFor(registry, entity);
    return archetype ? archetype->shroudClearingRangeFixed
                     : math::q32_32{};
}

inline void setObjectVisionRangeOverride(
    ecs::registry& registry, ecs::entity entity, math::q32_32 visionRange,
    math::q32_32 shroudClearingRange) {
    ObjectVisionRangeOverrideComponent value{
        .visionRange = math::q32_32::max(math::q32_32{}, visionRange),
        .shroudClearingRange =
            math::q32_32::max(math::q32_32{}, shroudClearingRange),
    };
    if (ecs::has<ObjectVisionRangeOverrideComponent>(registry, entity)) {
        ecs::patch<ObjectVisionRangeOverrideComponent>(
            registry, entity,
            [&value](ObjectVisionRangeOverrideComponent& existing) noexcept {
                existing = value;
            });
        return;
    }
    ecs::emplace<ObjectVisionRangeOverrideComponent>(registry, entity, value);
}

// Runtime conversion API: snapshot both effective source values before
// mutating the target.  This exactly models the two RefCode setter calls and
// also preserves a source override inherited from an earlier conversion.
inline void copyObjectVisionRanges(
    ecs::registry& registry, ecs::entity source, ecs::entity target) {
    const math::q32_32 vision =
        effectiveObjectVisionRangeFixed(registry, source);
    const math::q32_32 shroud =
        effectiveObjectShroudClearingRangeFixed(registry, source);
    setObjectVisionRangeOverride(registry, target, vision, shroud);
}

inline void clearObjectVisionRangeOverride(
    ecs::registry& registry, ecs::entity entity) {
    if (ecs::has<ObjectVisionRangeOverrideComponent>(registry, entity)) {
        ecs::remove<ObjectVisionRangeOverrideComponent>(registry, entity);
    }
}

} // namespace engine
