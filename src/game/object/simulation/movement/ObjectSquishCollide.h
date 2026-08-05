#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

#include "game/object/plan/movement/ObjectSquishCollidePlanTypes.h"
namespace engine {

class ObjectLifecycle;
class ObjectSpatialIndex;
class PlayerRegistry;
struct ObjectSimulationRules;
struct ObjectDamageRequest;

struct ObjectSquishCollideComponent final {
    container::SharedPtr<const game::ObjectSquishCollidePlan> plan;
};

class ObjectSquishCollideSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity) const;

    void update(ecs::registry& registry, ObjectLifecycle& lifecycle,
                const ObjectSpatialIndex& spatialIndex,
                const game::terrain::TerrainLogic& terrain,
                const PlayerRegistry& players,
                const ObjectSimulationRules& rules,
                uint64_t confirmedTick,
                container::Vector<ObjectDamageRequest>& outDamage) const;
};

} // namespace engine
