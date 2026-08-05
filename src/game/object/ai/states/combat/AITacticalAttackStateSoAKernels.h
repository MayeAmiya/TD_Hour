#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateStep.h"
#include "game/object/ai/states/combat/AITacticalAttackStateData.h"

namespace engine::ai
{

struct AITacticalAttackStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    uint32_t enemyScanIntervalTicks = 30;
    container::Span<const uint8_t> scheduled{};
    AIExecutionSlotRange executionSlots{};
    container::Span<const ObjectId> subjects;
    container::Span<const uint64_t> sourceOrderRevisions;
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> allWeaponsOutOfAmmo;
    container::Span<const uint8_t> projectile;
    container::Span<const uint8_t> allArmyHunt;
    container::Span<const uint8_t> useTeamCommonTarget;
    container::Span<const uint32_t> initialScanJitter;
    container::Span<const AITargetCollectionHandle> targetCollections;
    container::Span<const uint64_t> targetCollectionRevisions;
    container::Span<const AIAttackAreaHandle> attackAreas;
    container::Span<const uint64_t> attackAreaRevisions;
    container::Span<const AISquadTargetSelection> squadSelections;
    container::Span<const AITacticalAttackQueryFeedbackBuffer> queryFeedback;
    container::Span<const AITacticalAttackChildFeedbackBuffer> childFeedback;
    container::Span<AITacticalAttackQueryCommandBuffer> queryCommands;
    container::Span<AITacticalAttackChildCommandBuffer> childCommands;
    container::Span<AIStateStepResult> results;
};

