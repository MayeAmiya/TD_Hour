#pragma once

#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/contracts/AIAttackServices.h"

namespace engine::ai
{

struct AITargetCollectionHandle final
{
    uint64_t value = 0;
    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return value != 0;
    }
    explicit constexpr operator bool() const noexcept
    {
        return isValid();
    }
    constexpr bool operator==(const AITargetCollectionHandle&) const noexcept = default;
};

struct AIAttackAreaHandle final
{
    uint64_t value = 0;
    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return value != 0;
    }
    explicit constexpr operator bool() const noexcept
    {
        return isValid();
    }
    constexpr bool operator==(const AIAttackAreaHandle&) const noexcept = default;
};

enum class AITacticalAttackWrapperPhase : uint8_t
{
    Inactive,
    Idle,
    QueryingCrate,
    QueryingTarget,
    RunningAttack,
    RunningPickUpCrate,
};

enum class AITacticalAttackQueryKind : uint8_t
{
    None,
    Crate,
    HuntTarget,
    SquadTarget,
    AreaTarget,
};

enum class AISquadTargetSelection : uint8_t
{
    RandomLiveMember,
    ClosestLiveMember,
    FirstLiveMember,
    LastDamageSource,
    NoTarget,
};

struct AITacticalAttackPolicy final
{
    bool scansOnTimer = false;
    bool checksCrates = false;
    bool noTargetCompletes = false;
    bool usesCollection = false;
    bool usesArea = false;
    bool valid = false;
};

[[nodiscard]] constexpr AITacticalAttackPolicy tacticalAttackPolicyFor(AIStateId state) noexcept
{
    switch (state)
    {
    case AIStateId::Hunt:
        return {.scansOnTimer = true,
                .checksCrates = true,
                .noTargetCompletes = true,
                .usesCollection = false,
                .usesArea = false,
                .valid = true};
    case AIStateId::AttackSquad:
        return {.scansOnTimer = false,
                .checksCrates = true,
                .noTargetCompletes = true,
                .usesCollection = true,
                .usesArea = false,
                .valid = true};
    case AIStateId::AttackArea:
        return {.scansOnTimer = true,
                .checksCrates = false,
                .noTargetCompletes = true,
                .usesCollection = false,
                .usesArea = true,
                .valid = true};
    default:
        return {};
    }
}

struct AITacticalAttackQueryCorrelation final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId stateRequest{};
    AIStateId wrapperState = AIStateId::Invalid;
    uint64_t sourceOrderRevision = 0;
    // Empty inside the SoA kernel. ObjectAIRuntime fills the complete
    // queue-backed Hunt identity before staging and clears it again when
    // scattering feedback into the kernel inbox.
    AIAsyncOrderIdentity orderIdentity{};
    uint32_t generation = 0;
    AITacticalAttackQueryKind query = AITacticalAttackQueryKind::None;
    AITargetCollectionHandle collection{};
    uint64_t collectionRevision = 0;
    AIAttackAreaHandle area{};
    uint64_t areaRevision = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        if (!subject || !stateRequest.isValid() || !tacticalAttackPolicyFor(wrapperState).valid ||
            sourceOrderRevision == 0 || generation == 0 || query == AITacticalAttackQueryKind::None)
        {
            return false;
        }
        if (query == AITacticalAttackQueryKind::SquadTarget)
            return collection && collectionRevision != 0;
        if (query == AITacticalAttackQueryKind::AreaTarget)
            return area && areaRevision != 0;
        return true;
    }
    constexpr bool operator==(const AITacticalAttackQueryCorrelation&) const noexcept = default;
};

enum class AITacticalAttackQueryCommandKind : uint8_t
{
    Begin,
    Cancel,
};

struct AITacticalAttackQueryCommand final
{
    AITacticalAttackQueryCorrelation correlation{};
    AITacticalAttackQueryCommandKind kind = AITacticalAttackQueryCommandKind::Begin;
    AISquadTargetSelection squadSelection = AISquadTargetSelection::ClosestLiveMember;
    bool canAttackOnly = true;
    bool useAttackPriority = true;
    bool fallbackWithoutAttackPriority = false;
    bool useTeamCommonTarget = false;
    uint64_t confirmedTick = 0;
};

enum class AITacticalAttackQueryStatus : uint8_t
{
    None,
    Completed,
    Unsupported,
};

struct AITacticalAttackQueryFeedback final
{
    AITacticalAttackQueryCorrelation correlation{};
    AITacticalAttackQueryStatus status = AITacticalAttackQueryStatus::None;
    ObjectId target = INVALID_OBJECT_ID;
    AIFixedPosition targetPosition{};
    bool targetPositionValid = false;
    uint64_t targetRevision = 0;
    uint64_t confirmedTick = 0;
};

