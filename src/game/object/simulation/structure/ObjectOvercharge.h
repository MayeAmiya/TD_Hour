#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

#include "game/object/plan/structure/ObjectOverchargePlanTypes.h"
namespace engine {

class ObjectLifecycle;
struct ObjectDamageRequest;
struct ObjectSimulationRules;

struct ObjectOverchargeRuntime final {
    bool active = false;
};

struct ObjectOverchargeComponent final {
    container::SharedPtr<const game::ObjectOverchargePlan> plan;
    container::Vector<ObjectOverchargeRuntime> instances;
};

class ObjectOverchargeSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    [[nodiscard]] bool setActive(ecs::registry& registry,
                                 const ObjectLifecycle& lifecycle,
                                 ObjectId object,
                                 bool active,
                                 const ObjectSimulationRules& rules,
                                 uint64_t confirmedTick) const;
    [[nodiscard]] bool toggle(ecs::registry& registry,
                              const ObjectLifecycle& lifecycle,
                              ObjectId object,
                              const ObjectSimulationRules& rules,
                              uint64_t confirmedTick) const;
    [[nodiscard]] bool isActive(const ecs::registry& registry,
                                const ObjectLifecycle& lifecycle,
                                ObjectId object) const noexcept;

    void update(ecs::registry& registry,
                ObjectLifecycle& lifecycle,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick,
                container::Vector<ObjectDamageRequest>& outDamage) const;
};

} // namespace engine