namespace tactical_attack_detail
{

[[nodiscard]] constexpr bool fact(uint8_t value) noexcept
{
    return value != 0;
}

[[nodiscard]] constexpr uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept
{
    return right > std::numeric_limits<uint64_t>::max() - left ? std::numeric_limits<uint64_t>::max() : left + right;
}

[[nodiscard]] constexpr uint64_t boundedJitter(uint32_t draw, uint32_t interval) noexcept
{
    return interval == std::numeric_limits<uint32_t>::max() ? draw : draw % (static_cast<uint64_t>(interval) + 1u);
}

[[nodiscard]] inline bool scheduled(const AITacticalAttackStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || fact(input.scheduled[slot]);
}

[[nodiscard]] inline bool hasAlignedSpans(const AITacticalAttackSoAColumns& columns,
                                          const AITacticalAttackStateSoAKernelInput& input) noexcept
{
    const size_t count = columns.size();
    return columns.isAligned() && (input.scheduled.empty() || input.scheduled.size() == count) &&
           input.subjects.size() == count && input.sourceOrderRevisions.size() == count &&
           input.effectivelyDead.size() == count && input.allWeaponsOutOfAmmo.size() == count &&
           input.projectile.size() == count && input.allArmyHunt.size() == count &&
           input.useTeamCommonTarget.size() == count && input.initialScanJitter.size() == count &&
           input.targetCollections.size() == count && input.targetCollectionRevisions.size() == count &&
           input.attackAreas.size() == count && input.attackAreaRevisions.size() == count &&
           input.squadSelections.size() == count && input.queryFeedback.size() == count &&
           input.childFeedback.size() == count && input.queryCommands.size() == count &&
           input.childCommands.size() == count && input.results.size() == count;
}

[[nodiscard]] inline uint32_t allocateGeneration(container::Vector<uint32_t>& next, size_t slot) noexcept
{
    uint32_t generation = next[slot];
    if (generation == 0)
        generation = 1;
    next[slot] = generation + 1;
    if (next[slot] == 0)
        next[slot] = 1;
    return generation;
}

inline void refreshHandles(AITacticalAttackSoAColumns& columns,
                           const AITacticalAttackStateSoAKernelInput& input,
                           size_t slot) noexcept
{
    columns.collectionHandle[slot] = input.targetCollections[slot];
    columns.collectionRevision[slot] = input.targetCollectionRevisions[slot];
    columns.areaHandle[slot] = input.attackAreas[slot];
    columns.areaRevision[slot] = input.attackAreaRevisions[slot];
}

[[nodiscard]] inline AITacticalAttackQueryCorrelation queryCorrelation(const AITacticalAttackSoAColumns& columns,
                                                                       const AITacticalAttackStateSoAKernelInput& input,
                                                                       size_t slot,
                                                                       AITacticalAttackQueryKind query,
                                                                       uint32_t generation) noexcept
{
    return {.subject = input.subjects[slot],
            .stateRequest = columns.requestAt(slot),
            .wrapperState = columns.state[slot],
            .sourceOrderRevision = columns.sourceOrderRevision[slot],
            .generation = generation,
            .query = query,
            .collection = columns.collectionHandle[slot],
            .collectionRevision = columns.collectionRevision[slot],
            .area = columns.areaHandle[slot],
            .areaRevision = columns.areaRevision[slot]};
}

[[nodiscard]] inline AITacticalAttackChildCorrelation childCorrelation(const AITacticalAttackSoAColumns& columns,
                                                                       const AITacticalAttackStateSoAKernelInput& input,
                                                                       size_t slot) noexcept
{
    return {.subject = input.subjects[slot],
            .stateRequest = columns.requestAt(slot),
            .wrapperState = columns.state[slot],
            .sourceOrderRevision = columns.sourceOrderRevision[slot],
            .childState = columns.childState[slot],
            .generation = columns.childGeneration[slot],
            .target = columns.target[slot],
            .targetRevision = columns.targetRevision[slot]};
}

inline void restoreWrapperPhase(AITacticalAttackSoAColumns& columns, size_t slot) noexcept
{
    if (columns.queryAt(slot) == AITacticalAttackQueryKind::Crate)
        columns.wrapperPhase[slot] = static_cast<uint8_t>(AITacticalAttackWrapperPhase::QueryingCrate);
    else if (columns.queryAt(slot) != AITacticalAttackQueryKind::None)
        columns.wrapperPhase[slot] = static_cast<uint8_t>(AITacticalAttackWrapperPhase::QueryingTarget);
    else if (columns.childState[slot] == AIStateId::AttackObject)
        columns.wrapperPhase[slot] = static_cast<uint8_t>(AITacticalAttackWrapperPhase::RunningAttack);
    else if (columns.childState[slot] == AIStateId::PickUpCrate)
        columns.wrapperPhase[slot] = static_cast<uint8_t>(AITacticalAttackWrapperPhase::RunningPickUpCrate);
    else
        columns.wrapperPhase[slot] = static_cast<uint8_t>(AITacticalAttackWrapperPhase::Idle);
}

inline void clearQuery(AITacticalAttackSoAColumns& columns, size_t slot) noexcept
{
    columns.pendingQuery[slot] = static_cast<uint8_t>(AITacticalAttackQueryKind::None);
    columns.queryGeneration[slot] = 0;
    restoreWrapperPhase(columns, slot);
}

inline void clearChild(AITacticalAttackSoAColumns& columns, size_t slot) noexcept
{
    columns.childState[slot] = AIStateId::Idle;
    columns.childGeneration[slot] = 0;
    columns.target[slot] = INVALID_OBJECT_ID;
    columns.targetRevision[slot] = 0;
    restoreWrapperPhase(columns, slot);
}

[[nodiscard]] inline bool beginQuery(AITacticalAttackSoAColumns& columns,
                                     const AITacticalAttackStateSoAKernelInput& input,
                                     size_t slot,
                                     AITacticalAttackQueryKind query) noexcept
{
    AITacticalAttackQueryCommandBuffer& output = input.queryCommands[slot];
    if (!output.hasCapacity())
        return false;
    const uint32_t generation = allocateGeneration(columns.nextQueryGeneration, slot);
    const AITacticalAttackQueryCorrelation correlation = queryCorrelation(columns, input, slot, query, generation);
    const AITacticalAttackQueryCommand command{
        .correlation = correlation,
        .kind = AITacticalAttackQueryCommandKind::Begin,
        .squadSelection = input.squadSelections[slot],
        // AttackSquad chooses a live member first and lets AttackObject
        // determine whether/how to engage it. Hunt and AttackArea alone use
        // AI::CAN_ATTACK during their victim scan in RefCode.
        .canAttackOnly = query == AITacticalAttackQueryKind::HuntTarget ||
            query == AITacticalAttackQueryKind::AreaTarget,
        .useAttackPriority =
            query == AITacticalAttackQueryKind::HuntTarget || query == AITacticalAttackQueryKind::AreaTarget,
        .fallbackWithoutAttackPriority =
            query == AITacticalAttackQueryKind::HuntTarget && fact(input.allArmyHunt[slot]),
        .useTeamCommonTarget = query == AITacticalAttackQueryKind::HuntTarget && fact(input.useTeamCommonTarget[slot]),
        .confirmedTick = input.confirmedTick,
    };
    if (!correlation.isValid() || !output.push(command))
        return false;
    columns.pendingQuery[slot] = static_cast<uint8_t>(query);
    columns.queryGeneration[slot] = generation;
    if (query == AITacticalAttackQueryKind::HuntTarget || query == AITacticalAttackQueryKind::AreaTarget)
    {
        columns.nextScanTick[slot] = saturatingAdd(input.confirmedTick, input.enemyScanIntervalTicks);
    }
    restoreWrapperPhase(columns, slot);
    return true;
}

inline void emitCancelQuery(const AITacticalAttackSoAColumns& columns,
                            const AITacticalAttackStateSoAKernelInput& input,
                            size_t slot) noexcept
{
    static_cast<void>(input.queryCommands[slot].push({
        .correlation = queryCorrelation(columns, input, slot, columns.queryAt(slot), columns.queryGeneration[slot]),
        .kind = AITacticalAttackQueryCommandKind::Cancel,
        .confirmedTick = input.confirmedTick,
    }));
}

[[nodiscard]] inline bool startChild(AITacticalAttackSoAColumns& columns,
                                     const AITacticalAttackStateSoAKernelInput& input,
                                     size_t slot,
                                     AIStateId childState,
                                     ObjectId target,
                                     AIFixedPosition targetPosition,
                                     bool targetPositionValid,
                                     uint64_t targetRevision) noexcept
{
    if (!input.childCommands[slot].hasCapacity() || !target || targetRevision == 0)
        return false;
    const uint32_t generation = allocateGeneration(columns.nextChildGeneration, slot);
    columns.childState[slot] = childState;
    columns.childGeneration[slot] = generation;
    columns.target[slot] = target;
    columns.targetRevision[slot] = targetRevision;
    const AITacticalAttackChildCommand command{
        .correlation = childCorrelation(columns, input, slot),
        .kind = AITacticalAttackChildCommandKind::StartOrReplace,
        .targetPosition = targetPosition,
        .targetPositionValid = targetPositionValid,
        .releaseTemporaryWeaponLock = false,
        .confirmedTick = input.confirmedTick,
    };
    if (!command.correlation.isChildValid() || !input.childCommands[slot].push(command))
        return false;
    restoreWrapperPhase(columns, slot);
    return true;
}

[[nodiscard]] inline const AITacticalAttackQueryFeedback* matchingQueryFeedback(
    const AITacticalAttackSoAColumns& columns, const AITacticalAttackStateSoAKernelInput& input, size_t slot) noexcept
{
    if (columns.queryAt(slot) == AITacticalAttackQueryKind::None)
        return nullptr;
    const AITacticalAttackQueryCorrelation expected =
        queryCorrelation(columns, input, slot, columns.queryAt(slot), columns.queryGeneration[slot]);
    const AITacticalAttackQueryFeedbackBuffer& buffer = input.queryFeedback[slot];
    const size_t count = std::min(buffer.count, buffer.values.size());
    const AITacticalAttackQueryFeedback* result = nullptr;
    for (size_t index = 0; index < count; ++index)
    {
        const AITacticalAttackQueryFeedback& candidate = buffer.values[index];
        if (candidate.correlation == expected && candidate.status != AITacticalAttackQueryStatus::None &&
            candidate.confirmedTick <= input.confirmedTick &&
            (!result || candidate.confirmedTick >= result->confirmedTick))
        {
            result = &candidate;
        }
    }
    return result;
}

[[nodiscard]] inline const AITacticalAttackChildFeedback* matchingChildFeedback(
    const AITacticalAttackSoAColumns& columns, const AITacticalAttackStateSoAKernelInput& input, size_t slot) noexcept
{
    if (columns.childState[slot] == AIStateId::Idle)
        return nullptr;
    const AITacticalAttackChildCorrelation expected = childCorrelation(columns, input, slot);
    const AITacticalAttackChildFeedbackBuffer& buffer = input.childFeedback[slot];
    const size_t count = std::min(buffer.count, buffer.values.size());
    const AITacticalAttackChildFeedback* result = nullptr;
    for (size_t index = 0; index < count; ++index)
    {
        const AITacticalAttackChildFeedback& candidate = buffer.values[index];
        if (candidate.correlation == expected && candidate.status != AITacticalAttackChildStatus::None &&
            candidate.confirmedTick <= input.confirmedTick &&
            (!result || candidate.confirmedTick >= result->confirmedTick))
        {
            result = &candidate;
        }
    }
    return result;
}

[[nodiscard]] inline bool queryContextStale(const AITacticalAttackSoAColumns& columns,
                                            AIStateId state,
                                            const AITacticalAttackStateSoAKernelInput& input,
                                            size_t slot) noexcept
{
    if (columns.queryAt(slot) != AITacticalAttackQueryKind::SquadTarget &&
        columns.queryAt(slot) != AITacticalAttackQueryKind::AreaTarget)
    {
        return false;
    }
    if (state == AIStateId::AttackSquad)
    {
        return columns.collectionHandle[slot] != input.targetCollections[slot] ||
               columns.collectionRevision[slot] != input.targetCollectionRevisions[slot];
    }
    return columns.areaHandle[slot] != input.attackAreas[slot] ||
           columns.areaRevision[slot] != input.attackAreaRevisions[slot];
}

inline void resetSlot(AITacticalAttackSoAColumns& columns, size_t slot) noexcept
{
    columns.store(slot, AITacticalAttackStatePayload{});
}

} // namespace tactical_attack_detail

