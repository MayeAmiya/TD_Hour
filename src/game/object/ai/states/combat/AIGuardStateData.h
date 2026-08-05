#pragma once

#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/contracts/AIStateCommands.h"
#include "game/object/ai/runtime/AIStateTypes.h"
#include "game/object/ai/states/combat/AITacticalAttackStateData.h"

namespace engine::ai
{

enum class AIGuardPhase : uint8_t
{
    Inactive,
    Return,
    Idle,
    Inner,
    Outer,
    GetCrate,
    AttackAggressor,
};

enum class AIGuardOperation : uint8_t
{
    None,
    Scan,
    Move,
    Attack,
    Enter,
    PickUpCrate,
    ExitTunnel,
    EndGuard,
};

enum class AIGuardOperationDomain : uint8_t
{
    None,
    Tactical,
    Interaction,
};

struct AIGuardPolicy final
{
    AIGuardPhase initialPhase = AIGuardPhase::Inactive;
    AIGuardPhase aggressorCompletionPhase = AIGuardPhase::Inactive;
    bool valid = false;
    bool retaliate = false;
    bool tunnelNetwork = false;
    bool returnUsesEnter = false;
    bool idleWithoutTargetCompletes = false;
    bool returnMayRetaliate = false;
    bool rejectsOrdinaryBuildings = false;
};

[[nodiscard]] constexpr AIGuardPolicy guardPolicyFor(AIStateId state) noexcept
{
    switch (state)
    {
    case AIStateId::Guard:
        return {.initialPhase = AIGuardPhase::Return,
                .aggressorCompletionPhase = AIGuardPhase::Inner,
                .valid = true,
                .retaliate = false,
                .tunnelNetwork = false,
                .returnUsesEnter = false,
                .idleWithoutTargetCompletes = false,
                .returnMayRetaliate = false,
                .rejectsOrdinaryBuildings = false};
    case AIStateId::GuardRetaliate:
        return {.initialPhase = AIGuardPhase::AttackAggressor,
                .aggressorCompletionPhase = AIGuardPhase::Return,
                .valid = true,
                .retaliate = true,
                .tunnelNetwork = false,
                .returnUsesEnter = false,
                .idleWithoutTargetCompletes = true,
                .returnMayRetaliate = false,
                .rejectsOrdinaryBuildings = true};
    case AIStateId::GuardTunnelNetwork:
        return {.initialPhase = AIGuardPhase::Return,
                .aggressorCompletionPhase = AIGuardPhase::Return,
                .valid = true,
                .retaliate = false,
                .tunnelNetwork = true,
                .returnUsesEnter = true,
                .idleWithoutTargetCompletes = false,
                .returnMayRetaliate = true,
                .rejectsOrdinaryBuildings = false};
    default:
        return {};
    }
}

struct AIGuardCorrelation final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId stateRequest{};
    AIStateId state = AIStateId::Invalid;
    AIGuardPhase phase = AIGuardPhase::Inactive;
    AIGuardOperation operation = AIGuardOperation::None;
    uint64_t sourceOrderRevision = 0;
    // Empty inside the SoA kernel. ObjectAIRuntime attaches the complete
    // admitted order identity before cross-phase staging and removes it when
    // feedback is projected back into the kernel inbox.
    AIAsyncOrderIdentity orderIdentity{};
    uint32_t operationRevision = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return subject && stateRequest.isValid() && guardPolicyFor(state).valid && phase != AIGuardPhase::Inactive &&
               operation != AIGuardOperation::None && sourceOrderRevision != 0 && operationRevision != 0;
    }
    constexpr bool operator==(const AIGuardCorrelation&) const noexcept = default;
};

enum class AIGuardTacticalCommandKind : uint8_t
{
    ScanForTarget,
    BeginMove,
    BeginAttack,
    Cancel,
};

struct AIGuardTacticalCommand final
{
    AIGuardCorrelation correlation{};
    AIGuardTacticalCommandKind kind = AIGuardTacticalCommandKind::ScanForTarget;
    ObjectId target = INVALID_OBJECT_ID;
    AIFixedPosition anchor{};
    int64_t radiusRaw = 0;
    // GuardArea scans retain the authored PolygonTrigger domain. The circle
    // remains the broad phase; GameSession applies exact legacy polygon
    // membership before selecting the closest enemy.
    AIAttackAreaHandle area{};
    uint64_t areaRevision = 0;
    bool enterGuardTargets = false;
    bool rejectOrdinaryBuildings = false;
    bool flyingOnly = false;
    bool publishTunnelNemesis = false;
    bool clearTeamTarget = false;
    uint64_t confirmedTick = 0;
    constexpr bool operator==(const AIGuardTacticalCommand&) const noexcept = default;
};

