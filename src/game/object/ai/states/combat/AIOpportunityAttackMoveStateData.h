#pragma once

#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateStep.h"
#include "game/object/ai/contracts/AIOrderIdentity.h"

namespace engine::ai
{

inline constexpr uint8_t AI_OPPORTUNITY_ATTACK_MOVE_RETRIES = 5;
inline constexpr uint32_t AI_OPPORTUNITY_ATTACK_MOVE_RETRY_DELAY_SECONDS = 3;
inline constexpr uint32_t AI_OPPORTUNITY_ATTACK_MOVE_CLOSE_ENOUGH_CELLS = 8;

enum class AIOpportunityAttackMovePhase : uint8_t
{
    Inactive,
    Scanning,
    Moving,
    Engaging,
    Resuming,
};

enum class AIOpportunityAttackMoveMovement : uint8_t
{
    None,
    MoveTo,
    Waypoint,
};

struct AIOpportunityAttackMovePolicy final
{
    AIOpportunityAttackMoveMovement movement = AIOpportunityAttackMoveMovement::None;
    bool moveAsTeam = false;
    bool valid = false;
};

[[nodiscard]] constexpr AIOpportunityAttackMovePolicy opportunityAttackMovePolicyFor(AIStateId state) noexcept
{
    switch (state)
    {
    case AIStateId::AttackMoveTo:
        return {.movement = AIOpportunityAttackMoveMovement::MoveTo, .moveAsTeam = false, .valid = true};
    case AIStateId::AttackFollowWaypointPathAsIndividuals:
        return {.movement = AIOpportunityAttackMoveMovement::Waypoint, .moveAsTeam = false, .valid = true};
    case AIStateId::AttackFollowWaypointPathAsTeam:
        return {.movement = AIOpportunityAttackMoveMovement::Waypoint, .moveAsTeam = true, .valid = true};
    default:
        return {};
    }
}

enum class AIOpportunityAttackMoveOperation : uint8_t
{
    None,
    FindCrate,
    FindMoodTarget,
    Attack,
    PickUpCrate,
};

struct AIOpportunityAttackMoveCorrelation final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId stateRequest{};
    AIStateId state = AIStateId::Invalid;
    AIOpportunityAttackMovePhase phase = AIOpportunityAttackMovePhase::Inactive;
    AIOpportunityAttackMoveOperation operation = AIOpportunityAttackMoveOperation::None;
    uint64_t sourceOrderRevision = 0;
    // The SoA kernel emits an empty identity. The session runtime fills the
    // complete admitted-order identity before staging the value and removes
    // it again when projecting correlated feedback back into the kernel.
    AIAsyncOrderIdentity orderIdentity{};
    uint32_t phaseRevision = 0;
    uint32_t operationRevision = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return subject && stateRequest.isValid() && opportunityAttackMovePolicyFor(state).valid &&
               phase != AIOpportunityAttackMovePhase::Inactive && operation != AIOpportunityAttackMoveOperation::None &&
               sourceOrderRevision != 0 && phaseRevision != 0 && operationRevision != 0;
    }
    constexpr bool operator==(const AIOpportunityAttackMoveCorrelation&) const noexcept = default;
};

enum class AIOpportunityAttackMoveQueryCommandKind : uint8_t
{
    FindCrate,
    FindMoodTarget,
    Cancel,
};

struct AIOpportunityAttackMoveQueryCommand final
{
    AIOpportunityAttackMoveCorrelation correlation{};
    AIOpportunityAttackMoveQueryCommandKind kind = AIOpportunityAttackMoveQueryCommandKind::FindCrate;
    uint64_t confirmedTick = 0;
    constexpr bool operator==(const AIOpportunityAttackMoveQueryCommand&) const noexcept = default;
};

enum class AIOpportunityAttackMoveChildCommandKind : uint8_t
{
    BeginAttack,
    BeginPickUpCrate,
    Cancel,
};

struct AIOpportunityAttackMoveChildCommand final
{
    AIOpportunityAttackMoveCorrelation correlation{};
    AIOpportunityAttackMoveChildCommandKind kind = AIOpportunityAttackMoveChildCommandKind::BeginAttack;
    ObjectId target = INVALID_OBJECT_ID;
    AIFixedPosition targetPosition{};
    bool targetPositionValid = false;
    bool commandSourceIsAI = false;
    uint64_t confirmedTick = 0;
    constexpr bool operator==(const AIOpportunityAttackMoveChildCommand&) const noexcept = default;
};

template <typename Value, size_t CapacityValue = 8>
struct AIOpportunityAttackMoveValueBuffer final
{
    static_assert(CapacityValue > 0);

    container::Array<Value, CapacityValue> values{};
    size_t count = 0;
    bool overflowed = false;

    [[nodiscard]] constexpr bool hasCapacity(size_t additional = 1) const noexcept
    {
        return count <= values.size() && additional <= values.size() - count;
    }

    [[nodiscard]] bool push(const Value& value) noexcept
    {
        if (!hasCapacity())
        {
            overflowed = true;
            return false;
        }
        values[count++] = value;
        return true;
    }

