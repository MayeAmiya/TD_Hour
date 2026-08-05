#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"
#include <cstdint>
#include <limits>
#include "game/object/plan/status/ObjectEmpUpdatePlanTypes.h"
namespace engine {

class ObjectLifecycle;
class PlayerRegistry;
class SimulationRandom;
struct ObjectDamageRequest;

struct ObjectEmpRuntime final {
    uint64_t dieTick = 0;
    uint64_t fadeTick = 0;
    math::q32_32 currentScale{1};
    math::q32_32 targetScale{1};
    bool randomized = false;
    bool effectApplied = false;
    bool killRequested = false;
};

struct ObjectEmpUpdateComponent final {
    container::SharedPtr<const game::ObjectEmpPlan> plan;
    container::Vector<ObjectEmpRuntime> instances;
    math::q32_32 visualScale{1};
    math::q32_32 visualBlend{};
    uint32_t visualRuleIndex = UINT32_MAX;
    bool visualActive = false;
    uint64_t lastUpdatedTick = std::numeric_limits<uint64_t>::max();
};

// Presentation value for DisableFXParticleSystem. Logic computes only the
// deterministic target/count/lifetime facts; the particle service owns
// emitter offsets, handles and device lifetime.
struct ObjectEmpParticleEvent final {
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    container::String particleSystem;
    uint32_t emitterCount = 0;
    uint32_t systemLifetimeFrames = 0;
    float footprintMajorRadius = 0.0f;
    float footprintMinorRadius = 0.0f;
    float maximumHeight = 0.0f;
    bool boxFootprint = false;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

class ObjectEmpUpdateSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          SimulationRandom* random,
                          uint32_t logicFramesPerSecond,
                          uint64_t createdAtTick) const;

    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                const PlayerRegistry& players, SimulationRandom& random,
                uint32_t logicFramesPerSecond, uint64_t confirmedTick,
                container::Vector<ObjectDamageRequest>& outDamage,
                container::Vector<ObjectEmpParticleEvent>& outParticles);

private:
    ObjectSpatialIndex m_currentPositionIndex;
};

} // namespace engine
