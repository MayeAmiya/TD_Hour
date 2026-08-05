#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "core/container/container_types.h"
#include "game/object/ai/states/combat/AIGuardStateData.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

struct AIGuardStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    uint32_t enemyScanIntervalTicks = 15;
    uint32_t returnScanIntervalTicks = 30;
    uint32_t chaseDurationTicks = 300;
    int64_t anchorMoveThresholdRaw = 0;
    container::Span<const uint8_t> scheduled{};
    AIExecutionSlotRange executionSlots{};
    container::Span<const ObjectId> subjects;
    container::Span<const uint64_t> sourceOrderRevisions;
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> mobile;
    container::Span<const uint8_t> allWeaponsOutOfAmmo;
    container::Span<const uint8_t> projectile;
    container::Span<const uint8_t> jetAI;
    container::Span<const uint8_t> enterGuard;
    container::Span<const uint8_t> guardWithoutPursuit;
    container::Span<const uint8_t> flyingOnly;
    container::Span<const uint8_t> tracksAnchor;
    container::Span<const uint8_t> contained;
    container::Span<const AIFixedPosition> currentAnchor;
    container::Span<const ObjectId> initialNemesis;
    container::Span<const ObjectId> priorityNemesis;
    container::Span<const ObjectId> aggressor;
    container::Span<const ObjectId> crate;
    container::Span<const AIFixedPosition> cratePosition;
    container::Span<const uint8_t> cratePositionValid;
    container::Span<const ObjectId> nearestTunnel;
    container::Span<const int64_t> guardRangeRaw;
    container::Span<const int64_t> visionRangeRaw;
    container::Span<const AIAttackAreaHandle> guardAreas;
    container::Span<const uint64_t> guardAreaRevisions;
    container::Span<const uint32_t> initialScanJitter;
    // Guard child-state completion is fed back into this bounded buffer by
    // the multiwave executor before the guard kernel consumes it.
    container::Span<AIGuardFeedbackBuffer> feedback;
    container::Span<AIGuardTacticalCommandBuffer> tacticalCommands;
    container::Span<AIGuardInteractionCommandBuffer> interactionCommands;
    container::Span<AIStateStepResult> results;
};

