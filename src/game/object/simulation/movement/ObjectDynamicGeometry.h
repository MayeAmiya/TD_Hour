#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <array>
#include <cstdint>

#include "game/object/plan/movement/ObjectDynamicGeometryPlanTypes.h"
namespace engine {

class ObjectLifecycle;
struct ObjectSimulationRules;

struct ObjectDynamicGeometryRuntime final {
    uint64_t startingDelayCountdown = 1;
    uint64_t earliestStartTick = 1;
    uint64_t timeActive = 0;
    uint64_t transitionTicks = 1;
    uint64_t damageIntervalTicks = 0;
    uint64_t lastDamageTick = 0;
    math::q32_32 initialHeight{};
    math::q32_32 initialMajorRadius{};
    math::q32_32 initialMinorRadius{};
    math::q32_32 finalHeight{};
    math::q32_32 finalMajorRadius{};
    math::q32_32 finalMinorRadius{};
    math::q32_32 currentBoundingCircleRadius{};
    bool started = false;
    bool finished = false;
    bool reversePending = false;
    bool switchedDirections = false;
    bool effectsStarted = false;
    bool scorchPlaced = false;
    bool stopEmitted = false;
};

struct ObjectDynamicGeometryComponent final {
    container::SharedPtr<const game::ObjectDynamicGeometryPlan> plan;
    container::Vector<ObjectDynamicGeometryRuntime> instances;
};

// Confirmed gameplay occurrence emitted at the exact authored activation
// edge.  It is intentionally separate from the renderer-neutral particle
// journal: academy progression must not depend on a presentation drain.
struct ObjectDynamicGeometryGameplayEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
    uint64_t submissionOrdinal = 0;
    container::Vector<ObjectDamageRequest> damage;
    bool firestormCreated = false;
};

enum class ObjectDynamicGeometryPresentationEventKind : uint8_t {
    Start,
    RadiusUpdate,
    Scorch,
    Stop,
};

// Renderer-neutral persistent-effect journal. ObjectId + authoredOrder is the
// stable handle key. GameSession maps Start and Stop to today's lossless FX
// stream; RadiusUpdate remains an explicit value edge for the persistent
// emission-volume adapter. No EnTT entity or renderer handle enters confirmed
// simulation.
struct ObjectDynamicGeometryPresentationEvent final {
    ObjectDynamicGeometryPresentationEventKind kind =
        ObjectDynamicGeometryPresentationEventKind::Start;
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
    LogicFixedVec3 objectPosition{};
    LogicFixedVec3 particlePosition{};
    math::q32_32 majorRadius{};
    math::q32_32 scorchSize{};
    std::array<container::String, 16> particleSystems;
    container::String fxList;
};

class ObjectDynamicGeometrySystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const ObjectSimulationRules& rules) const;

    // Geometry is written before collision/spatial consumers. Firestorm
    // damage leaves only fixed-point requests; ObjectSimulation remains the
    // sole Body/Health writer and resolves them at its normal barrier.
    void update(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const game::terrain::TerrainLogic& terrain, uint64_t confirmedTick,
        uint64_t& nextGameplaySubmissionOrdinal,
        container::Vector<ObjectDynamicGeometryGameplayEvent>& outGameplay,
        container::Vector<ObjectDynamicGeometryPresentationEvent>&
            outPresentation) const;

    // Called at the stable DestroyRequested boundary while the pending entity
    // still owns its sparse component, guaranteeing one Stop edge even when
    // physical ECS removal happens before another update pass.
    void onObjectReclaim(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick,
        container::Vector<ObjectDynamicGeometryPresentationEvent>&
            outPresentation) const;
};

} // namespace engine
