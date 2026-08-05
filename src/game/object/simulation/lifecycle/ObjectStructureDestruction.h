#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/simulation/combat/ObjectBoneFx.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

namespace game::terrain {
class TerrainLogic;
}

namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class SimulationRandom;
struct ObjectSimulationRules;

enum class ObjectStructureEffectKind : uint8_t {
    FxList,
    ObjectCreationList,
};

enum class ObjectStructureEffectAnchor : uint8_t {
    WorldPosition,
    ObjectAttachment,
};

// Shared, detached phase stream for StructureTopple/Collapse. FX and OCL use
// the same gameplay sequence as the adjacent crushing-weapon commands, so the
// session can replay the original synchronous weapon -> FX -> line-OCL order
// without keeping a legacy FXList/OCL/Weapon pointer.
struct ObjectStructureEffectEvent final {
    ObjectStructureEffectKind kind = ObjectStructureEffectKind::FxList;
    ObjectStructureEffectAnchor anchor =
        ObjectStructureEffectAnchor::WorldPosition;
    ObjectId object = INVALID_OBJECT_ID;
    LogicFixedVec3 position{};
    math::q32_32 orientationRadians{};
    uint32_t sourcePathfindLayer = 0;
    container::String resource;
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
};

enum class ObjectStructureMotionState : uint8_t {
    Waiting,
    Moving,
    WaitingForDone,
    Done,
};

struct ObjectStructureToppleRuntime final {
    container::SharedPtr<const game::ObjectDeathReactionPlan> plan;
    uint32_t ruleIndex = 0;
    ObjectId damageSource = INVALID_OBJECT_ID;
    LogicFixedVec3 origin{};
    math::q32_32 originalYaw{};
    uint32_t sourcePathfindLayer = 0;
    math::q32_32 directionX{int32_t{1}};
    math::q32_32 directionY{};
    math::q32_32 accumulatedAngle =
        math::q32_32::from_fraction(1, 1000);
    math::q32_32 velocity{};
    math::q32_32 structuralIntegrity{};
    math::q32_32 lastCrushedDistance{};
    math::q32_32 buildingHeight{};
    math::q32_32 majorRadius{};
    math::q32_32 minorRadius{};
    LogicFixedVec3 delayBurstPosition{};
    uint64_t randomKey = 0;
    uint64_t startTick = 0;
    uint64_t nextBurstTick = 0;
    uint32_t nextShotSequence = 1;
    uint32_t phaseSequence = 0;
    ObjectStructureMotionState state = ObjectStructureMotionState::Waiting;
    bool damageFxAccepted = true;
};

struct ObjectStructureCollapseRuntime final {
    container::SharedPtr<const game::ObjectDeathReactionPlan> plan;
    uint32_t ruleIndex = 0;
    LogicFixedVec3 origin{};
    math::q32_32 orientationRadians{};
    uint32_t sourcePathfindLayer = 0;
    math::q32_32 buildingHeight{};
    math::q32_32 currentHeight{};
    math::q32_32 velocity{};
    math::q32_32 visualShudderX{};
    math::q32_32 visualShudderY{};
    uint64_t randomKey = 0;
    uint64_t startTick = 0;
    uint64_t nextBurstTick = 0;
    uint32_t phaseSequence = 0;
    ObjectStructureMotionState state = ObjectStructureMotionState::Waiting;
};

struct ObjectStructureDestructionComponent final {
    container::Vector<ObjectStructureToppleRuntime> topples;
    container::Vector<ObjectStructureCollapseRuntime> collapses;
};

class ObjectStructureDestructionSystem final {
public:
    [[nodiscard]] bool begin(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ecs::entity entity, ObjectId object, ObjectId damageSource,
        game::DamageType damageType,
        container::SharedPtr<const game::ObjectDeathReactionPlan> plan,
        uint32_t ruleIndex, const ObjectSimulationRules& rules,
        uint64_t sessionSeed, uint64_t confirmedTick,
        uint64_t& nextEffectSequence,
        container::Vector<ObjectStructureEffectEvent>& effects) const;

    void update(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const game::terrain::TerrainLogic& terrain,
        const GameContentSnapshot* content, SimulationRandom* random,
        const ObjectSimulationRules& rules, uint64_t confirmedTick,
        uint64_t& nextEffectSequence, uint64_t& nextWeaponSequence,
        container::Vector<ObjectStructureEffectEvent>& effects,
        container::Vector<ObjectSystemWeaponFireCommand>& weaponCommands,
        container::Vector<ObjectBoneFxStopRequest>& boneStops) const;
};

} // namespace engine
