#pragma once

#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace engine {

// One-shot ToppleUpdate ingress. Unlike persistent ECS state this value may
// occur more than once for the same object in one confirmed tick, so it is
// appended to a per-registry deterministic journal rather than emplaced as a
// single target component.
struct ObjectToppleRequest final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId source = INVALID_OBJECT_ID;
    LogicFixedVec3 direction{};
    math::q32_32 speed{};
    uint32_t sourceSequence = 0;
    uint64_t confirmedTick = 0;
    bool noBounce = true;
    bool noFx = true;
};

// Logic-owner-thread ingress. The journal assigns a monotonic submission
// ordinal; workers must merge deterministic staging before calling this API.
void queueObjectToppleRequest(ecs::registry& registry,
                              ObjectToppleRequest request);

} // namespace engine