    void clear() noexcept
    {
        count = 0;
        overflowed = false;
    }
};

using AIOpportunityAttackMoveQueryCommandBuffer =
    AIOpportunityAttackMoveValueBuffer<AIOpportunityAttackMoveQueryCommand>;
using AIOpportunityAttackMoveChildCommandBuffer =
    AIOpportunityAttackMoveValueBuffer<AIOpportunityAttackMoveChildCommand>;

enum class AIOpportunityAttackMoveQueryFeedbackKind : uint8_t
{
    None,
    Target,
    NoTarget,
    Unsupported,
};

struct AIOpportunityAttackMoveQueryFeedback final
{
    AIOpportunityAttackMoveCorrelation correlation{};
    AIOpportunityAttackMoveQueryFeedbackKind kind = AIOpportunityAttackMoveQueryFeedbackKind::None;
    ObjectId target = INVALID_OBJECT_ID;
    AIFixedPosition targetPosition{};
    bool targetPositionValid = false;
    // RefCode's getNextMoodTarget(false, ...) still returns an authored
    // Team::getTeamTargetObject().  The AttackMove wrapper must therefore not
    // apply its ordinary force-retarget exclusion to this value.
    bool commonTeamTarget = false;
    uint64_t confirmedTick = 0;
    constexpr bool operator==(const AIOpportunityAttackMoveQueryFeedback&) const noexcept = default;
};

enum class AIOpportunityAttackMoveChildFeedbackKind : uint8_t
{
    None,
    Progress,
    Succeeded,
    Failed,
    Unsupported,
};

struct AIOpportunityAttackMoveChildFeedback final
{
    AIOpportunityAttackMoveCorrelation correlation{};
    AIOpportunityAttackMoveChildFeedbackKind kind = AIOpportunityAttackMoveChildFeedbackKind::None;
    uint64_t confirmedTick = 0;
    constexpr bool operator==(const AIOpportunityAttackMoveChildFeedback&) const noexcept = default;
};

using AIOpportunityAttackMoveQueryFeedbackBuffer =
    AIOpportunityAttackMoveValueBuffer<AIOpportunityAttackMoveQueryFeedback>;
using AIOpportunityAttackMoveChildFeedbackBuffer =
    AIOpportunityAttackMoveValueBuffer<AIOpportunityAttackMoveChildFeedback>;

// Storage-owned orchestration state. The existing movement payload remains a
// temporary adapter concern and is intentionally absent from this value.
struct AIOpportunityAttackMoveStatePayload final
{
    AIStateRequestId request{};
    uint64_t sourceOrderRevision = 0;
    AIStateId state = AIStateId::Invalid;
    AIOpportunityAttackMovePhase phase = AIOpportunityAttackMovePhase::Inactive;
    uint32_t phaseRevision = 0;
    uint32_t nextOperationRevision = 1;
    AIOpportunityAttackMoveOperation scanOperation = AIOpportunityAttackMoveOperation::FindCrate;
    uint32_t queryRevision = 0;
    AIOpportunityAttackMoveOperation childOperation = AIOpportunityAttackMoveOperation::None;
    uint32_t childRevision = 0;
    ObjectId childTarget = INVALID_OBJECT_ID;
    uint8_t retriesRemaining = 0;
    uint64_t retryWakeTick = 0;
    // 0 = none, 1 = success, 2 = failure.
    uint8_t movementTerminal = 0;
    bool active = false;
    bool queryPending = false;
    bool childActive = false;
    bool movementPaused = false;
    bool resumeRequired = false;
    bool resumeScanComplete = false;
    bool forceRetarget = false;

    constexpr AIOpportunityAttackMoveStatePayload() = default;
    explicit constexpr AIOpportunityAttackMoveStatePayload(AIStateRequestId value)
        : request(value)
    {
    }
    constexpr bool operator==(const AIOpportunityAttackMoveStatePayload&) const noexcept = default;
};

struct AIOpportunityAttackMoveSoAColumns final
{
    container::Vector<uint64_t> requestIssuedTick;
    container::Vector<uint32_t> requestSequence;
    container::Vector<uint64_t> sourceOrderRevision;
    container::Vector<AIStateId> state;
    container::Vector<uint8_t> active;
    container::Vector<uint8_t> phase;
    container::Vector<uint32_t> phaseRevision;
    container::Vector<uint32_t> nextOperationRevision;
    container::Vector<uint8_t> scanOperation;
    container::Vector<uint8_t> queryPending;
    container::Vector<uint32_t> queryRevision;
    container::Vector<uint8_t> childOperation;
    container::Vector<uint8_t> childActive;
    container::Vector<uint32_t> childRevision;
    container::Vector<ObjectId> childTarget;
    container::Vector<uint8_t> movementPaused;
    container::Vector<uint8_t> resumeRequired;
    container::Vector<uint8_t> resumeScanComplete;
    container::Vector<uint8_t> forceRetarget;
    container::Vector<uint8_t> retriesRemaining;
    container::Vector<uint64_t> retryWakeTick;
    container::Vector<uint8_t> movementTerminal;

