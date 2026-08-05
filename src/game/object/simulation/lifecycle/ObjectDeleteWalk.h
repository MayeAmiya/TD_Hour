#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/contracts/ObjectOnDeletePlan.h"

#include <cstdint>
#include <optional>

namespace engine {

// Object::onDestroy invokes BehaviorModule::onDelete in final authored
// module order.  This value-only continuation lets the Session close every
// child Damage/Destroy transaction before advancing to the next occurrence.
enum class ObjectDeleteWalkPhase : uint8_t {
    Behaviors,
    Postamble,
    Completed,
};

enum class ObjectDeleteWalkAdvance : uint8_t {
    BehaviorHandled,
    ReadyForPostamble,
    InvalidState,
};

struct ObjectDeleteWalkState final {
    ObjectId object = INVALID_OBJECT_ID;
    container::SharedPtr<const game::ObjectOnDeletePlan> plan;
    uint32_t nextEntryIndex = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
    ObjectDeleteWalkPhase phase = ObjectDeleteWalkPhase::Behaviors;
};

// A structural child emitted by an authored onDelete occurrence.  It enters
// the same Session-owned gameplay stack as Weapon/Damage rather than asking
// ObjectLifecycle to create an independently ordered side cascade.
struct ObjectDeleteDestroyRequest final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectDestroyReason reason = ObjectDestroyReason::System;
    ObjectId source = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    uint32_t localOrdinal = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
};

// Root destroy transaction postamble.  It is emitted only after every
// authored onDelete occurrence and all recursively-created child work have
// completed, matching GameLogic::destroyObject's partition/pathfinder/index
// cleanup boundary.
struct ObjectDeletePostambleEvent final {
    ObjectId object = INVALID_OBJECT_ID;
    uint64_t confirmedTick = 0;
};

[[nodiscard]] inline std::optional<game::ObjectOnDeleteEntry>
takeNextObjectDeleteEntry(ObjectDeleteWalkState& state) {
    if (!state.plan || state.nextEntryIndex >= state.plan->entries.size()) {
        return std::nullopt;
    }
    return state.plan->entries[state.nextEntryIndex++];
}

} // namespace engine
