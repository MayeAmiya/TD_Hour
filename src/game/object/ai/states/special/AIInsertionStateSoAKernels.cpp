#include "game/object/ai/states/special/AIInsertionStateSoAKernels.h"

namespace engine::ai::insertion_detail
{

bool hasAlignedInputSpans(size_t count, const AIInsertionStateSoAKernelInput& input) noexcept
{
    return (input.scheduled.empty() || input.scheduled.size() == count) && input.subjects.size() == count &&
           input.activeStates.size() == count && input.goalObjects.size() == count &&
           input.goalPositions.size() == count && input.goalPositionValid.size() == count &&
           input.parameters.size() == count && input.effectivelyDead.size() == count &&
           input.mobile.size() == count && input.subjectPositions.size() == count &&
           input.pathFeedback.size() == count && input.movementFeedback.size() == count &&
           input.motionFeedback.size() == count && input.containmentFeedback.size() == count &&
           input.operationFeedback.size() == count && input.motionCommands.size() == count &&
           input.containmentCommands.size() == count && input.operationCommands.size() == count &&
           input.effectCommands.size() == count && input.pathRequests.size() == count &&
           input.movementCommands.size() == count && input.results.size() == count;
}

bool hasAlignedSpans(const AIInsertionStateSoAColumns& columns,
                     const AIInsertionStateSoAKernelInput& input) noexcept
{
    const size_t count = input.activeStates.size();
    return hasAlignedInputSpans(count, input) && columns.requestTick.size() == count &&
           columns.requestSequence.size() == count && columns.rappelTarget.size() == count &&
           columns.rappelTargetIsBuilding.size() == count && columns.rappelDestinationZRaw.size() == count &&
           columns.rappelSpeedRaw.size() == count && columns.rappelPhase.size() == count &&
           columns.combatDropOperation.size() == count && columns.combatDropNextEventSequence.size() == count &&
           columns.combatDropPhase.size() == count && columns.combatDropPath.size() == count &&
           columns.combatDropSourceOrderRevision.size() == count &&
           columns.combatDropPathGeneration.size() == count &&
           columns.combatDropOldPreferredHeightRaw.size() == count &&
           columns.combatDropPathRequestIssued.size() == count &&
           columns.combatDropApproachConfigured.size() == count;
}

const AIInsertionMotionFeedback* motionEvent(const AIInsertionMotionFeedbackBuffer& buffer,
                                             const AIInsertionCorrelation& expected,
                                             bool& ambiguous) noexcept
{
    return uniqueMatch(buffer,
                       [&](const AIInsertionMotionFeedback& value) {
                           return value.correlation == expected && value.kind != AIInsertionMotionFeedbackKind::None;
                       },
                       ambiguous);
}

const AIInsertionContainmentFeedback* containmentEvent(const AIInsertionContainmentFeedbackBuffer& buffer,
                                                       const AIInsertionCorrelation& expected,
                                                       ObjectId building,
                                                       bool& ambiguous) noexcept
{
    return uniqueMatch(buffer,
                       [&](const AIInsertionContainmentFeedback& value) {
                           return value.correlation == expected && value.building == building &&
                                  value.kind != AIInsertionContainmentFeedbackKind::None;
                       },
                       ambiguous);
}

const AIInsertionOperationFeedback* beginEvent(const AIInsertionOperationFeedbackBuffer& buffer,
                                               const AIInsertionCorrelation& expected,
                                               bool& ambiguous) noexcept
{
    return uniqueMatch(buffer,
                       [&](const AIInsertionOperationFeedback& value) {
                           return value.correlation == expected && value.eventSequence == 0 &&
                                  value.kind != AIInsertionOperationFeedbackKind::None;
                       },
                       ambiguous);
}

const AIInsertionOperationFeedback* pollEvent(const AIInsertionOperationFeedbackBuffer& buffer,
                                              const AIInsertionCorrelation& expected,
                                              AIInsertionOperationHandle operation,
                                              uint32_t sequence,
                                              bool& ambiguous) noexcept
{
    return uniqueMatch(buffer,
                       [&](const AIInsertionOperationFeedback& value) {
                           return value.correlation == expected && value.operation == operation &&
                                  value.eventSequence == sequence &&
                                  value.kind != AIInsertionOperationFeedbackKind::None;
                       },
                       ambiguous);
}

void emitMotion(const AIInsertionStateSoAKernelInput& input,
                size_t slot,
                const AIInsertionCorrelation& expected,
                AIInsertionMotionCommandKind kind,
                AIFixedPosition position,
                int64_t verticalSpeedRaw,
                int64_t orientationRaw,
                uint32_t layer,
                int64_t preferredHeightRaw,
                bool ultraAccurate) noexcept
{
    static_cast<void>(input.motionCommands[slot].push({
        .correlation = expected,
        .kind = kind,
        .position = position,
        .verticalSpeedRaw = verticalSpeedRaw,
        .orientationRaw = orientationRaw,
        .preferredHeightRaw = preferredHeightRaw,
        .layer = layer,
        .ultraAccurate = ultraAccurate,
        .confirmedTick = input.confirmedTick,
    }));
}

void emitContainment(const AIInsertionStateSoAKernelInput& input,
                     size_t slot,
                     const AIInsertionCorrelation& expected,
                     AIInsertionContainmentCommandKind kind,
                     ObjectId building,
                     uint8_t maximumEnemiesToKill) noexcept
{
    static_cast<void>(input.containmentCommands[slot].push({
        .correlation = expected,
        .kind = kind,
        .building = building,
        .maximumEnemiesToKill = maximumEnemiesToKill,
        .confirmedTick = input.confirmedTick,
    }));
}

void emitOperation(const AIInsertionStateSoAKernelInput& input,
                   size_t slot,
                   const AIInsertionCorrelation& expected,
                   AIInsertionOperationCommandKind kind,
                   AIInsertionOperationHandle operation,
                   uint32_t eventSequence,
                   ObjectId child,
                   int64_t childRappelSpeedRaw) noexcept
{
    static_cast<void>(input.operationCommands[slot].push({
        .correlation = expected,
        .kind = kind,
        .operation = operation,
        .eventSequence = eventSequence,
        .goal = input.goalObjects[slot],
        .goalPosition = input.goalPositions[slot],
        .child = child,
        .childRappelSpeedRaw = childRappelSpeedRaw,
        .confirmedTick = input.confirmedTick,
    }));
}

void emitEffect(const AIInsertionStateSoAKernelInput& input,
                size_t slot,
                const AIInsertionCorrelation& expected,
                AIInsertionEffectCommandKind kind,
                ObjectId target,
                uint8_t enemiesKilled) noexcept
{
    static_cast<void>(input.effectCommands[slot].push({
        .correlation = expected,
        .kind = kind,
        .target = target,
        .enemiesKilled = enemiesKilled,
        .confirmedTick = input.confirmedTick,
    }));
}

} // namespace engine::ai::insertion_detail

