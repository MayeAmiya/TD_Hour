#pragma once

#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/simulation/runtime/ObjectHealthEvents.h"

#include <cstdint>
#include <cstddef>
#include <optional>

namespace engine {

// Explicit continuation phase for Object::onDie.  The synchronous adapter
// currently advances the same value through every phase; the session-owned
// transaction executor will later suspend only at these boundaries.
enum class ObjectDeathWalkPhase : uint8_t {
    Preamble,
    Behaviors,
    Postamble,
    Completed,
};

// Result of advancing exactly one authored onDie Behavior.  A handled entry
// may have emitted child gameplay transactions; the session must close those
// children before presenting this walk again.  ReadyForPostamble means no
// authored Behavior remains and the fixed Object::onDie suffix may commit.
enum class ObjectDeathWalkAdvance : uint8_t {
    BehaviorHandled,
    ReadyForPostamble,
    InvalidState,
};

// Value-only state which may safely cross a gameplay transaction boundary.
// It deliberately contains no ecs::entity, component pointer, callback or
// event-vector reference.  Dynamic status/ownership/runtime data is resolved
// again from damage.target before each authored Behavior handler.
struct ObjectDeathWalkState final {
    ObjectDamageRequest damage;
    container::SharedPtr<const game::ObjectDeathReactionPlan> plan;
    ObjectHealthEvent diedEvent;
    ObjectHealthComponent::Scalar resolvedDamage{};
    ObjectHealthComponent::Scalar clippedDamage{};
    ObjectHealthComponent::Scalar previousHealth{};
    ObjectHealthComponent::Scalar currentHealth{};
    ObjectHealthComponent::Scalar maximumHealth{};
    ObjectHealthComponent::Scalar subdualDamage{};
    game::ObjectVeterancyLevel veterancy =
        game::ObjectVeterancyLevel::Regular;
    uint32_t sourcePathfindLayer = 0;
    uint32_t nextBehaviorIndex = 0;
    uint64_t sessionSeed = 0;
    std::optional<ObjectContainmentDeathFinalizeCommand>
        containmentDeathFinalize;
    ObjectDeathWalkPhase phase = ObjectDeathWalkPhase::Preamble;
    bool hasReactionComponent = false;
    bool hasAiDeathGate = false;
    bool aiDeathClaimed = false;
};

// Continuation for the part of ActiveBody::attemptDamage which follows
// Object::onDie.  Indices are stable value offsets into the session-owned
// health journal; no vector reference or ECS handle crosses the suspension.
struct ObjectBodyResumeState final {
    ObjectDamageRequest damage;
    size_t healthEventStart = 0;
    uint64_t bodyTransactionOrdinal = 0;
};

struct ObjectDamageTransactionResult final {
    std::optional<ObjectDeathWalkState> deathWalk;
    std::optional<ObjectBodyResumeState> bodyResume;
};

// Generic Session-owned ownership child. Object subsystems may name the new
// owner but never reach into ObjectTeamRegistry; the executor resolves the
// owner's current default Team at the exact authored transaction position.
struct ObjectOwnershipChangeRequest final {
    ObjectId object = INVALID_OBJECT_ID;
    PlayerId owner = INVALID_PLAYER_ID;
    uint32_t authoredOrder = 0;
    uint64_t submissionOrdinal = 0;
    uint64_t confirmedTick = 0;
    bool useOwnerDefaultTeam = true;
};

// Advances before returning the entry.  A continuation can therefore be
// stored below the current handler's children without repeating that handler
// when the LIFO executor resumes it.
[[nodiscard]] inline std::optional<game::ObjectOnDieBehaviorEntry>
takeNextObjectDeathBehavior(ObjectDeathWalkState& state) {
    if (!state.plan ||
        state.nextBehaviorIndex >= state.plan->onDieBehaviors.size()) {
        return std::nullopt;
    }
    return state.plan->onDieBehaviors[state.nextBehaviorIndex++];
}

} // namespace engine
