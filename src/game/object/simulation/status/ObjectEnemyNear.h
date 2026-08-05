#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

#include "game/object/plan/status/ObjectEnemyNearPlanTypes.h"
namespace engine {

class ObjectLifecycle;
class PlayerRegistry;
class SimulationRandom;
struct ObjectSimulationRules;

struct ObjectEnemyNearRuntime final {
    uint64_t nextScanTick = 0;
    bool enemyNear = false;
};

struct ObjectEnemyNearComponent final {
    container::SharedPtr<const game::ObjectEnemyNearPlan> plan;
    container::Vector<ObjectEnemyNearRuntime> instances;
};

class ObjectEnemyNearSystem final {
public:
    void initializeObject(
        ecs::registry& registry, ecs::entity entity,
        const ObjectSimulationRules& rules, SimulationRandom* random) const;

    void update(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, const ObjectSimulationRules& rules,
        const game::terrain::MapVisibilitySnapshot* visibility,
        uint64_t confirmedTick) const;
};

} // namespace engine
