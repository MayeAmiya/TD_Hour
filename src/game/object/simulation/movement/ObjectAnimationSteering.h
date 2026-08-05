#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <limits>

#include "game/object/plan/movement/ObjectAnimationSteeringPlanTypes.h"
namespace engine {

class ObjectLifecycle;
struct ObjectSimulationRules;

enum class ObjectSteeringAnimationPhase : uint8_t {
    Centered,
    CenterToRight,
    CenterToLeft,
    RightToCenter,
    LeftToCenter,
};

struct ObjectAnimationSteeringRuntime final {
    ObjectSteeringAnimationPhase phase =
        ObjectSteeringAnimationPhase::Centered;
    uint64_t nextTransitionTick = 0;
};

struct ObjectAnimationSteeringComponent final {
    container::SharedPtr<const game::ObjectAnimationSteeringPlan> plan;
    container::Vector<ObjectAnimationSteeringRuntime> instances;
    math::q32_32 previousRotation{};
    uint64_t nextDueTick = std::numeric_limits<uint64_t>::max();
    bool rotationInitialized = false;
};

class ObjectAnimationSteeringSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick) const;
};

} // namespace engine
