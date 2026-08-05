#pragma once

#include <cstdint>

#include "core/ecs/registry.h"
#include "game/object/contracts/ObjectDeathReaction.h"

namespace engine
{

// One deterministic mutation request.  StatusToSet is applied before
// StatusToClear, matching RefCode's StatusBitsUpgrade implementation; an
// overlapping clear therefore wins.
struct ObjectStatusMutation final
{
    game::ObjectStatusMask setMask = 0;
    game::ObjectStatusMask clearMask = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectStatusTransition final
{
    game::ObjectStatusMask previous = 0;
    game::ObjectStatusMask current = 0;
    game::ObjectStatusMask newlySet = 0;
    game::ObjectStatusMask newlyCleared = 0;
    ObjectStatusDependencyMask dependencies = 0;

    [[nodiscard]] bool changed() const noexcept
    {
        return newlySet != 0 || newlyCleared != 0;
    }
};

// Central write boundary for ObjectStatusComponent.  This replaces scattered
// `flags |=`/`flags &=` mutations without recreating Object's legacy web of
// PartitionManager and Drawable pointers.  Dependent ECS systems observe the
// revision/pending dependency bits and consume the derived work at their own
// deterministic phase boundary.
class ObjectStatusSystem final
{
public:
    [[nodiscard]] static ObjectStatusTransition apply(ecs::registry& registry,
                                                      ecs::entity entity,
                                                      const ObjectStatusMutation& mutation);

    // Acknowledges only the requested dependency classes and returns the bits
    // which were actually pending.  This makes multiple independent consumers
    // safe without requiring them to clear the whole invalidation word.
    [[nodiscard]] static ObjectStatusDependencyMask acknowledgeDependencies(
        ObjectStatusComponent& status, ObjectStatusDependencyMask dependencies) noexcept;
};

} // namespace engine
