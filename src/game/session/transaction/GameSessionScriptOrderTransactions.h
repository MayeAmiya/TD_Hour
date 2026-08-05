#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "core/ecs/registry.h"
#include "game/object/contracts/ObjectFixedGeometryTypes.h"

#include <optional>

namespace game::terrain {
class TerrainLogic;
}

namespace engine {

class ObjectLifecycle;
class GameContentSnapshot;
class ObjectSimulation;
class SimulationRandom;
namespace ai {
class ObjectAIRuntime;
}

// Synchronous deterministic command mutations used by confirmed script
// effects. ECS queue revisions and detached ObjectAI state commit together.
class GameSessionScriptOrderTransactions final {
public:
    GameSessionScriptOrderTransactions(
        ecs::registry& registry, ObjectLifecycle& objects,
        ai::ObjectAIRuntime& objectAI,
        const GameContentSnapshot& content,
        game::terrain::TerrainLogic& terrain,
        SimulationRandom& random,
        ObjectSimulation& simulation) noexcept;

    [[nodiscard]] bool face(
        ObjectId actor, ObjectId targetObject,
        const std::optional<LogicFixedVec3>& targetPosition,
        uint64_t confirmedTick);
    [[nodiscard]] bool fireWeaponFollowingWaypointPath(
        ObjectId object, container::StringView waypointPath,
        uint32_t authoredOrder, uint64_t confirmedTick);

private:
    ecs::registry& m_registry;
    ObjectLifecycle& m_objects;
    ai::ObjectAIRuntime& m_objectAI;
    const GameContentSnapshot& m_content;
    game::terrain::TerrainLogic& m_terrain;
    SimulationRandom& m_random;
    ObjectSimulation& m_simulation;
};

} // namespace engine