enum class AIGuardInteractionCommandKind : uint8_t
{
    BeginEnter,
    BeginPickUpCrate,
    ExitTunnel,
    Cancel,
    EndGuard,
};

struct AIGuardInteractionCommand final
{
    AIGuardCorrelation correlation{};
    AIGuardInteractionCommandKind kind = AIGuardInteractionCommandKind::BeginEnter;
    ObjectId target = INVALID_OBJECT_ID;
    AIFixedPosition targetPosition{};
    bool targetPositionValid = false;
    bool urgent = false;
    bool clearTeamTarget = false;
    uint64_t confirmedTick = 0;
    constexpr bool operator==(const AIGuardInteractionCommand&) const noexcept = default;
};

template <typename Value, size_t CapacityValue = 4>
struct AIGuardValueBuffer final
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

using AIGuardTacticalCommandBuffer = AIGuardValueBuffer<AIGuardTacticalCommand, 8>;
using AIGuardInteractionCommandBuffer = AIGuardValueBuffer<AIGuardInteractionCommand, 8>;

enum class AIGuardFeedbackKind : uint8_t
{
    None,
    Progress,
    Succeeded,
    Failed,
    Unsupported,
};

struct AIGuardFeedback final
{
    AIGuardCorrelation correlation{};
    AIGuardFeedbackKind kind = AIGuardFeedbackKind::None;
    ObjectId target = INVALID_OBJECT_ID;
    AIFixedPosition targetPosition{};
    uint64_t confirmedTick = 0;
    bool targetWithinInnerRange = false;
    constexpr bool operator==(const AIGuardFeedback&) const noexcept = default;
};

using AIGuardFeedbackBuffer = AIGuardValueBuffer<AIGuardFeedback, 8>;

struct AIGuardStatePayload final
{
    AIStateRequestId request{};
    uint64_t sourceOrderRevision = 0;
    AIStateId state = AIStateId::Invalid;
    bool active = false;
    AIGuardPhase phase = AIGuardPhase::Inactive;
    uint32_t nextOperationRevision = 1;
    AIGuardOperation taskOperation = AIGuardOperation::None;
    AIGuardOperationDomain taskDomain = AIGuardOperationDomain::None;
    uint32_t taskRevision = 0;
    bool scanPending = false;
    uint32_t scanRevision = 0;
    uint64_t nextScanTick = 0;
    uint64_t chaseDeadlineTick = 0;
    ObjectId nemesis = INVALID_OBJECT_ID;
    AIFixedPosition anchor{};

    constexpr bool operator==(const AIGuardStatePayload&) const noexcept = default;
};

// This slice deliberately owns field vectors rather than an AoS payload. It
// can be moved into AIStateFamilySoAStorage later without changing the command
// protocol or the kernels below.
struct AIGuardSoAColumns final
{
    container::Vector<uint64_t> requestIssuedTick;
    container::Vector<uint32_t> requestSequence;
    container::Vector<uint64_t> sourceOrderRevision;
    container::Vector<AIStateId> state;
    container::Vector<uint8_t> active;
    container::Vector<uint8_t> phase;
    container::Vector<uint32_t> nextOperationRevision;
    container::Vector<uint8_t> taskOperation;
    container::Vector<uint8_t> taskDomain;
    container::Vector<uint32_t> taskRevision;
    container::Vector<uint8_t> scanPending;
    container::Vector<uint32_t> scanRevision;
    container::Vector<uint64_t> nextScanTick;
    container::Vector<uint64_t> chaseDeadlineTick;
    container::Vector<ObjectId> nemesis;
    container::Vector<int64_t> anchorX;
    container::Vector<int64_t> anchorY;
    container::Vector<int64_t> anchorZ;

    void reset(size_t count)
    {
        requestIssuedTick.assign(count, 0);
        requestSequence.assign(count, 0);
        sourceOrderRevision.assign(count, 0);
        state.assign(count, AIStateId::Invalid);
        active.assign(count, 0);
        phase.assign(count, static_cast<uint8_t>(AIGuardPhase::Inactive));
        nextOperationRevision.assign(count, 1);
        taskOperation.assign(count, static_cast<uint8_t>(AIGuardOperation::None));
        taskDomain.assign(count, static_cast<uint8_t>(AIGuardOperationDomain::None));
        taskRevision.assign(count, 0);
        scanPending.assign(count, 0);
        scanRevision.assign(count, 0);
        nextScanTick.assign(count, 0);
        chaseDeadlineTick.assign(count, 0);
        nemesis.assign(count, INVALID_OBJECT_ID);
        anchorX.assign(count, 0);
        anchorY.assign(count, 0);
        anchorZ.assign(count, 0);
    }

