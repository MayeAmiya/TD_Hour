#pragma once

#include "core/ecs/registry.h"
#include "game/object/simulation/containment/ObjectContainment.h"

namespace engine {

namespace ai { class ObjectAIRuntime; }

class GameContentSnapshot;
class ObjectLifecycle;
class ObjectOwnershipIndex;
class ObjectSimulation;
class ObjectSpatialIndex;
class ObjectTeamRegistry;
class PlayerRegistry;

// Owns confirmed containment admission and its ECS intent projection. Higher
// level team/player planners select participants; this service commits one
// canonical attach/detach or approach transaction.
class GameSessionContainmentTransactions final {
public:
    GameSessionContainmentTransactions(
        ecs::registry& registry, ObjectLifecycle& objects,
        ObjectSimulation& simulation, ObjectSpatialIndex& spatialIndex,
        ObjectTeamRegistry& objectTeams, PlayerRegistry& players,
        const GameContentSnapshot& content) noexcept;

    [[nodiscard]] bool request(ObjectContainmentRequest request);
    [[nodiscard]] bool setEvacuationDisposition(
        ObjectId container,
        ObjectContainmentEvacuationDisposition disposition);
    [[nodiscard]] bool requestObjectEnter(
        ObjectId object, ObjectId container, uint32_t sourceSequence,
        uint64_t confirmedTick, uint32_t reservedCapacity = 0);
    [[nodiscard]] bool requestObjectGarrison(
        ObjectId object, std::optional<ObjectId> building,
        uint32_t sourceSequence, uint64_t confirmedTick);
    [[nodiscard]] bool requestPlayerExit(
        ObjectId container, ObjectId passenger, uint64_t confirmedTick,
        const ObjectOwnershipIndex& ownership,
        ai::ObjectAIRuntime& objectAI);
    [[nodiscard]] size_t requestTeamEnter(
        ObjectTeamId team, ObjectId container, bool requireGarrison,
        uint32_t sourceSequence, uint64_t confirmedTick);

private:
    ecs::registry& m_registry;
    ObjectLifecycle& m_objects;
    ObjectSimulation& m_simulation;
    ObjectSpatialIndex& m_spatialIndex;
    ObjectTeamRegistry& m_objectTeams;
    PlayerRegistry& m_players;
    const GameContentSnapshot& m_content;
};

} // namespace engine