namespace engine::ai
{

bool enterRappelIntoStateSoA(AIInsertionStateSoAColumns columns,
                             const AIInsertionStateSoAKernelInput& input) noexcept
{
    using namespace insertion_detail;
    if (!hasAlignedSpans(columns, input))
        return false;

    for (size_t slot = 0; slot < input.activeStates.size(); ++slot)
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::RappelInto)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::RappelInto);
        bool ambiguous = false;
        const auto* event = motionEvent(input.motionFeedback[slot], expected, ambiguous);
        if (event && event->kind == AIInsertionMotionFeedbackKind::RappelEntryReady && event->canRappel)
        {
            const bool targetBuilding = input.goalObjects[slot] && event->goal == input.goalObjects[slot] &&
                                        event->goalIsStructure && event->goalAlive;
            const size_t required = 2 + static_cast<size_t>(!targetBuilding);
            if (!input.motionCommands[slot].hasCapacity(required))
                return false;
        }
    }

    for (size_t slot = 0; slot < input.activeStates.size(); ++slot)
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::RappelInto)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::RappelInto);
        if (!expected.isValid())
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        bool ambiguous = false;
        const auto* event = motionEvent(input.motionFeedback[slot], expected, ambiguous);
        if (ambiguous)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (!event)
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        if (event->kind == AIInsertionMotionFeedbackKind::RappelEntryRejected)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (event->kind != AIInsertionMotionFeedbackKind::RappelEntryReady)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (!event->canRappel)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }

        const bool targetBuilding = input.goalObjects[slot] && event->goal == input.goalObjects[slot] &&
                                    event->goalIsStructure && event->goalAlive;
        const int64_t desired = positiveMagnitude(event->desiredSpeedRaw);
        const int64_t maximum = positiveMagnitude(event->maximumRappelSpeedRaw);
        const int64_t rate = -(desired < maximum ? desired : maximum);
        columns.rappelTarget[slot] = targetBuilding ? input.goalObjects[slot] : INVALID_OBJECT_ID;
        columns.rappelTargetIsBuilding[slot] = static_cast<uint8_t>(targetBuilding);
        columns.rappelDestinationZRaw[slot] = targetBuilding ? event->buildingTopRaw : event->layerHeightRaw;
        columns.rappelSpeedRaw[slot] = rate;
        columns.rappelPhase[slot] = AIRappelInsertionPhase::Descending;
        emitMotion(input, slot, expected, AIInsertionMotionCommandKind::SetRappelling);
        emitMotion(input, slot, expected, AIInsertionMotionCommandKind::ResetDynamicPhysics);
        if (!targetBuilding)
            emitMotion(input, slot, expected, AIInsertionMotionCommandKind::SetLayer, {}, 0, 0, event->destinationLayer);
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