    void reset(size_t count)
    {
        requestIssuedTick.assign(count, 0);
        requestSequence.assign(count, 0);
        sourceOrderRevision.assign(count, 0);
        state.assign(count, AIStateId::Invalid);
        active.assign(count, 0);
        phase.assign(count, static_cast<uint8_t>(AIOpportunityAttackMovePhase::Inactive));
        phaseRevision.assign(count, 0);
        nextOperationRevision.assign(count, 1);
        scanOperation.assign(count, static_cast<uint8_t>(AIOpportunityAttackMoveOperation::FindCrate));
        queryPending.assign(count, 0);
        queryRevision.assign(count, 0);
        childOperation.assign(count, static_cast<uint8_t>(AIOpportunityAttackMoveOperation::None));
        childActive.assign(count, 0);
        childRevision.assign(count, 0);
        childTarget.assign(count, INVALID_OBJECT_ID);
        movementPaused.assign(count, 0);
        resumeRequired.assign(count, 0);
        resumeScanComplete.assign(count, 0);
        forceRetarget.assign(count, 0);
        retriesRemaining.assign(count, 0);
        retryWakeTick.assign(count, 0);
        movementTerminal.assign(count, 0);
    }

    [[nodiscard]] size_t size() const noexcept
    {
        return active.size();
    }

    void activate(size_t slot, AIStateRequestId request) noexcept
    {
        store(slot, AIOpportunityAttackMoveStatePayload{request});
    }

    [[nodiscard]] AIStateRequestId requestAt(size_t slot) const noexcept
    {
        return {requestIssuedTick[slot], requestSequence[slot]};
    }

    [[nodiscard]] AIOpportunityAttackMovePhase phaseAt(size_t slot) const noexcept
    {
        return static_cast<AIOpportunityAttackMovePhase>(phase[slot]);
    }

    [[nodiscard]] AIOpportunityAttackMoveStatePayload load(size_t slot) const noexcept
    {
        AIOpportunityAttackMoveStatePayload value;
        value.request = requestAt(slot);
        value.sourceOrderRevision = sourceOrderRevision[slot];
        value.state = state[slot];
        value.phase = phaseAt(slot);
        value.phaseRevision = phaseRevision[slot];
        value.nextOperationRevision = nextOperationRevision[slot];
        value.scanOperation = static_cast<AIOpportunityAttackMoveOperation>(scanOperation[slot]);
        value.queryRevision = queryRevision[slot];
        value.childOperation = static_cast<AIOpportunityAttackMoveOperation>(childOperation[slot]);
        value.childRevision = childRevision[slot];
        value.childTarget = childTarget[slot];
        value.retriesRemaining = retriesRemaining[slot];
        value.retryWakeTick = retryWakeTick[slot];
        value.movementTerminal = movementTerminal[slot];
        value.active = active[slot] != 0;
        value.queryPending = queryPending[slot] != 0;
        value.childActive = childActive[slot] != 0;
        value.movementPaused = movementPaused[slot] != 0;
        value.resumeRequired = resumeRequired[slot] != 0;
        value.resumeScanComplete = resumeScanComplete[slot] != 0;
        value.forceRetarget = forceRetarget[slot] != 0;
        return value;
    }

    void store(size_t slot, const AIOpportunityAttackMoveStatePayload& value) noexcept
    {
        requestIssuedTick[slot] = value.request.issuedTick;
        requestSequence[slot] = value.request.sequence;
        sourceOrderRevision[slot] = value.sourceOrderRevision;
        state[slot] = value.state;
        active[slot] = value.active ? uint8_t{1} : uint8_t{0};
        phase[slot] = static_cast<uint8_t>(value.phase);
        phaseRevision[slot] = value.phaseRevision;
        nextOperationRevision[slot] = value.nextOperationRevision;
        scanOperation[slot] = static_cast<uint8_t>(value.scanOperation);
        queryPending[slot] = value.queryPending ? uint8_t{1} : uint8_t{0};
        queryRevision[slot] = value.queryRevision;
        childOperation[slot] = static_cast<uint8_t>(value.childOperation);
        childActive[slot] = value.childActive ? uint8_t{1} : uint8_t{0};
        childRevision[slot] = value.childRevision;
        childTarget[slot] = value.childTarget;
        movementPaused[slot] = value.movementPaused ? uint8_t{1} : uint8_t{0};
        resumeRequired[slot] = value.resumeRequired ? uint8_t{1} : uint8_t{0};
        resumeScanComplete[slot] = value.resumeScanComplete ? uint8_t{1} : uint8_t{0};
        forceRetarget[slot] = value.forceRetarget ? uint8_t{1} : uint8_t{0};
        retriesRemaining[slot] = value.retriesRemaining;
        retryWakeTick[slot] = value.retryWakeTick;
        movementTerminal[slot] = value.movementTerminal;
    }
};

} // namespace engine::ai
