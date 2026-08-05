#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

#include "game/object/plan/lifecycle/ObjectCleanupHazardPlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class SimulationRandom;
struct ObjectSimulationRules;

struct ObjectCleanupHazardRuntime final {
    ObjectId bestTarget = INVALID_OBJECT_ID;
    uint64_t nextScanTick = 0;
    uint64_t lastUpdateTick = UINT64_MAX;
    uint64_t observedExternalOrderRevision = 0;
    LogicFixedVec3 areaCenter{};
    math::q32_32 moveRange{};
    uint32_t nextCommandSequence = 1;
    bool inWeaponRange = false;
    bool areaActive = false;
    bool ownsTemporaryWeaponLock = false;
};

struct ObjectCleanupHazardComponent final {
    container::SharedPtr<const game::ObjectCleanupHazardPlan> plan;
    container::Vector<ObjectCleanupHazardRuntime> instances;
};

class ObjectCleanupHazardSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          uint64_t confirmedTick) const;

    // Runs at the pre-combat barrier.  It emits only typed System Move/Attack
    // intents; Combat and locomotion remain the sole weapon/position writers.
    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                const GameContentSnapshot& content,
                const ObjectSimulationRules& rules, SimulationRandom* random,
                uint64_t confirmedTick) const;

    // CleanupAreaPower augments the first CleanupHazardUpdate occurrence,
    // matching RefCode's findUpdateModule() behavior.
    [[nodiscard]] bool activateArea(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, const LogicFixedVec3& center,
        math::q32_32 moveRange, uint32_t logicFramesPerSecond,
        uint64_t confirmedTick) const;
};

} // namespace engine
