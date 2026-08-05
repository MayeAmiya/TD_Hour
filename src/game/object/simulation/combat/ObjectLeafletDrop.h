#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

#include "game/object/plan/combat/ObjectLeafletDropPlanTypes.h"
namespace engine {

class ObjectLifecycle;
class PlayerRegistry;

struct ObjectLeafletDropRuntime final {
    uint64_t startTick = 0;
    bool particleEmitted = false;
};

struct ObjectLeafletDropComponent final {
    container::SharedPtr<const game::ObjectLeafletDropPlan> plan;
    container::Vector<ObjectLeafletDropRuntime> instances;
    uint64_t lastUpdatedTick = UINT64_MAX;
};

struct ObjectLeafletParticleEvent final {
    ObjectId source = INVALID_OBJECT_ID;
    container::String particleSystem;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

class ObjectLeafletDropSystem final {
public:
    void reset() noexcept {
        m_currentPositionIndex.clear();
        container::Vector<ObjectId>{}.swap(m_nearbyScratch);
    }

    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          uint32_t logicFramesPerSecond,
                          uint64_t createdAtTick) const;

    void update(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, uint32_t logicFramesPerSecond,
        uint64_t confirmedTick,
        container::Vector<ObjectLeafletParticleEvent>& outParticles);

    // Executes the module's non-terminal Die callback in authored order.
    [[nodiscard]] bool onDie(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, ObjectId object,
        uint32_t authoredOrder, uint32_t logicFramesPerSecond,
        uint64_t confirmedTick);

private:
    void disableAttack(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, ecs::entity sourceEntity,
        ObjectId source, const game::ObjectLeafletDropParameters& rule,
        uint32_t logicFramesPerSecond, uint64_t confirmedTick);

    ObjectSpatialIndex m_currentPositionIndex;
    // update()/onDie() are serialized by ObjectSimulation. Reuse one ordered
    // candidate buffer across rules and ticks; no element reference escapes
    // disableAttack().
    container::Vector<ObjectId> m_nearbyScratch;
};

} // namespace engine
