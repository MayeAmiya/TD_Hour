#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"

#include <cstdint>
#include "game/object/plan/combat/ObjectFireWeaponUpdatePlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class SimulationRandom;

struct ObjectFireWeaponUpdateRuntime final {
    ObjectSystemWeaponRuntime weapon;
    uint64_t initialDelayCompleteTick = 0;
};

struct ObjectFireWeaponUpdateComponent final {
    container::SharedPtr<const game::ObjectFireWeaponUpdatePlan> plan;
    container::Vector<ObjectFireWeaponUpdateRuntime> instances;
};

class ObjectFireWeaponUpdateSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const GameContentSnapshot& content,
                          uint32_t logicFramesPerSecond,
                          uint64_t confirmedTick) const;

    void update(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const GameContentSnapshot& content, SimulationRandom& random,
        uint32_t logicFramesPerSecond, uint64_t confirmedTick,
        uint64_t& nextEmissionSequence,
        container::Vector<ObjectSystemWeaponFireCommand>& outCommands) const;
};

} // namespace engine