namespace guard_detail
{

[[nodiscard]] constexpr bool fact(uint8_t value) noexcept
{
    return value != 0;
}

[[nodiscard]] constexpr uint64_t saturatingAdd(uint64_t left, uint64_t right) noexcept
{
    return right > std::numeric_limits<uint64_t>::max() - left ? std::numeric_limits<uint64_t>::max() : left + right;
}

[[nodiscard]] constexpr int64_t saturatingAdd(int64_t left, int64_t right) noexcept
{
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
        return std::numeric_limits<int64_t>::max();
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
        return std::numeric_limits<int64_t>::min();
    return left + right;
}

[[nodiscard]] constexpr int64_t nonNegative(int64_t value) noexcept
{
    return value < 0 ? 0 : value;
}

[[nodiscard]] constexpr int64_t scaleRadius(int64_t value, uint32_t numerator, uint32_t denominator) noexcept
{
    value = nonNegative(value);
    if (value == 0 || numerator == 0)
        return 0;
    const uint64_t unsignedValue = static_cast<uint64_t>(value);
    const uint64_t quotient = unsignedValue / denominator;
    const uint64_t remainder = unsignedValue % denominator;
    const uint64_t maximum = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (quotient > maximum / numerator)
        return std::numeric_limits<int64_t>::max();
    const uint64_t scaledQuotient = quotient * numerator;
    const uint64_t scaledRemainder = (remainder * numerator) / denominator;
    return scaledRemainder > maximum - scaledQuotient ? std::numeric_limits<int64_t>::max()
                                                      : static_cast<int64_t>(scaledQuotient + scaledRemainder);
}

[[nodiscard]] constexpr bool movedBeyond(const AIFixedPosition& prior,
                                         const AIFixedPosition& current,
                                         int64_t threshold) noexcept
{
    threshold = nonNegative(threshold);
    const auto beyond = [threshold](int64_t left, int64_t right) constexpr noexcept
    {
        if (left >= right)
            return static_cast<uint64_t>(left) - static_cast<uint64_t>(right) > static_cast<uint64_t>(threshold);
        return static_cast<uint64_t>(right) - static_cast<uint64_t>(left) > static_cast<uint64_t>(threshold);
    };
    return beyond(prior.xRaw, current.xRaw) || beyond(prior.yRaw, current.yRaw);
}

[[nodiscard]] inline bool scheduled(const AIGuardStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || fact(input.scheduled[slot]);
}

[[nodiscard]] bool hasAlignedSpans(const AIGuardSoAColumns& columns,
                                   const AIGuardStateSoAKernelInput& input) noexcept;

[[nodiscard]] inline uint32_t allocateRevision(AIGuardSoAColumns& columns, size_t slot) noexcept
{
    uint32_t revision = columns.nextOperationRevision[slot];
    if (revision == 0)
        revision = 1;
    columns.nextOperationRevision[slot] = revision + 1;
    if (columns.nextOperationRevision[slot] == 0)
        columns.nextOperationRevision[slot] = 1;
    return revision;
}

[[nodiscard]] inline AIGuardCorrelation correlation(const AIGuardSoAColumns& columns,
                                                    AIStateId state,
                                                    ObjectId subject,
                                                    size_t slot,
                                                    AIGuardOperation operation,
                                                    uint32_t revision) noexcept
{
    return {.subject = subject,
            .stateRequest = columns.requestAt(slot),
            .state = state,
            .phase = columns.phaseAt(slot),
            .operation = operation,
            .sourceOrderRevision = columns.sourceOrderRevision[slot],
            .operationRevision = revision};
}

[[nodiscard]] inline const AIGuardFeedback* matchingFeedback(const AIGuardFeedbackBuffer& buffer,
                                                             const AIGuardCorrelation& expected) noexcept
{
    for (size_t index = 0; index < buffer.count; ++index)
    {
        if (buffer.values[index].correlation == expected && buffer.values[index].kind != AIGuardFeedbackKind::None)
        {
            return &buffer.values[index];
        }
    }
    return nullptr;
}

inline void setPhase(AIGuardSoAColumns& columns, size_t slot, AIGuardPhase phase) noexcept
{
    columns.phase[slot] = static_cast<uint8_t>(phase);
    columns.taskOperation[slot] = static_cast<uint8_t>(AIGuardOperation::None);
    columns.taskDomain[slot] = static_cast<uint8_t>(AIGuardOperationDomain::None);
    columns.taskRevision[slot] = 0;
    columns.chaseDeadlineTick[slot] = 0;
}

[[nodiscard]] constexpr uint64_t boundedJitter(uint32_t draw, uint32_t interval) noexcept
{
    return interval == std::numeric_limits<uint32_t>::max() ? draw : draw % (interval + 1u);
}

inline void enterPhase(AIGuardSoAColumns& columns,
                       const AIGuardStateSoAKernelInput& input,
                       size_t slot,
                       AIGuardPhase phase) noexcept
{
    setPhase(columns, slot, phase);
    if (phase == AIGuardPhase::Return)
    {
        columns.nextScanTick[slot] = saturatingAdd(
            input.confirmedTick, boundedJitter(input.initialScanJitter[slot], input.returnScanIntervalTicks));
    }
    else if (phase == AIGuardPhase::Idle)
    {
        columns.nextScanTick[slot] = saturatingAdd(
            input.confirmedTick, boundedJitter(input.initialScanJitter[slot], input.enemyScanIntervalTicks));
    }
}

inline void clearTask(AIGuardSoAColumns& columns, size_t slot) noexcept
{
    columns.taskOperation[slot] = static_cast<uint8_t>(AIGuardOperation::None);
    columns.taskDomain[slot] = static_cast<uint8_t>(AIGuardOperationDomain::None);
    columns.taskRevision[slot] = 0;
}

[[nodiscard]] inline int64_t innerRadius(const AIGuardPolicy& policy,
                                         const AIGuardStateSoAKernelInput& input,
                                         size_t slot) noexcept
{
    if (policy.tunnelNetwork)
        return 0;
    return policy.retaliate ? scaleRadius(input.guardRangeRaw[slot], 3, 2) : nonNegative(input.guardRangeRaw[slot]);
}

[[nodiscard]] inline int64_t outerRadius(const AIGuardPolicy& policy,
                                         const AIGuardStateSoAKernelInput& input,
                                         size_t slot) noexcept
{
    if (policy.tunnelNetwork)
        return 0;
    if (!policy.retaliate)
        return std::max(nonNegative(input.visionRangeRaw[slot]),
                        nonNegative(input.guardRangeRaw[slot]));
    return scaleRadius(
        saturatingAdd(nonNegative(input.visionRangeRaw[slot]), nonNegative(input.guardRangeRaw[slot])), 67, 100);
}

[[nodiscard]] inline int64_t aggressorRadius(const AIGuardPolicy& policy,
                                             const AIGuardStateSoAKernelInput& input,
                                             size_t slot) noexcept
{
    if (policy.tunnelNetwork)
        return 0;
    if (!policy.retaliate)
        return nonNegative(input.guardRangeRaw[slot]);
    return saturatingAdd(nonNegative(input.visionRangeRaw[slot]), nonNegative(input.guardRangeRaw[slot]));
}

[[nodiscard]] constexpr bool clearsTeamTargetOnExit(
    AIStateId state, AIGuardPhase phase) noexcept
{
    // RefCode clears the shared Team target when ordinary/retaliate Inner or
    // any AttackAggressor state exits. Tunnel-network Inner and every Outer,
    // Return and Idle exit retain it.
    return phase == AIGuardPhase::AttackAggressor ||
        (phase == AIGuardPhase::Inner &&
         state != AIStateId::GuardTunnelNetwork);
}

[[nodiscard]] inline bool beginTactical(AIGuardSoAColumns& columns,
                                        AIStateId state,
                                        const AIGuardStateSoAKernelInput& input,
                                        size_t slot,
                                        AIGuardOperation operation,
                                        AIGuardTacticalCommandKind kind,
                                        ObjectId target,
                                        int64_t radiusRaw) noexcept
{
    AIGuardTacticalCommandBuffer& output = input.tacticalCommands[slot];
    if (!output.hasCapacity())
        return false;
    const uint32_t revision = allocateRevision(columns, slot);
    const AIGuardPolicy policy = guardPolicyFor(state);
    const AIGuardTacticalCommand command{
        .correlation = correlation(columns, state, input.subjects[slot], slot, operation, revision),
        .kind = kind,
        .target = target,
        .anchor = columns.anchorAt(slot),
        .radiusRaw = radiusRaw,
        .enterGuardTargets = fact(input.enterGuard[slot]),
        .rejectOrdinaryBuildings = policy.rejectsOrdinaryBuildings,
        .flyingOnly = fact(input.flyingOnly[slot]),
        .publishTunnelNemesis = policy.tunnelNetwork && operation == AIGuardOperation::Attack,
        .clearTeamTarget = false,
        .confirmedTick = input.confirmedTick,
    };
    if (!command.correlation.isValid() || !output.push(command))
        return false;
    columns.taskOperation[slot] = static_cast<uint8_t>(operation);
    columns.taskDomain[slot] = static_cast<uint8_t>(AIGuardOperationDomain::Tactical);
    columns.taskRevision[slot] = revision;
    return true;
}

[[nodiscard]] inline bool beginInteraction(AIGuardSoAColumns& columns,
                                           AIStateId state,
                                           const AIGuardStateSoAKernelInput& input,
                                           size_t slot,
                                           AIGuardOperation operation,
                                           AIGuardInteractionCommandKind kind,
                                           ObjectId target,
                                           bool urgent,
                                           AIFixedPosition targetPosition = {},
                                           bool targetPositionValid = false) noexcept
{
    AIGuardInteractionCommandBuffer& output = input.interactionCommands[slot];
    if (!output.hasCapacity())
        return false;
    const uint32_t revision = allocateRevision(columns, slot);
    const AIGuardInteractionCommand command{
        .correlation = correlation(columns, state, input.subjects[slot], slot, operation, revision),
        .kind = kind,
        .target = target,
        .targetPosition = targetPosition,
        .targetPositionValid = targetPositionValid,
        .urgent = urgent,
        .clearTeamTarget = false,
        .confirmedTick = input.confirmedTick,
    };
    if (!command.correlation.isValid() || !output.push(command))
        return false;
    columns.taskOperation[slot] = static_cast<uint8_t>(operation);
    columns.taskDomain[slot] = static_cast<uint8_t>(AIGuardOperationDomain::Interaction);
    columns.taskRevision[slot] = revision;
    return true;
}

[[nodiscard]] inline bool beginScan(AIGuardSoAColumns& columns,
                                    AIStateId state,
                                    const AIGuardStateSoAKernelInput& input,
                                    size_t slot,
                                    uint32_t interval) noexcept
{
    AIGuardTacticalCommandBuffer& output = input.tacticalCommands[slot];
    if (!output.hasCapacity())
        return false;
    const uint32_t revision = allocateRevision(columns, slot);
    const AIGuardPolicy policy = guardPolicyFor(state);
    const AIGuardTacticalCommand command{
        .correlation = correlation(columns, state, input.subjects[slot], slot, AIGuardOperation::Scan, revision),
        .kind = AIGuardTacticalCommandKind::ScanForTarget,
        .target = {},
        .anchor = columns.anchorAt(slot),
        .radiusRaw = innerRadius(policy, input, slot),
        .area = input.guardAreas[slot],
        .areaRevision = input.guardAreaRevisions[slot],
        .enterGuardTargets = fact(input.enterGuard[slot]),
        .rejectOrdinaryBuildings = policy.rejectsOrdinaryBuildings,
        .flyingOnly = fact(input.flyingOnly[slot]),
        .publishTunnelNemesis = false,
        .clearTeamTarget = false,
        .confirmedTick = input.confirmedTick,
    };
    if (!command.correlation.isValid() || !output.push(command))
        return false;
    columns.scanPending[slot] = 1;
    columns.scanRevision[slot] = revision;
    columns.nextScanTick[slot] = saturatingAdd(input.confirmedTick, interval);
    return true;
}

[[nodiscard]] inline bool cancelOutstanding(AIGuardSoAColumns& columns,
                                            AIStateId state,
                                            const AIGuardStateSoAKernelInput& input,
                                            size_t slot) noexcept
{
    const bool cancelScan = fact(columns.scanPending[slot]);
    const AIGuardOperationDomain domain = columns.taskDomainAt(slot);
    const size_t tacticalCount =
        static_cast<size_t>(cancelScan) + static_cast<size_t>(domain == AIGuardOperationDomain::Tactical);
    const size_t interactionCount = static_cast<size_t>(domain == AIGuardOperationDomain::Interaction);
    if (!input.tacticalCommands[slot].hasCapacity(tacticalCount) ||
        !input.interactionCommands[slot].hasCapacity(interactionCount))
    {
        return false;
    }

    if (cancelScan)
    {
        static_cast<void>(input.tacticalCommands[slot].push({
            .correlation = correlation(
                columns, state, input.subjects[slot], slot, AIGuardOperation::Scan, columns.scanRevision[slot]),
            .kind = AIGuardTacticalCommandKind::Cancel,
            .confirmedTick = input.confirmedTick,
        }));
        columns.scanPending[slot] = 0;
        columns.scanRevision[slot] = 0;
    }

    if (domain == AIGuardOperationDomain::Tactical)
    {
        static_cast<void>(input.tacticalCommands[slot].push({
            .correlation = correlation(
                columns, state, input.subjects[slot], slot, columns.taskAt(slot), columns.taskRevision[slot]),
            .kind = AIGuardTacticalCommandKind::Cancel,
            .clearTeamTarget = clearsTeamTargetOnExit(
                state, columns.phaseAt(slot)),
            .confirmedTick = input.confirmedTick,
        }));
    }
    else if (domain == AIGuardOperationDomain::Interaction)
    {
        static_cast<void>(input.interactionCommands[slot].push({
            .correlation = correlation(
                columns, state, input.subjects[slot], slot, columns.taskAt(slot), columns.taskRevision[slot]),
            .kind = AIGuardInteractionCommandKind::Cancel,
            .clearTeamTarget = clearsTeamTargetOnExit(
                state, columns.phaseAt(slot)),
            .confirmedTick = input.confirmedTick,
        }));
    }
    clearTask(columns, slot);
    return true;
}

[[nodiscard]] inline bool transition(AIGuardSoAColumns& columns,
                                     AIStateId state,
                                     const AIGuardStateSoAKernelInput& input,
                                     size_t slot,
                                     AIGuardPhase next) noexcept
{
    if (!cancelOutstanding(columns, state, input, slot))
        return false;
    enterPhase(columns, input, slot, next);
    return true;
}

[[nodiscard]] inline bool beginCurrentPhase(AIGuardSoAColumns& columns,
                                            AIStateId state,
                                            const AIGuardStateSoAKernelInput& input,
                                            size_t slot) noexcept
{
    const AIGuardPolicy policy = guardPolicyFor(state);
    switch (columns.phaseAt(slot))
    {
    case AIGuardPhase::Return:
        if (policy.returnUsesEnter)
        {
            if (fact(input.contained[slot]))
            {
                enterPhase(columns, input, slot, AIGuardPhase::Idle);
                return true;
            }
            if (!input.nearestTunnel[slot])
            {
                enterPhase(columns, input, slot, AIGuardPhase::Inner);
                return true;
            }
            return beginInteraction(columns,
                                    state,
                                    input,
                                    slot,
                                    AIGuardOperation::Enter,
                                    AIGuardInteractionCommandKind::BeginEnter,
                                    input.nearestTunnel[slot],
                                    false);
        }
        if (!fact(input.mobile[slot]))
            return false;
        return beginTactical(columns,
                             state,
                             input,
                             slot,
                             AIGuardOperation::Move,
                             AIGuardTacticalCommandKind::BeginMove,
                             {},
                             0);
    case AIGuardPhase::Idle:
        return true;
    case AIGuardPhase::Inner:
        if (!columns.nemesis[slot])
        {
            enterPhase(columns, input, slot, AIGuardPhase::Outer);
            return true;
        }
        if (fact(input.enterGuard[slot]) && !policy.tunnelNetwork)
        {
            return beginInteraction(columns,
                                    state,
                                    input,
                                    slot,
                                    AIGuardOperation::Enter,
                                    AIGuardInteractionCommandKind::BeginEnter,
                                    columns.nemesis[slot],
                                    false);
        }
        columns.chaseDeadlineTick[slot] =
            policy.tunnelNetwork ? saturatingAdd(input.confirmedTick, input.chaseDurationTicks) : 0;
        return beginTactical(columns,
                             state,
                             input,
                             slot,
                             AIGuardOperation::Attack,
                             AIGuardTacticalCommandKind::BeginAttack,
                             columns.nemesis[slot],
                             innerRadius(policy, input, slot));
    case AIGuardPhase::Outer:
        if ((!policy.retaliate && fact(input.guardWithoutPursuit[slot])) || !columns.nemesis[slot])
        {
            enterPhase(columns, input, slot, AIGuardPhase::GetCrate);
            return true;
        }
        columns.chaseDeadlineTick[slot] = saturatingAdd(input.confirmedTick, input.chaseDurationTicks);
        return beginTactical(columns,
                             state,
                             input,
                             slot,
                             AIGuardOperation::Attack,
                             AIGuardTacticalCommandKind::BeginAttack,
                             columns.nemesis[slot],
                             outerRadius(policy, input, slot));
    case AIGuardPhase::GetCrate:
        if (!input.crate[slot] || !fact(input.cratePositionValid[slot]))
        {
            enterPhase(columns, input, slot, AIGuardPhase::Return);
            return true;
        }
        return beginInteraction(columns,
                                state,
                                input,
                                slot,
                                AIGuardOperation::PickUpCrate,
                                AIGuardInteractionCommandKind::BeginPickUpCrate,
                                input.crate[slot],
                                false,
                                input.cratePosition[slot],
                                fact(input.cratePositionValid[slot]));
    case AIGuardPhase::AttackAggressor:
    {
        ObjectId target = columns.nemesis[slot];
        if (!policy.retaliate || !target)
            target = input.aggressor[slot];
        if (!target)
        {
            enterPhase(columns, input, slot, policy.aggressorCompletionPhase);
            return true;
        }
        columns.nemesis[slot] = target;
        columns.chaseDeadlineTick[slot] = saturatingAdd(input.confirmedTick, input.chaseDurationTicks);
        return beginTactical(columns,
                             state,
                             input,
                             slot,
                             AIGuardOperation::Attack,
                             AIGuardTacticalCommandKind::BeginAttack,
                             target,
                             aggressorRadius(policy, input, slot));
    }
    case AIGuardPhase::Inactive:
        return false;
    }
    return false;
}

[[nodiscard]] inline const AIGuardFeedback* taskFeedback(const AIGuardSoAColumns& columns,
                                                         AIStateId state,
                                                         const AIGuardStateSoAKernelInput& input,
                                                         size_t slot) noexcept
{
    if (columns.taskAt(slot) == AIGuardOperation::None)
        return nullptr;
    return matchingFeedback(
        input.feedback[slot],
        correlation(columns, state, input.subjects[slot], slot, columns.taskAt(slot), columns.taskRevision[slot]));
}

[[nodiscard]] inline const AIGuardFeedback* scanFeedback(const AIGuardSoAColumns& columns,
                                                         AIStateId state,
                                                         const AIGuardStateSoAKernelInput& input,
                                                         size_t slot) noexcept
{
    if (!fact(columns.scanPending[slot]))
        return nullptr;
    return matchingFeedback(
        input.feedback[slot],
        correlation(columns, state, input.subjects[slot], slot, AIGuardOperation::Scan, columns.scanRevision[slot]));
}

inline void refreshAnchor(AIGuardSoAColumns& columns, const AIGuardStateSoAKernelInput& input, size_t slot) noexcept
{
    if (!fact(input.tracksAnchor[slot]))
        return;
    columns.anchorX[slot] = input.currentAnchor[slot].xRaw;
    columns.anchorY[slot] = input.currentAnchor[slot].yRaw;
    columns.anchorZ[slot] = input.currentAnchor[slot].zRaw;
}

[[nodiscard]] inline AIStateStepResult processScan(AIGuardSoAColumns& columns,
                                                   AIStateId state,
                                                   const AIGuardStateSoAKernelInput& input,
                                                   size_t slot,
                                                   const AIGuardFeedback& feedback) noexcept
{
    const AIGuardPolicy policy = guardPolicyFor(state);
    if (feedback.kind == AIGuardFeedbackKind::Progress)
        return AIStateStepResult::continueState();
    if (feedback.kind == AIGuardFeedbackKind::Unsupported)
    {
        columns.scanPending[slot] = 0;
        columns.scanRevision[slot] = 0;
        return AIStateStepResult::unsupported();
    }

    if (feedback.kind == AIGuardFeedbackKind::Succeeded && feedback.target)
    {
        if (policy.tunnelNetwork && fact(input.contained[slot]) && !input.nearestTunnel[slot])
            return AIStateStepResult::blocked();
        const AIGuardOperationDomain domain = columns.taskDomainAt(slot);
        const size_t tacticalCount = static_cast<size_t>(domain == AIGuardOperationDomain::Tactical);
        const size_t interactionCount = static_cast<size_t>(domain == AIGuardOperationDomain::Interaction) +
                                        static_cast<size_t>(policy.tunnelNetwork && fact(input.contained[slot]));
        if (!input.tacticalCommands[slot].hasCapacity(tacticalCount) ||
            !input.interactionCommands[slot].hasCapacity(interactionCount))
        {
            return AIStateStepResult::blocked();
        }

        columns.scanPending[slot] = 0;
        columns.scanRevision[slot] = 0;
        columns.nemesis[slot] = feedback.target;
        if (policy.tunnelNetwork && fact(input.contained[slot]))
        {
            static_cast<void>(beginInteraction(columns,
                                               state,
                                               input,
                                               slot,
                                               AIGuardOperation::ExitTunnel,
                                               AIGuardInteractionCommandKind::ExitTunnel,
                                               input.nearestTunnel[slot],
                                               true));
            return AIStateStepResult::continueState();
        }
        static_cast<void>(transition(columns, state, input, slot, AIGuardPhase::Inner));
        return AIStateStepResult::continueState();
    }

    columns.scanPending[slot] = 0;
    columns.scanRevision[slot] = 0;
    if (columns.phaseAt(slot) == AIGuardPhase::Idle)
    {
        if (policy.idleWithoutTargetCompletes)
            return AIStateStepResult::success();
        if (policy.tunnelNetwork && !fact(input.contained[slot]) && input.nearestTunnel[slot])
            enterPhase(columns, input, slot, AIGuardPhase::Return);
    }
    return AIStateStepResult::sleepUntil(columns.nextScanTick[slot]);
}

[[nodiscard]] AIStateStepResult processTask(AIGuardSoAColumns& columns,
                                            AIStateId state,
                                            const AIGuardStateSoAKernelInput& input,
                                            size_t slot,
                                            const AIGuardFeedback& feedback) noexcept;

} // namespace guard_detail