using AITacticalAttackQueryCommandBuffer = AIAttackValueBuffer<AITacticalAttackQueryCommand, 8>;
using AITacticalAttackQueryFeedbackBuffer = AIAttackValueBuffer<AITacticalAttackQueryFeedback, 8>;

struct AITacticalAttackChildCorrelation final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId stateRequest{};
    AIStateId wrapperState = AIStateId::Invalid;
    uint64_t sourceOrderRevision = 0;
    AIAsyncOrderIdentity orderIdentity{};
    AIStateId childState = AIStateId::Invalid;
    uint32_t generation = 0;
    ObjectId target = INVALID_OBJECT_ID;
    uint64_t targetRevision = 0;

    [[nodiscard]] constexpr bool isWrapperValid() const noexcept
    {
        return subject && stateRequest.isValid() && tacticalAttackPolicyFor(wrapperState).valid &&
               sourceOrderRevision != 0;
    }
    [[nodiscard]] constexpr bool isChildValid() const noexcept
    {
        return isWrapperValid() && (childState == AIStateId::AttackObject || childState == AIStateId::PickUpCrate) &&
               generation != 0 && target && targetRevision != 0;
    }
    constexpr bool operator==(const AITacticalAttackChildCorrelation&) const noexcept = default;
};

enum class AITacticalAttackChildCommandKind : uint8_t
{
    StartOrReplace,
    EndWrapper,
};

struct AITacticalAttackChildCommand final
{
    AITacticalAttackChildCorrelation correlation{};
    AITacticalAttackChildCommandKind kind = AITacticalAttackChildCommandKind::StartOrReplace;
    AIFixedPosition targetPosition{};
    bool targetPositionValid = false;
    bool releaseTemporaryWeaponLock = false;
    uint64_t confirmedTick = 0;
};

enum class AITacticalAttackChildStatus : uint8_t
{
    None,
    Running,
    Succeeded,
    Failed,
    Unsupported,
};

struct AITacticalAttackChildFeedback final
{
    AITacticalAttackChildCorrelation correlation{};
    AITacticalAttackChildStatus status = AITacticalAttackChildStatus::None;
    uint64_t confirmedTick = 0;
};

using AITacticalAttackChildCommandBuffer = AIAttackValueBuffer<AITacticalAttackChildCommand, 8>;
using AITacticalAttackChildFeedbackBuffer = AIAttackValueBuffer<AITacticalAttackChildFeedback, 8>;

struct AITacticalAttackStatePayload final
{
    bool active = false;
    AIStateRequestId request{};
    uint64_t sourceOrderRevision = 0;
    AIStateId state = AIStateId::Invalid;
    AITacticalAttackWrapperPhase wrapperPhase = AITacticalAttackWrapperPhase::Inactive;
    uint64_t nextScanTick = 0;
    ObjectId target = INVALID_OBJECT_ID;
    uint64_t targetRevision = 0;
    AITargetCollectionHandle collectionHandle{};
    uint64_t collectionRevision = 0;
    AIAttackAreaHandle areaHandle{};
    uint64_t areaRevision = 0;
    uint32_t nextQueryGeneration = 1;
    AITacticalAttackQueryKind pendingQuery = AITacticalAttackQueryKind::None;
    uint32_t queryGeneration = 0;
    uint32_t nextChildGeneration = 1;
    AIStateId childState = AIStateId::Idle;
    uint32_t childGeneration = 0;

    constexpr AITacticalAttackStatePayload() = default;
    explicit constexpr AITacticalAttackStatePayload(AIStateRequestId value)
        : request(value)
    {
    }
    constexpr bool operator==(const AITacticalAttackStatePayload&) const noexcept = default;
};

struct AITacticalAttackSoAColumns final
{
    container::Vector<uint8_t> active;
    container::Vector<uint64_t> requestIssuedTick;
    container::Vector<uint32_t> requestSequence;
    container::Vector<uint64_t> sourceOrderRevision;
    container::Vector<AIStateId> state;
    container::Vector<uint8_t> wrapperPhase;
    container::Vector<uint64_t> nextScanTick;
    container::Vector<ObjectId> target;
    container::Vector<uint64_t> targetRevision;
    container::Vector<AITargetCollectionHandle> collectionHandle;
    container::Vector<uint64_t> collectionRevision;
    container::Vector<AIAttackAreaHandle> areaHandle;
    container::Vector<uint64_t> areaRevision;
    container::Vector<uint32_t> nextQueryGeneration;
    container::Vector<uint8_t> pendingQuery;
    container::Vector<uint32_t> queryGeneration;
    container::Vector<uint32_t> nextChildGeneration;
    container::Vector<AIStateId> childState;
    container::Vector<uint32_t> childGeneration;

    explicit AITacticalAttackSoAColumns(size_t count = 0)
    {
        resize(count);
    }

