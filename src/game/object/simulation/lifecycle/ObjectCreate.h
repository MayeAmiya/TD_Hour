#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"

#include "game/object/definition/CombatProfile.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/data/base/UpgradeCatalog.h"

#include <cstddef>
#include <cstdint>
#include <variant>
#include "game/object/plan/lifecycle/ObjectCreatePlanTypes.h"
namespace engine {

class ObjectLifecycle;

enum class ObjectSupplyAnchorKind : uint8_t {
    Center,
    Warehouse,
};

// Sparse replacement for ResourceGatheringManager's replicated Object*
// lists.  Resource AI can view only these entities and sort their stable
// ObjectIds; ordinary tanks/infantry are never scanned.
struct ObjectSupplyAnchorComponent final {
    bool supplyCenterReady = false;
    bool supplyWarehouseReady = false;
    uint64_t revision = 0;
    uint64_t lastChangedTick = 0;
};

[[nodiscard]] container::Vector<ObjectId> collectReadySupplyAnchors(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectSupplyAnchorKind kind);

enum class ObjectCreatePhase : uint8_t {
    Created,
    BuildComplete,
};

struct ObjectCreateExecutionReport final {
    size_t attempted = 0;
    size_t applied = 0;
    size_t skipped = 0;
    size_t failed = 0;
};

} // namespace engine
