#pragma once

#include "core/ecs/registry.h"

#include <cstdint>

namespace engine {

enum class ObjectDirtyDomain : uint8_t {
    Spatial = 1u << 0u,
    ModelCondition = 1u << 1u,
    RenderExtraction = 1u << 2u,
};

[[nodiscard]] constexpr uint8_t objectDirtyBit(
    ObjectDirtyDomain domain) noexcept {
    return static_cast<uint8_t>(domain);
}

struct ObjectDirtyComponent final {
    uint8_t domains = 0;
};

// Registry-local monotonic revisions let read-side caches test a dirty domain
// without walking even the sparse ObjectDirtyComponent storage. Components
// remain the per-object work list; revisions are only invalidation tokens.
struct ObjectDirtyRevisionState final {
    uint64_t spatial = 0;
    uint64_t modelCondition = 0;
    uint64_t renderExtraction = 0;
};

[[nodiscard]] inline uint64_t objectDirtyRevision(
    const ecs::registry& registry, ObjectDirtyDomain domain) noexcept {
    const ObjectDirtyRevisionState* state =
        registry.ctx().find<ObjectDirtyRevisionState>();
    if (!state) return 0;
    switch (domain) {
    case ObjectDirtyDomain::Spatial:
        return state->spatial;
    case ObjectDirtyDomain::ModelCondition:
        return state->modelCondition;
    case ObjectDirtyDomain::RenderExtraction:
        return state->renderExtraction;
    }
    return 0;
}

inline void advanceObjectDirtyRevisions(
    ecs::registry& registry, uint8_t newlyDirtyDomains) {
    if (newlyDirtyDomains == 0) return;
    ObjectDirtyRevisionState* state =
        registry.ctx().find<ObjectDirtyRevisionState>();
    if (!state) {
        state = &registry.ctx().emplace<ObjectDirtyRevisionState>();
    }
    if ((newlyDirtyDomains & objectDirtyBit(ObjectDirtyDomain::Spatial)) != 0)
        ++state->spatial;
    if ((newlyDirtyDomains &
         objectDirtyBit(ObjectDirtyDomain::ModelCondition)) != 0)
        ++state->modelCondition;
    if ((newlyDirtyDomains &
         objectDirtyBit(ObjectDirtyDomain::RenderExtraction)) != 0)
        ++state->renderExtraction;
}

inline void markObjectDirty(
    ecs::registry& registry, ecs::entity entity,
    uint8_t domains) {
    if (domains == 0 || entity == ecs::null || !registry.valid(entity)) return;
    ObjectDirtyComponent* dirty =
        ecs::try_get<ObjectDirtyComponent>(registry, entity);
    const uint8_t existingDomains = dirty ? dirty->domains : 0;
    if (!dirty) {
        dirty = &ecs::emplace<ObjectDirtyComponent>(
            registry, entity, ObjectDirtyComponent{});
    }
    dirty->domains |= domains;
    advanceObjectDirtyRevisions(
        registry, static_cast<uint8_t>(domains & ~existingDomains));
}

inline void markObjectDirty(
    ecs::registry& registry, ecs::entity entity,
    ObjectDirtyDomain domain) {
    markObjectDirty(registry, entity, objectDirtyBit(domain));
}

inline void clearObjectDirty(
    ecs::registry& registry, ecs::entity entity,
    ObjectDirtyDomain domain) {
    ObjectDirtyComponent* dirty =
        ecs::try_get<ObjectDirtyComponent>(registry, entity);
    if (!dirty) return;
    dirty->domains &= static_cast<uint8_t>(~objectDirtyBit(domain));
    if (dirty->domains == 0) {
        ecs::remove<ObjectDirtyComponent>(registry, entity);
    }
}

inline constexpr uint8_t kObjectDirtyAll =
    objectDirtyBit(ObjectDirtyDomain::Spatial) |
    objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
    objectDirtyBit(ObjectDirtyDomain::RenderExtraction);

} // namespace engine