[[nodiscard]] inline bool enterTacticalAttackSoA(AITacticalAttackSoAColumns& columns,
                                                 AIStateId state,
                                                 const AITacticalAttackStateSoAKernelInput& input) noexcept
{
    using namespace tactical_attack_detail;
    const AITacticalAttackPolicy policy = tacticalAttackPolicyFor(state);
    if (!policy.valid || !hasAlignedSpans(columns, input))
        return false;

    for (const size_t slot : executionSlotRange(input.executionSlots, columns.size()))
    {
        if (!scheduled(input, slot))
            continue;
        if (state == AIStateId::AttackSquad && !fact(input.effectivelyDead[slot]) && input.subjects[slot] &&
            columns.requestAt(slot).isValid() && input.sourceOrderRevisions[slot] != 0 &&
            input.targetCollections[slot] && input.targetCollectionRevisions[slot] != 0 &&
            !input.queryCommands[slot].hasCapacity())
        {
            return false;
        }
    }

    for (const size_t slot : executionSlotRange(input.executionSlots, columns.size()))
    {
        if (!scheduled(input, slot))
            continue;
        if (fact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        const AIStateRequestId request = columns.requestAt(slot);
        if (!input.subjects[slot] || !request.isValid() || input.sourceOrderRevisions[slot] == 0)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        columns.activate(slot, request);
        columns.active[slot] = 1;
        columns.sourceOrderRevision[slot] = input.sourceOrderRevisions[slot];
        columns.state[slot] = state;
        columns.wrapperPhase[slot] = static_cast<uint8_t>(AITacticalAttackWrapperPhase::Idle);
        columns.nextQueryGeneration[slot] = 1;
        columns.nextChildGeneration[slot] = 1;
        refreshHandles(columns, input, slot);

        if (policy.scansOnTimer)
        {
            columns.nextScanTick[slot] = saturatingAdd(
                input.confirmedTick, boundedJitter(input.initialScanJitter[slot], input.enemyScanIntervalTicks));
        }

        if (state == AIStateId::AttackSquad)
        {
            if (!columns.collectionHandle[slot] || columns.collectionRevision[slot] == 0)
            {
                input.results[slot] = AIStateStepResult::success();
                continue;
            }
            static_cast<void>(beginQuery(columns, input, slot, AITacticalAttackQueryKind::SquadTarget));
        }
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool updateTacticalAttackSoA(AITacticalAttackSoAColumns& columns,
                                                  AIStateId state,
                                                  const AITacticalAttackStateSoAKernelInput& input) noexcept
{
    using namespace tactical_attack_detail;
    const AITacticalAttackPolicy policy = tacticalAttackPolicyFor(state);
    if (!policy.valid || !hasAlignedSpans(columns, input))
        return false;

    for (const size_t slot : executionSlotRange(input.executionSlots, columns.size()))
    {
        if (!scheduled(input, slot))
            continue;
        if (!fact(columns.active[slot]) || columns.state[slot] != state || !columns.requestAt(slot).isValid() ||
            columns.sourceOrderRevision[slot] != input.sourceOrderRevisions[slot])
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (fact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }

        if (queryContextStale(columns, state, input, slot))
        {
            const bool validReplacement =
                state == AIStateId::AttackSquad
                    ? input.targetCollections[slot] && input.targetCollectionRevisions[slot] != 0
                    : input.attackAreas[slot] && input.attackAreaRevisions[slot] != 0;
            const size_t needed = 1 + static_cast<size_t>(validReplacement);
            if (!input.queryCommands[slot].hasCapacity(needed))
            {
                input.results[slot] = AIStateStepResult::blocked();
                continue;
            }
            emitCancelQuery(columns, input, slot);
            clearQuery(columns, slot);
            refreshHandles(columns, input, slot);
            if (!validReplacement)
            {
                input.results[slot] =
                    state == AIStateId::AttackArea ? AIStateStepResult::failure() : AIStateStepResult::success();
                continue;
            }
            const AITacticalAttackQueryKind replacement = state == AIStateId::AttackSquad
                                                              ? AITacticalAttackQueryKind::SquadTarget
                                                              : AITacticalAttackQueryKind::AreaTarget;
            static_cast<void>(beginQuery(columns, input, slot, replacement));
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        if (columns.queryAt(slot) == AITacticalAttackQueryKind::None)
            refreshHandles(columns, input, slot);

        if (const AITacticalAttackChildFeedback* feedback = matchingChildFeedback(columns, input, slot))
        {
            if (feedback->status == AITacticalAttackChildStatus::Unsupported)
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
            if (feedback->status == AITacticalAttackChildStatus::Succeeded ||
                feedback->status == AITacticalAttackChildStatus::Failed)
            {
                if (state == AIStateId::AttackSquad && columns.queryAt(slot) == AITacticalAttackQueryKind::None &&
                    !input.queryCommands[slot].hasCapacity())
                {
                    input.results[slot] = AIStateStepResult::blocked();
                    continue;
                }
                clearChild(columns, slot);
                if (state == AIStateId::AttackSquad && columns.queryAt(slot) == AITacticalAttackQueryKind::None)
                    static_cast<void>(beginQuery(columns, input, slot, AITacticalAttackQueryKind::Crate));
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
        }

        if (const AITacticalAttackQueryFeedback* feedback = matchingQueryFeedback(columns, input, slot))
        {
            if (feedback->status == AITacticalAttackQueryStatus::Unsupported)
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }

            const AITacticalAttackQueryKind query = columns.queryAt(slot);
            const bool hasTarget = feedback->target && feedback->targetRevision != 0;
            if (query == AITacticalAttackQueryKind::Crate && !hasTarget)
            {
                const bool validSquad = state != AIStateId::AttackSquad ||
                                        (input.targetCollections[slot] && input.targetCollectionRevisions[slot] != 0);
                if (validSquad && !input.queryCommands[slot].hasCapacity())
                {
                    input.results[slot] = AIStateStepResult::blocked();
                    continue;
                }
                clearQuery(columns, slot);
                refreshHandles(columns, input, slot);
                if (!validSquad)
                {
                    input.results[slot] = AIStateStepResult::success();
                    continue;
                }
                const AITacticalAttackQueryKind next = state == AIStateId::AttackSquad
                                                           ? AITacticalAttackQueryKind::SquadTarget
                                                           : AITacticalAttackQueryKind::HuntTarget;
                static_cast<void>(beginQuery(columns, input, slot, next));
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (hasTarget)
            {
                const AIStateId desiredChild =
                    query == AITacticalAttackQueryKind::Crate ? AIStateId::PickUpCrate : AIStateId::AttackObject;
                if (columns.childState[slot] == desiredChild && columns.target[slot] == feedback->target &&
                    columns.targetRevision[slot] == feedback->targetRevision)
                {
                    clearQuery(columns, slot);
                    input.results[slot] = AIStateStepResult::continueState();
                    continue;
                }
                if (!input.childCommands[slot].hasCapacity())
                {
                    input.results[slot] = AIStateStepResult::blocked();
                    continue;
                }
                const ObjectId target = feedback->target;
                const uint64_t revision = feedback->targetRevision;
                clearQuery(columns, slot);
                static_cast<void>(startChild(columns, input, slot, desiredChild,
                                             target, feedback->targetPosition,
                                             feedback->targetPositionValid,
                                             revision));
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }

            clearQuery(columns, slot);
            if (state == AIStateId::Hunt &&
                (fact(input.allArmyHunt[slot]) || columns.childState[slot] != AIStateId::Idle))
            {
                input.results[slot] = AIStateStepResult::continueState();
            }
            else
            {
                input.results[slot] = AIStateStepResult::success();
            }
            continue;
        }

        if (columns.queryAt(slot) != AITacticalAttackQueryKind::None)
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }

        if (state == AIStateId::AttackSquad)
        {
            if (columns.childState[slot] == AIStateId::Idle)
            {
                input.results[slot] = beginQuery(columns, input, slot, AITacticalAttackQueryKind::Crate)
                                          ? AIStateStepResult::continueState()
                                          : AIStateStepResult::blocked();
            }
            else
            {
                input.results[slot] = AIStateStepResult::continueState();
            }
            continue;
        }

        if (input.confirmedTick < columns.nextScanTick[slot])
        {
            input.results[slot] = columns.childState[slot] == AIStateId::Idle
                                      ? AIStateStepResult::sleepUntil(columns.nextScanTick[slot])
                                      : AIStateStepResult::continueState();
            continue;
        }
        if (fact(input.allWeaponsOutOfAmmo[slot]) && !fact(input.projectile[slot]))
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (state == AIStateId::AttackArea && (!columns.areaHandle[slot] || columns.areaRevision[slot] == 0))
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        const AITacticalAttackQueryKind query =
            state == AIStateId::Hunt ? AITacticalAttackQueryKind::Crate : AITacticalAttackQueryKind::AreaTarget;
        input.results[slot] =
            beginQuery(columns, input, slot, query) ? AIStateStepResult::continueState() : AIStateStepResult::blocked();
    }
    return true;
}

[[nodiscard]] inline bool canExitTacticalAttackSoA(const AITacticalAttackSoAColumns& columns,
                                                   AIStateId state,
                                                   const AITacticalAttackStateSoAKernelInput& input) noexcept
{
    using namespace tactical_attack_detail;
    if (!tacticalAttackPolicyFor(state).valid || !hasAlignedSpans(columns, input))
        return false;
    for (const size_t slot : executionSlotRange(input.executionSlots, columns.size()))
    {
        if (!scheduled(input, slot) || !fact(columns.active[slot]) ||
            columns.state[slot] != state)
            continue;
        if (!input.childCommands[slot].hasCapacity())
            return false;
        if (columns.queryAt(slot) != AITacticalAttackQueryKind::None && !input.queryCommands[slot].hasCapacity())
            return false;
    }
    return true;
}

[[nodiscard]] inline bool exitTacticalAttackSoA(AITacticalAttackSoAColumns& columns,
                                                AIStateId state,
                                                const AITacticalAttackStateSoAKernelInput& input) noexcept
{
    using namespace tactical_attack_detail;
    if (!canExitTacticalAttackSoA(columns, state, input))
        return false;

    for (const size_t slot : executionSlotRange(input.executionSlots, columns.size()))
    {
        if (!scheduled(input, slot) || !fact(columns.active[slot]) ||
            columns.state[slot] != state)
            continue;
        if (columns.queryAt(slot) != AITacticalAttackQueryKind::None)
            emitCancelQuery(columns, input, slot);
        const AITacticalAttackChildCorrelation correlation = childCorrelation(columns, input, slot);
        static_cast<void>(input.childCommands[slot].push({
            .correlation = correlation,
            .kind = AITacticalAttackChildCommandKind::EndWrapper,
            .releaseTemporaryWeaponLock = state == AIStateId::Hunt,
            .confirmedTick = input.confirmedTick,
        }));
        resetSlot(columns, slot);
    }
    return true;
}

[[nodiscard]] inline bool enterTacticalAttackStateSoA(AITacticalAttackSoAColumns& columns,
                                                      AIStateId state,
                                                      const AITacticalAttackStateSoAKernelInput& input) noexcept
{
    return enterTacticalAttackSoA(columns, state, input);
}

[[nodiscard]] inline bool updateTacticalAttackStateSoA(AITacticalAttackSoAColumns& columns,
                                                       AIStateId state,
                                                       const AITacticalAttackStateSoAKernelInput& input) noexcept
{
    return updateTacticalAttackSoA(columns, state, input);
}

[[nodiscard]] inline bool canExitTacticalAttackStateSoA(const AITacticalAttackSoAColumns& columns,
                                                        AIStateId state,
                                                        const AITacticalAttackStateSoAKernelInput& input) noexcept
{
    return canExitTacticalAttackSoA(columns, state, input);
}

[[nodiscard]] inline bool exitTacticalAttackStateSoA(AITacticalAttackSoAColumns& columns,
                                                     AIStateId state,
                                                     const AITacticalAttackStateSoAKernelInput& input) noexcept
{
    return exitTacticalAttackSoA(columns, state, input);
}

#define ENGINE_AI_DEFINE_TACTICAL_ATTACK_SLICE(NAME, STATE)                                                            \
    [[nodiscard]] inline bool enter##NAME##StateSoA(AITacticalAttackSoAColumns& columns,                               \
                                                    const AITacticalAttackStateSoAKernelInput& input) noexcept         \
    {                                                                                                                  \
        return enterTacticalAttackSoA(columns, STATE, input);                                                          \
    }                                                                                                                  \
    [[nodiscard]] inline bool update##NAME##StateSoA(AITacticalAttackSoAColumns& columns,                              \
                                                     const AITacticalAttackStateSoAKernelInput& input) noexcept        \
    {                                                                                                                  \
        return updateTacticalAttackSoA(columns, STATE, input);                                                         \
    }                                                                                                                  \
    [[nodiscard]] inline bool canExit##NAME##StateSoA(const AITacticalAttackSoAColumns& columns,                       \
                                                      const AITacticalAttackStateSoAKernelInput& input) noexcept       \
    {                                                                                                                  \
        return canExitTacticalAttackSoA(columns, STATE, input);                                                        \
    }                                                                                                                  \
    [[nodiscard]] inline bool exit##NAME##StateSoA(AITacticalAttackSoAColumns& columns,                                \
                                                   const AITacticalAttackStateSoAKernelInput& input) noexcept          \
    {                                                                                                                  \
        return exitTacticalAttackSoA(columns, STATE, input);                                                           \
    }

ENGINE_AI_DEFINE_TACTICAL_ATTACK_SLICE(Hunt, AIStateId::Hunt)
ENGINE_AI_DEFINE_TACTICAL_ATTACK_SLICE(AttackSquad, AIStateId::AttackSquad)
ENGINE_AI_DEFINE_TACTICAL_ATTACK_SLICE(AttackArea, AIStateId::AttackArea)

#undef ENGINE_AI_DEFINE_TACTICAL_ATTACK_SLICE

} // namespace engine::ai