    void resize(size_t count)
    {
        active.assign(count, 0);
        requestIssuedTick.assign(count, 0);
        requestSequence.assign(count, 0);
        sourceOrderRevision.assign(count, 0);
        state.assign(count, AIStateId::Invalid);
        wrapperPhase.assign(count, static_cast<uint8_t>(AITacticalAttackWrapperPhase::Inactive));
        nextScanTick.assign(count, 0);
        target.assign(count, INVALID_OBJECT_ID);
        targetRevision.assign(count, 0);
        collectionHandle.assign(count, {});
        collectionRevision.assign(count, 0);
        areaHandle.assign(count, {});
        areaRevision.assign(count, 0);
        nextQueryGeneration.assign(count, 1);
        pendingQuery.assign(count, static_cast<uint8_t>(AITacticalAttackQueryKind::None));
        queryGeneration.assign(count, 0);
        nextChildGeneration.assign(count, 1);
        childState.assign(count, AIStateId::Idle);
        childGeneration.assign(count, 0);
    }

    [[nodiscard]] size_t size() const noexcept
    {
        return active.size();
    }
    [[nodiscard]] bool isAligned() const noexcept
    {
        const size_t count = size();
        return requestIssuedTick.size() == count && requestSequence.size() == count &&
               sourceOrderRevision.size() == count && state.size() == count && wrapperPhase.size() == count &&
               nextScanTick.size() == count && target.size() == count && targetRevision.size() == count &&
               collectionHandle.size() == count && collectionRevision.size() == count && areaHandle.size() == count &&
               areaRevision.size() == count && nextQueryGeneration.size() == count && pendingQuery.size() == count &&
               queryGeneration.size() == count && nextChildGeneration.size() == count && childState.size() == count &&
               childGeneration.size() == count;
    }
    void activate(size_t slot, AIStateRequestId request) noexcept
    {
        store(slot, AITacticalAttackStatePayload{request});
    }
    [[nodiscard]] AITacticalAttackStatePayload load(size_t slot) const noexcept
    {
        AITacticalAttackStatePayload value;
        value.active = active[slot] != 0;
        value.request = requestAt(slot);
        value.sourceOrderRevision = sourceOrderRevision[slot];
        value.state = state[slot];
        value.wrapperPhase = phaseAt(slot);
        value.nextScanTick = nextScanTick[slot];
        value.target = target[slot];
        value.targetRevision = targetRevision[slot];
        value.collectionHandle = collectionHandle[slot];
        value.collectionRevision = collectionRevision[slot];
        value.areaHandle = areaHandle[slot];
        value.areaRevision = areaRevision[slot];
        value.nextQueryGeneration = nextQueryGeneration[slot];
        value.pendingQuery = queryAt(slot);
        value.queryGeneration = queryGeneration[slot];
        value.nextChildGeneration = nextChildGeneration[slot];
        value.childState = childState[slot];
        value.childGeneration = childGeneration[slot];
        return value;
    }
    void store(size_t slot, const AITacticalAttackStatePayload& value) noexcept
    {
        active[slot] = value.active ? uint8_t{1} : uint8_t{0};
        requestIssuedTick[slot] = value.request.issuedTick;
        requestSequence[slot] = value.request.sequence;
        sourceOrderRevision[slot] = value.sourceOrderRevision;
        state[slot] = value.state;
        wrapperPhase[slot] = static_cast<uint8_t>(value.wrapperPhase);
        nextScanTick[slot] = value.nextScanTick;
        target[slot] = value.target;
        targetRevision[slot] = value.targetRevision;
        collectionHandle[slot] = value.collectionHandle;
        collectionRevision[slot] = value.collectionRevision;
        areaHandle[slot] = value.areaHandle;
        areaRevision[slot] = value.areaRevision;
        nextQueryGeneration[slot] = value.nextQueryGeneration;
        pendingQuery[slot] = static_cast<uint8_t>(value.pendingQuery);
        queryGeneration[slot] = value.queryGeneration;
        nextChildGeneration[slot] = value.nextChildGeneration;
        childState[slot] = value.childState;
        childGeneration[slot] = value.childGeneration;
    }
    [[nodiscard]] AIStateRequestId requestAt(size_t slot) const noexcept
    {
        return {.issuedTick = requestIssuedTick[slot], .sequence = requestSequence[slot]};
    }
    [[nodiscard]] AITacticalAttackWrapperPhase phaseAt(size_t slot) const noexcept
    {
        return static_cast<AITacticalAttackWrapperPhase>(wrapperPhase[slot]);
    }
    [[nodiscard]] AITacticalAttackQueryKind queryAt(size_t slot) const noexcept
    {
        return static_cast<AITacticalAttackQueryKind>(pendingQuery[slot]);
    }
};

using AITacticalAttackStateSoAColumns = AITacticalAttackSoAColumns;

} // namespace engine::ai
