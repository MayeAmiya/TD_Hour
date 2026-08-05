#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include "game/object/plan/combat/ObjectSmartBombPlanTypes.h"
namespace engine {
class ObjectLifecycle;
struct ObjectSimulationRules;

struct ObjectSmartBombRuleRuntime final {
    LogicFixedVec3 target{};
    bool targetReceived = false;
};

struct ObjectSmartBombComponent final {
    container::SharedPtr<const game::ObjectSmartBombPlan> plan;
    container::Vector<ObjectSmartBombRuleRuntime> rules;
};

class ObjectSmartBombSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;
    [[nodiscard]] bool setTarget(ecs::registry& registry,
                                 const ObjectLifecycle& lifecycle,
                                 ObjectId object,
                                 const LogicFixedVec3& target) const;
    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                const game::terrain::TerrainLogic& terrain,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick) const;
};
} // namespace engine
