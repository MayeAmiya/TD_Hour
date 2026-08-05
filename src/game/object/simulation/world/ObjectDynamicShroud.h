#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "core/ecs/ObjectId.h"
#include "game/player/PlayerTypes.h"
#include "math/fixed/q32_32.h"

#include <array>
#include <cstdint>

#include "game/object/plan/world/ObjectDynamicShroudPlanTypes.h"
namespace engine {

class ObjectLifecycle;
struct ObjectSimulationRules;

enum class ObjectDynamicShroudPhase : uint8_t {
    NotStarted,
    Growing,
    Sustaining,
    Shrinking,
    DoneForever,
    Sleeping,
};

struct ObjectDynamicShroudRuntime final {
    ObjectDynamicShroudPhase phase = ObjectDynamicShroudPhase::NotStarted;
    uint64_t stateCountdown = 0;
    uint64_t totalTicks = 1;
    uint64_t growStartDeadline = 0;
    uint64_t sustainDeadline = 0;
    uint64_t shrinkStartDeadline = 0;
    uint64_t doneForeverTick = 0;
    uint64_t changeIntervalCountdown = 0;
    uint64_t changeIntervalTicks = 0;
    uint64_t growIntervalTicks = 0;
    uint64_t growTimeTicks = 0;
    uint64_t shrinkTimeTicks = 0;
    uint64_t opacityThrobTicks = 0;
    math::q32_32 nativeClearingRange{};
    math::q32_32 currentClearingRange{};
    LogicFixedVec3 decalPresentedPosition{};
    math::q32_32 decalPresentedClearingRange{};
    uint64_t decalPresentedStateCountdown = 0;
    bool decalPresentationSampleValid = false;
    bool decalBeginEmitted = false;
    bool decalEndEmitted = false;
};

// Sparse authoritative range writer. `projectedRadius` is the last value
// actually committed at an interval boundary, not the module's continuously
// animated private value. Multiple authored rules overwrite it in stable
// authored order exactly like multiple legacy UpdateModules on one object.
struct ObjectDynamicShroudComponent final {
    container::SharedPtr<const game::ObjectDynamicShroudPlan> plan;
    container::Vector<ObjectDynamicShroudRuntime> instances;
    math::q32_32 projectedRadius{};
    bool hasProjectedRadius = false;
};

enum class ObjectDynamicShroudDecalEventKind : uint8_t {
    Begin,
    Update,
    End,
};

// Detached presentation edge. Begin freezes everything a future persistent
// RadiusDecal consumer needs; End is emitted at the legacy killGridDecals
// transition. Stable ObjectId lets presentation follow a render snapshot
// without retaining an EnTT entity or mutating gameplay.
struct ObjectDynamicShroudDecalEvent final {
    ObjectDynamicShroudDecalEventKind kind =
        ObjectDynamicShroudDecalEventKind::Begin;
    ObjectId object = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    uint32_t authoredOrder = 0;
    LogicFixedVec3 position{};
    uint32_t decalCount = 30;
    uint32_t gridSnapSize = 23;
    math::q32_32 initialDecalRadius{int32_t{100}};
    math::q32_32 nativeClearingRange{};
    math::q32_32 currentClearingRange{};
    uint64_t stateCountdown = 0;
    uint64_t totalTicks = 1;
    uint64_t growStartDeadline = 0;
    uint64_t sustainDeadline = 0;
    uint64_t shrinkStartDeadline = 0;
    uint64_t opacityThrobTicks = 0;
    game::ObjectDynamicShroudDecalRecipe recipe;
    uint64_t confirmedTick = 0;
};

class ObjectDynamicShroudSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const ObjectSimulationRules& rules,
                          uint64_t createdAtTick) const;

    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                uint64_t confirmedTick,
                container::Vector<ObjectDynamicShroudDecalEvent>&
                    outDecalEvents) const;

    // RefCode's module destructor always kills its persistent decals. The
    // lifecycle owner calls this while the pending entity is still readable,
    // producing End without giving presentation an ECS handle.
    void terminateObject(
        ecs::registry& registry, ecs::entity entity, ObjectId object,
        uint64_t confirmedTick,
        container::Vector<ObjectDynamicShroudDecalEvent>&
            outDecalEvents) const;
};

} // namespace engine
