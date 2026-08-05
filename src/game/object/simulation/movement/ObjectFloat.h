#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "core/math/wwmath/quaternion/quat.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>
#include <optional>
#include "game/object/plan/movement/ObjectFloatPlanTypes.h"
namespace engine {

class ObjectLifecycle;

struct ObjectFloatRuntime final {
    bool enabled = false;
};

struct ObjectFloatComponent final {
    container::SharedPtr<const game::ObjectFloatPlan> plan;
    container::Vector<ObjectFloatRuntime> instances;
    // Last global logic tick on which the UpdateModule was allowed to run.
    // Extraction samples sway at this tick, so Disabled freezes the existing
    // Drawable matrix and recovery jumps back to the global phase like Ref.
    uint64_t visualSampleTick = 0;
};

// A future OCL Disposition=FLOATING consumer can enable either every final
// FloatUpdate occurrence or one exact authored occurrence without discovering
// a live ECS entity or retaining a legacy UpdateModule pointer.
struct ObjectFloatEnableRequest final {
    ObjectId object = INVALID_OBJECT_ID;
    bool enabled = false;
    std::optional<uint32_t> authoredOrder;
};

class ObjectFloatSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    [[nodiscard]] bool setEnabled(ecs::registry& registry,
                                  const ObjectLifecycle& lifecycle,
                                  const ObjectFloatEnableRequest& request) const;

    // Runs after all current position writers. It is intentionally a direct
    // water-surface snap, not a buoyancy or velocity system: that is exactly
    // the state transition owned by the legacy FloatUpdate module.
    void update(ecs::registry& registry, const ObjectLifecycle& lifecycle,
                const game::terrain::TerrainLogic& terrain,
                uint64_t confirmedTick) const;
};

} // namespace engine
