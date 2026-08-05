#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include "game/object/plan/lifecycle/ObjectHeightDiePlanTypes.h"
namespace engine {

struct ObjectSimulationRules;
class ObjectLifecycle;

// One old HeightDieUpdate object carried these facts independently.  In
// particular, `lastPosition` intentionally starts at (-1,-1,-1), matching
// RefCode's first OnlyWhenMovingDown comparison rather than prewarming it at
// spawn or during InitialDelay.
struct ObjectHeightDieRuntime final {
    static constexpr uint64_t NeverTick = UINT64_MAX;

    uint64_t earliestDeathTick = NeverTick;
    LogicFixedVec3 lastPosition{
        .x = math::q32_32{int32_t{-1}},
        .y = math::q32_32{int32_t{-1}},
        .z = math::q32_32{int32_t{-1}},
    };
    bool hasDied = false;
    bool attachedParticlesDestroyed = false;
};

struct ObjectHeightDieComponent final {
    container::SharedPtr<const game::ObjectHeightDiePlan> plan;
    container::Vector<ObjectHeightDieRuntime> instances;
};

// HeightDie is an Update module, not a direct lifecycle owner.  The system
// emits this value command after it has performed any allowed position snap;
// ObjectSimulation then routes the kill through the single Body/Die writer.
struct ObjectHeightDieCommand final {
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
};

// `DestroyAttachedParticlesAtHeight` owns no renderer handle.  It publishes a
// one-shot identity-only request so the presentation side can release all
// effects attached to an ObjectId without observing a live ECS entity.
struct ObjectHeightDiePresentationEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

class ObjectHeightDieSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    // Runs after position writers. Commands and presentation events are
    // emitted in stable ObjectId then authored-rule order, never EnTT view
    // order. The terrain query itself is an explicit fixed-point boundary.
    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                const game::terrain::TerrainLogic& terrain,
                const ObjectSimulationRules& rules, uint64_t confirmedTick,
                container::Vector<ObjectHeightDieCommand>& outCommands,
                container::Vector<ObjectHeightDiePresentationEvent>& outPresentation) const;

private:
    [[nodiscard]] static uint64_t millisecondsToTicks(uint32_t milliseconds,
                                                       uint32_t framesPerSecond) noexcept;
};

} // namespace engine
