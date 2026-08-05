#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/object/creation/ObjectCreationListRuntime.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include "game/object/plan/special/ObjectOclUpdatePlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class PlayerRegistry;
class SimulationRandom;

struct ObjectOclUpdateRuntime final {
    game::ObjectCreationListContentId defaultContent;
    container::Vector<game::ObjectCreationListContentId> factionContent;
    uint64_t timerStartedTick = 0;
    uint64_t nextCreationTick = 0;
    PlayerId controllingPlayer = INVALID_PLAYER_ID;
    bool factionNeutral = true;
    bool timerArmed = false;
};

struct ObjectOclUpdateComponent final {
    container::SharedPtr<const game::ObjectOclUpdatePlan> plan;
    container::Vector<ObjectOclUpdateRuntime> instances;
};

class ObjectOclUpdateSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const GameContentSnapshot& content) const;

    // SabotageSupplyDropzone's modern replacement for
    // Object::findUpdateModule("OCLUpdate")->resetTimer().  RefCode resolves
    // only the first authored OCLUpdate occurrence, so later occurrences must
    // retain their independent deadlines.
    [[nodiscard]] bool resetTimers(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, SimulationRandom& random,
        uint32_t logicFramesPerSecond, uint64_t confirmedTick) const;

    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                const PlayerRegistry& players,
                const GameContentSnapshot& content,
                const game::terrain::TerrainLogic& terrain,
                SimulationRandom& random, uint32_t logicFramesPerSecond,
                uint64_t confirmedTick, uint64_t& nextEmissionSequence,
                container::Vector<ObjectCreationListInvocation>& outInvocations) const;
};

} // namespace engine