[[nodiscard]] bool enterGuardSoA(AIGuardSoAColumns& columns,
                                 AIStateId state,
                                 const AIGuardStateSoAKernelInput& input) noexcept;

[[nodiscard]] inline bool updateGuardSoA(AIGuardSoAColumns& columns,
                                         AIStateId state,
                                         const AIGuardStateSoAKernelInput& input) noexcept
{
    using namespace guard_detail;
    const AIGuardPolicy policy = guardPolicyFor(state);
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
        const bool rejectsEmptyWeaponSet = policy.tunnelNetwork ||
            (fact(input.jetAI[slot]) && !fact(input.enterGuard[slot]));
        if (rejectsEmptyWeaponSet &&
            fact(input.allWeaponsOutOfAmmo[slot]) &&
            !fact(input.projectile[slot]))
        {
            // AITunnelNetworkGuardState always rejects an empty WeaponSet.
            // AIGuardState/AIGuardRetaliate do so only for JetAIUpdate and
            // explicitly exempt EnterGuard objects and projectiles.
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }

        const AIFixedPosition priorAnchor = columns.anchorAt(slot);
        refreshAnchor(columns, input, slot);

        if (policy.tunnelNetwork &&
            columns.phaseAt(slot) == AIGuardPhase::Inner &&
            input.priorityNemesis[slot] &&
            input.priorityNemesis[slot] != columns.nemesis[slot])
        {
            // AITNGuardInnerState adopts a newly published Team target while
            // already fighting. Replace the nested attack in the same logic
            // tick; retaining the old AttackObject would leave the wrapper's
            // nemesis and the actual weapon target divergent.
            if (!input.tacticalCommands[slot].hasCapacity(2) ||
                !transition(columns, state, input, slot,
                            AIGuardPhase::Inner))
            {
                input.results[slot] = AIStateStepResult::blocked();
            }
            else
            {
                columns.nemesis[slot] = input.priorityNemesis[slot];
                input.results[slot] = beginCurrentPhase(
                    columns, state, input, slot)
                    ? AIStateStepResult::continueState()
                    : AIStateStepResult::blocked();
            }
            continue;
        }

        if (policy.tunnelNetwork && columns.phaseAt(slot) == AIGuardPhase::Return && input.priorityNemesis[slot])
        {
            if (!transition(columns, state, input, slot, AIGuardPhase::Inner))
                input.results[slot] = AIStateStepResult::blocked();
            else
            {
                columns.nemesis[slot] = input.priorityNemesis[slot];
                input.results[slot] = AIStateStepResult::continueState();
            }
            continue;
        }

        if (policy.returnMayRetaliate && columns.phaseAt(slot) == AIGuardPhase::Return && input.aggressor[slot])
        {
            if (!transition(columns, state, input, slot, AIGuardPhase::AttackAggressor))
                input.results[slot] = AIStateStepResult::blocked();
            else
            {
                columns.nemesis[slot] = input.aggressor[slot];
                input.results[slot] = AIStateStepResult::continueState();
            }
            continue;
        }

        if (const AIGuardFeedback* feedback = scanFeedback(columns, state, input, slot))
        {
            const AIGuardFeedback saved = *feedback;
            input.results[slot] = processScan(columns, state, input, slot, saved);
            continue;
        }

        if (const AIGuardFeedback* feedback = taskFeedback(columns, state, input, slot))
        {
            const AIGuardFeedback saved = *feedback;
            input.results[slot] = processTask(columns, state, input, slot, saved);
            continue;
        }

        const AIGuardPhase phase = columns.phaseAt(slot);
        if (phase == AIGuardPhase::Idle && columns.taskAt(slot) == AIGuardOperation::None)
        {
            if (input.aggressor[slot])
            {
                if (!transition(columns, state, input, slot, AIGuardPhase::AttackAggressor))
                    input.results[slot] = AIStateStepResult::blocked();
                else
                {
                    columns.nemesis[slot] = input.aggressor[slot];
                    input.results[slot] = AIStateStepResult::continueState();
                }
                continue;
            }
            if (input.confirmedTick < columns.nextScanTick[slot])
            {
                input.results[slot] = AIStateStepResult::sleepUntil(columns.nextScanTick[slot]);
                continue;
            }
            if (input.crate[slot])
            {
                enterPhase(columns, input, slot, AIGuardPhase::GetCrate);
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            if (fact(input.tracksAnchor[slot]) &&
                movedBeyond(priorAnchor, input.currentAnchor[slot], input.anchorMoveThresholdRaw))
            {
                enterPhase(columns, input, slot, AIGuardPhase::Return);
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            input.results[slot] = beginScan(columns, state, input, slot, input.enemyScanIntervalTicks)
                                      ? AIStateStepResult::continueState()
                                      : AIStateStepResult::blocked();
            continue;
        }

        if (phase == AIGuardPhase::Return && !policy.tunnelNetwork && columns.taskAt(slot) != AIGuardOperation::None &&
            !fact(columns.scanPending[slot]) && input.confirmedTick >= columns.nextScanTick[slot])
        {
            input.results[slot] = beginScan(columns, state, input, slot, input.returnScanIntervalTicks)
                                      ? AIStateStepResult::continueState()
                                      : AIStateStepResult::blocked();
            continue;
        }

        if ((phase == AIGuardPhase::Outer || phase == AIGuardPhase::AttackAggressor ||
             (phase == AIGuardPhase::Inner && policy.tunnelNetwork)) &&
            columns.taskAt(slot) == AIGuardOperation::Attack && columns.chaseDeadlineTick[slot] != 0 &&
            input.confirmedTick >= columns.chaseDeadlineTick[slot])
        {
            const AIGuardPhase next =
                phase == AIGuardPhase::Outer
                    ? AIGuardPhase::GetCrate
                    : (phase == AIGuardPhase::AttackAggressor ? policy.aggressorCompletionPhase : AIGuardPhase::Outer);
            input.results[slot] = transition(columns, state, input, slot, next) ? AIStateStepResult::continueState()
                                                                                : AIStateStepResult::blocked();
            continue;
        }

        if (columns.taskAt(slot) == AIGuardOperation::None)
        {
            if (!beginCurrentPhase(columns, state, input, slot))
            {
                input.results[slot] =
                    fact(input.mobile[slot]) ? AIStateStepResult::blocked() : AIStateStepResult::failure();
                continue;
            }
        }
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] bool canExitGuardSoA(const AIGuardSoAColumns& columns,
                                   AIStateId state,
                                   const AIGuardStateSoAKernelInput& input) noexcept;

[[nodiscard]] bool exitGuardSoA(AIGuardSoAColumns& columns,
                                AIStateId state,
                                const AIGuardStateSoAKernelInput& input) noexcept;

} // namespace engine::ai
