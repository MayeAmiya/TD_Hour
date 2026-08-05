#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "core/ecs/ObjectId.h"
#include "core/math/fixed/q32_32.h"

#include <cstdint>

#include "game/object/plan/status/ObjectCheckpointPlanTypes.h"
namespace engine {

class ObjectLifecycle;
class PlayerRegistry;
class SimulationRandom;
struct ObjectSimulationRules;

struct ObjectCheckpointRuntime final {
    uint64_t nextScanTick = 0;
    math::q32_32 maximumMinorRadius{};
    bool enemyNear = false;
    bool allyNear = false;
};

struct ObjectCheckpointComponent final {
    container::SharedPtr<const game::ObjectCheckpointPlan> plan;
    container::Vector<ObjectCheckpointRuntime> instances;
};

// Value-change occurrence emitted by the authoritative geometry writer.
// Multiple changes in one confirmed tick remain distinct; Session resolves
// the current footprint by ObjectId without scanning every checkpoint.
struct ObjectCheckpointNavigationEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
    uint64_t submissionOrdinal = 0;
};

class ObjectCheckpointSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const ObjectSimulationRules& rules,
                          SimulationRandom* random) const;

    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                const PlayerRegistry& players,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick,
                uint64_t& nextGameplaySubmissionOrdinal,
                container::Vector<ObjectCheckpointNavigationEvent>&
                    outNavigationEvents) const;
};

} // namespace engine