bool canExitRappelIntoStateSoA(const AIInsertionStateSoAColumns& columns,
                              const AIInsertionStateSoAKernelInput& input) noexcept
{
    using namespace insertion_detail;
    if (!hasAlignedSpans(columns, input))
        return false;
    for (size_t slot = 0; slot < input.activeStates.size(); ++slot)
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::RappelInto ||
            columns.rappelPhase[slot] == AIRappelInsertionPhase::Inactive)
            continue;
        const size_t containmentRequired =
            static_cast<size_t>(columns.rappelPhase[slot] == AIRappelInsertionPhase::AwaitingBuildingResolution);
        if (!correlation(columns, input, slot, AIStateId::RappelInto).isValid() ||
            !input.motionCommands[slot].hasCapacity(2) ||
            !input.containmentCommands[slot].hasCapacity(containmentRequired))
            return false;
    }
    return true;
}

bool exitRappelIntoStateSoA(AIInsertionStateSoAColumns columns,
                            const AIInsertionStateSoAKernelInput& input) noexcept
{
    using namespace insertion_detail;
    if (!canExitRappelIntoStateSoA(columns, input))
        return false;
    for (size_t slot = 0; slot < input.activeStates.size(); ++slot)
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::RappelInto ||
            columns.rappelPhase[slot] == AIRappelInsertionPhase::Inactive)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::RappelInto);
        if (columns.rappelPhase[slot] == AIRappelInsertionPhase::AwaitingBuildingResolution)
            emitContainment(input,
                            slot,
                            expected,
                            AIInsertionContainmentCommandKind::CancelBuildingLanding,
                            columns.rappelTarget[slot]);
        emitMotion(input, slot, expected, AIInsertionMotionCommandKind::ClearRappelling);
        emitMotion(input, slot, expected, AIInsertionMotionCommandKind::RestoreFastDesiredSpeed);
        columns.rappelTarget[slot] = INVALID_OBJECT_ID;
        columns.rappelTargetIsBuilding[slot] = 0;
        columns.rappelDestinationZRaw[slot] = 0;
        columns.rappelSpeedRaw[slot] = 0;
        columns.rappelPhase[slot] = AIRappelInsertionPhase::Inactive;
    }
    return true;
}

bool enterCombatDropStateSoA(AIInsertionStateSoAColumns columns,
                             const AIInsertionStateSoAKernelInput& input) noexcept
{
    using namespace insertion_detail;
    if (!hasAlignedSpans(columns, input))
        return false;
    for (size_t slot = 0; slot < input.activeStates.size(); ++slot)
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::CombatDrop)
            continue;
        const auto expected = correlation(
            columns, input, slot, AIStateId::CombatDrop);
        bool ambiguous = false;
        const AIInsertionMotionFeedback* event = motionEvent(
            input.motionFeedback[slot], expected, ambiguous);
        if (!ambiguous && event &&
            event->kind ==
                AIInsertionMotionFeedbackKind::CombatDropApproachReady &&
            (!input.motionCommands[slot].hasCapacity(1) ||
             !input.pathRequests[slot].hasCapacity(1)))
            return false;
    }
    for (size_t slot = 0; slot < input.activeStates.size(); ++slot)
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::CombatDrop)
            continue;
        const auto expected = correlation(columns, input, slot, AIStateId::CombatDrop);
        if (!expected.isValid() || input.goalPositionValid[slot] == 0 ||
            input.parameters[slot].sourceOrderRevision == 0 ||
            input.parameters[slot].pathSurfaceMask == 0)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (input.effectivelyDead[slot] != 0 || input.mobile[slot] == 0)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        bool ambiguous = false;
        const AIInsertionMotionFeedback* event = motionEvent(
            input.motionFeedback[slot], expected, ambiguous);
        if (ambiguous)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (!event)
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        if (event->kind !=
                AIInsertionMotionFeedbackKind::CombatDropApproachReady ||
            (input.goalObjects[slot] &&
             (event->goal != input.goalObjects[slot] || !event->goalAlive)))
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        columns.combatDropOperation[slot] = {};
        columns.combatDropNextEventSequence[slot] = 0;
        columns.combatDropPath[slot] = {};
        columns.combatDropSourceOrderRevision[slot] =
            input.parameters[slot].sourceOrderRevision;
        columns.combatDropPathGeneration[slot] = 1;
        columns.combatDropOldPreferredHeightRaw[slot] =
            event->previousPreferredHeightRaw;
        columns.combatDropPathRequestIssued[slot] = 0;
        columns.combatDropApproachConfigured[slot] = 1;
        columns.combatDropPhase[slot] =
            AICombatDropInsertionPhase::ApproachWaitingForPath;
        emitMotion(input, slot, expected,
                   AIInsertionMotionCommandKind::ConfigureCombatDropApproach,
                   {}, 0, 0, 0, event->approachPreferredHeightRaw, true);
        if (!emitCombatDropPathRequest(
                columns, input, slot, PathRequestKind::New))
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