    void activate(size_t slot, AIStateRequestId request) noexcept
    {
        AIGuardStatePayload payload;
        payload.request = request;
        store(slot, payload);
    }

    [[nodiscard]] AIGuardStatePayload load(size_t slot) const noexcept
    {
        AIGuardStatePayload payload;
        payload.request = requestAt(slot);
        payload.sourceOrderRevision = sourceOrderRevision[slot];
        payload.state = state[slot];
        payload.active = active[slot] != 0;
        payload.phase = phaseAt(slot);
        payload.nextOperationRevision = nextOperationRevision[slot];
        payload.taskOperation = taskAt(slot);
        payload.taskDomain = taskDomainAt(slot);
        payload.taskRevision = taskRevision[slot];
        payload.scanPending = scanPending[slot] != 0;
        payload.scanRevision = scanRevision[slot];
        payload.nextScanTick = nextScanTick[slot];
        payload.chaseDeadlineTick = chaseDeadlineTick[slot];
        payload.nemesis = nemesis[slot];
        payload.anchor = anchorAt(slot);
        return payload;
    }

    void store(size_t slot, const AIGuardStatePayload& payload) noexcept
    {
        requestIssuedTick[slot] = payload.request.issuedTick;
        requestSequence[slot] = payload.request.sequence;
        sourceOrderRevision[slot] = payload.sourceOrderRevision;
        state[slot] = payload.state;
        active[slot] = payload.active ? uint8_t{1} : uint8_t{0};
        phase[slot] = static_cast<uint8_t>(payload.phase);
        nextOperationRevision[slot] = payload.nextOperationRevision;
        taskOperation[slot] = static_cast<uint8_t>(payload.taskOperation);
        taskDomain[slot] = static_cast<uint8_t>(payload.taskDomain);
        taskRevision[slot] = payload.taskRevision;
        scanPending[slot] = payload.scanPending ? uint8_t{1} : uint8_t{0};
        scanRevision[slot] = payload.scanRevision;
        nextScanTick[slot] = payload.nextScanTick;
        chaseDeadlineTick[slot] = payload.chaseDeadlineTick;
        nemesis[slot] = payload.nemesis;
        anchorX[slot] = payload.anchor.xRaw;
        anchorY[slot] = payload.anchor.yRaw;
        anchorZ[slot] = payload.anchor.zRaw;
    }

    [[nodiscard]] size_t size() const noexcept
    {
        return active.size();
    }
    [[nodiscard]] bool isAligned() const noexcept
    {
        const size_t count = size();
        return requestIssuedTick.size() == count && requestSequence.size() == count &&
               sourceOrderRevision.size() == count && state.size() == count && phase.size() == count &&
               nextOperationRevision.size() == count && taskOperation.size() == count && taskDomain.size() == count &&
               taskRevision.size() == count && scanPending.size() == count && scanRevision.size() == count &&
               nextScanTick.size() == count && chaseDeadlineTick.size() == count && nemesis.size() == count &&
               anchorX.size() == count && anchorY.size() == count && anchorZ.size() == count;
    }
    [[nodiscard]] AIGuardPhase phaseAt(size_t slot) const noexcept
    {
        return static_cast<AIGuardPhase>(phase[slot]);
    }
    [[nodiscard]] AIGuardOperation taskAt(size_t slot) const noexcept
    {
        return static_cast<AIGuardOperation>(taskOperation[slot]);
    }
    [[nodiscard]] AIGuardOperationDomain taskDomainAt(size_t slot) const noexcept
    {
        return static_cast<AIGuardOperationDomain>(taskDomain[slot]);
    }
    [[nodiscard]] AIStateRequestId requestAt(size_t slot) const noexcept
    {
        return {requestIssuedTick[slot], requestSequence[slot]};
    }
    [[nodiscard]] AIFixedPosition anchorAt(size_t slot) const noexcept
    {
        return {anchorX[slot], anchorY[slot], anchorZ[slot]};
    }
};

} // namespace engine::ai
