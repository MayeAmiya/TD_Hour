#pragma once

#include "core/container/container_types.h"
#include "game/data/base/UpgradeCatalog.h"

#include "core/ecs/registry.h"
#include "game/base/DamageTypes.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"
#include <cstdint>
#include <limits>
#include "game/object/plan/combat/ObjectFireWeaponBehaviorPlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class SimulationRandom;
struct ObjectHealthEvent;

struct ObjectFireWeaponWhenDamagedRuntime final {
    container::Array<ObjectSystemWeaponRuntime, 4> reactionWeapons;
    container::Array<ObjectSystemWeaponRuntime, 4> continuousWeapons;
    bool upgradeActivated = false;
};

struct ObjectFireWeaponWhenDamagedComponent final {
    container::SharedPtr<const game::ObjectFireWeaponWhenDamagedPlan> plan;
    container::Vector<ObjectFireWeaponWhenDamagedRuntime> instances;
};

struct ObjectFireWeaponWhenDeadRuleRuntime final {
    game::WeaponContentId content;
    uint32_t nextShotSequence = 1;
    bool upgradeActivated = false;
    bool playerConflict = false;
};

struct ObjectFireWeaponWhenDeadRuntimeComponent final {
    container::Vector<ObjectFireWeaponWhenDeadRuleRuntime> rules;
};

class ObjectFireWeaponBehaviorSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const GameContentSnapshot& content,
                          uint32_t logicFramesPerSecond,
                          uint64_t confirmedTick) const;

    void onHealthEvent(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectHealthEvent& event,
        const GameContentSnapshot& content, SimulationRandom& random,
        uint32_t logicFramesPerSecond,
        uint64_t& nextEmissionSequence,
        container::Vector<ObjectSystemWeaponFireCommand>& outCommands) const;

    void updateContinuous(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const GameContentSnapshot& content, SimulationRandom& random,
        uint32_t logicFramesPerSecond, uint64_t confirmedTick,
        uint64_t& nextEmissionSequence,
        container::Vector<ObjectSystemWeaponFireCommand>& outCommands) const;

    [[nodiscard]] bool tryFireWhenDead(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        ecs::entity entity, ObjectId object, uint32_t ruleIndex,
        const game::ObjectDeathReactionRule& rule,
        const UpgradeMask& ownerCompletedUpgrades,
        const GameContentSnapshot& content, SimulationRandom& random,
        uint64_t confirmedTick, uint64_t& nextEmissionSequence,
        container::Vector<ObjectSystemWeaponFireCommand>& outCommands) const;
};

} // namespace engine