bool canExitCombatDropStateSoA(const AIInsertionStateSoAColumns& columns,
                               const AIInsertionStateSoAKernelInput& input) noexcept
{
    using namespace insertion_detail;
    if (!hasAlignedSpans(columns, input))
        return false;
    for (size_t slot = 0; slot < input.activeStates.size(); ++slot)
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::CombatDrop)
            continue;
        const bool approach =
            columns.combatDropPhase[slot] ==
                AICombatDropInsertionPhase::ApproachWaitingForPath ||
            columns.combatDropPhase[slot] ==
                AICombatDropInsertionPhase::ApproachFollowingPath;
        const bool operation =
            columns.combatDropPhase[slot] ==
                AICombatDropInsertionPhase::BeginPending ||
            columns.combatDropPhase[slot] ==
                AICombatDropInsertionPhase::PollPending;
        const size_t pathRequired = static_cast<size_t>(
            approach && columns.combatDropPathRequestIssued[slot] != 0);
        const size_t movementRequired = static_cast<size_t>(approach);
        const size_t motionRequired = static_cast<size_t>(
            columns.combatDropApproachConfigured[slot] != 0);
        if ((approach || operation) &&
            !correlation(columns, input, slot,
                         AIStateId::CombatDrop).isValid())
            return false;
        if (approach &&
            !combatDropPathCorrelation(columns, input, slot).isValid())
            return false;
        if (!input.pathRequests[slot].hasCapacity(pathRequired) ||
            !input.movementCommands[slot].hasCapacity(movementRequired) ||
            !input.motionCommands[slot].hasCapacity(motionRequired) ||
            !input.operationCommands[slot].hasCapacity(
                static_cast<size_t>(operation)))
            return false;
    }
    return true;
}

bool exitCombatDropStateSoA(AIInsertionStateSoAColumns columns,
                            const AIInsertionStateSoAKernelInput& input) noexcept
{
    using namespace insertion_detail;
    if (!canExitCombatDropStateSoA(columns, input))
        return false;
    for (size_t slot = 0; slot < input.activeStates.size(); ++slot)
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::CombatDrop)
            continue;
        const bool approach =
            columns.combatDropPhase[slot] ==
                AICombatDropInsertionPhase::ApproachWaitingForPath ||
            columns.combatDropPhase[slot] ==
                AICombatDropInsertionPhase::ApproachFollowingPath;
        const bool operation =
            columns.combatDropPhase[slot] ==
                AICombatDropInsertionPhase::BeginPending ||
            columns.combatDropPhase[slot] ==
                AICombatDropInsertionPhase::PollPending;
        const auto expected = correlation(
            columns, input, slot, AIStateId::CombatDrop);
        if (approach)
        {
            if (columns.combatDropPathRequestIssued[slot] != 0)
                static_cast<void>(emitCombatDropPathRequest(
                    columns, input, slot, PathRequestKind::Cancel));
            static_cast<void>(emitCombatDropMovement(
                columns, input, slot, MovementCommandKind::EndMovement));
        }
        if (columns.combatDropApproachConfigured[slot] != 0)
        {
            emitMotion(
                input, slot, expected,
                AIInsertionMotionCommandKind::RestoreCombatDropApproach,
                {}, 0, 0, 0,
                columns.combatDropOldPreferredHeightRaw[slot], false);
        }
        if (operation)
        {
            emitOperation(input,
                          slot,
                          expected,
                          AIInsertionOperationCommandKind::Cancel,
                          columns.combatDropOperation[slot],
                          columns.combatDropNextEventSequence[slot]);
        }
        columns.combatDropOperation[slot] = {};
        columns.combatDropNextEventSequence[slot] = 0;
        columns.combatDropPhase[slot] = AICombatDropInsertionPhase::Inactive;
        columns.combatDropPath[slot] = {};
        columns.combatDropSourceOrderRevision[slot] = 0;
        columns.combatDropPathGeneration[slot] = 1;
        columns.combatDropOldPreferredHeightRaw[slot] = 0;
        columns.combatDropPathRequestIssued[slot] = 0;
        columns.combatDropApproachConfigured[slot] = 0;
    }
    return true;
}

} // namespace engine::ai
