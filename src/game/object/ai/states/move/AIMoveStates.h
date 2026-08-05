#pragma once

#include <limits>
#include <variant>

#include "game/object/ai/runtime/AIStateData.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

[[nodiscard]] inline PathCorrelation moveCorrelation(const AIStateContext& context,
                                                     const AIMoveToStatePayload& payload) noexcept
{
    return {
        .subject = context.subject,
        .stateRequest = payload.request,
        .generation = payload.generation,
        .sourceOrderRevision = payload.sourceOrderRevision,
    };
}

[[nodiscard]] inline bool matchesMoveFeedback(const PathCorrelation& expected, const PathCorrelation& actual) noexcept
{
    return expected == actual;
}

[[nodiscard]] inline bool emitPathRequest(AIStateData& data,
                                          const AIStateContext& context,
                                          PathRequestKind kind) noexcept
{
    auto* payload = std::get_if<AIMoveToStatePayload>(&data.activePayload);
    if (!payload || !context.pathServices.pathRequests)
        return false;

    const PathRequest request{
        .correlation = moveCorrelation(context, *payload),
        .start = context.subjectPosition,
        .originalGoal = data.parameters.goalPosition,
        .adjustDestinations = payload->adjustDestinations,
        .ignoredObstacle = data.parameters.ignoredObstacle,
        .surfaceMask = data.parameters.pathSurfaceMask,
        .arrivalRadiusRaw = data.parameters.arrivalRadiusRaw,
        .kind = kind,
        .currentPath = payload->path,
        .traversalMode = AIPathTraversalMode::Navmesh,
        .waypointStart = {},
        .waypointGraphRevision = 0,
        .waypointHopLimit = 0,
        .polylineOffset = {},
        .extraDistanceRaw = 0,
        .pathThroughUnits = false,
        .preciseFinalZ = false,
    };
    if (!request.correlation.isValid() || !context.pathServices.pathRequests->push(request))
        return false;
    payload->pathRequestIssued = kind != PathRequestKind::Cancel;
    return true;
}

[[nodiscard]] inline bool emitInstallPath(const AIStateContext& context,
                                          const AIMoveToStatePayload& payload,
                                          ObjectId ignoredObstacle) noexcept
{
    if (!context.pathServices.movementCommands || !payload.path)
        return false;
    return context.pathServices.movementCommands->push({
        .correlation = moveCorrelation(context, payload),
        .kind = MovementCommandKind::InstallPath,
        .path = payload.path,
        .ignoredObstacle = ignoredObstacle,
        .clearGoal = false,
        .preserveUltraAccurateFinalPosition = false,
        .confirmedTick = context.confirmedTick,
    });
}

[[nodiscard]] inline bool beginRepath(AIStateData& data, const AIStateContext& context) noexcept
{
    auto* payload = std::get_if<AIMoveToStatePayload>(&data.activePayload);
    if (!payload)
        return false;
    ++payload->generation;
    if (payload->generation == 0)
        ++payload->generation;
    payload->phase = AIMoveToPhase::WaitingForPath;
    payload->pathRequestIssued = false;
    return emitPathRequest(data, context, PathRequestKind::Patch);
}

[[nodiscard]] inline AIStateStepResult updateMoveTo(AIStateData& data, const AIStateContext& context)
{
    if (context.effectivelyDead)
        return AIStateStepResult::transitionTo(AIStateId::Dead);
    if (!context.mobile)
        return AIStateStepResult::failure();

    auto* payload = std::get_if<AIMoveToStatePayload>(&data.activePayload);
    if (!payload)
        return AIStateStepResult::unsupported();

    if (data.parameters.goalObject)
    {
        if (!context.moveTargetValid)
            return AIStateStepResult::failure();
        payload->resolvedGoal = context.resolvedMoveTarget;
    }

    const PathCorrelation expected = moveCorrelation(context, *payload);
    if (payload->phase == AIMoveToPhase::WaitingForPath)
    {
        const PathFeedback* feedback = context.pathServices.pathFeedback;
        if (!feedback || !matchesMoveFeedback(expected, feedback->correlation))
            return AIStateStepResult::continueState();

        switch (feedback->status)
        {
        case PathFeedbackStatus::Pending:
        case PathFeedbackStatus::Delayed:
            return AIStateStepResult::continueState();
        case PathFeedbackStatus::Ready:
            if (!feedback->path)
                return AIStateStepResult::unsupported();
            payload->pathRequestIssued = false;
            payload->path = feedback->path;
            payload->adjustedGoal = feedback->adjustedGoal;
            payload->adjustedLayer = feedback->adjustedLayer;
            payload->phase = AIMoveToPhase::FollowingPath;
            if (!emitInstallPath(context, *payload, data.parameters.ignoredObstacle))
                return AIStateStepResult::unsupported();
            return AIStateStepResult::continueState();
        case PathFeedbackStatus::NoPath:
        case PathFeedbackStatus::Cancelled:
            payload->pathRequestIssued = false;
            return AIStateStepResult::failure();
        case PathFeedbackStatus::Unsupported:
            payload->pathRequestIssued = false;
            return AIStateStepResult::unsupported();
        }
    }

    const MovementFeedback* feedback = context.pathServices.movementFeedback;
    if (!feedback || !matchesMoveFeedback(expected, feedback->correlation))
        return AIStateStepResult::continueState();

    switch (feedback->status)
    {
    case MovementFeedbackStatus::Started:
    case MovementFeedbackStatus::Moving:
        return AIStateStepResult::continueState();
    case MovementFeedbackStatus::Completed:
        return AIStateStepResult::success();
    case MovementFeedbackStatus::Blocked:
    {
        const uint32_t maximum = std::numeric_limits<uint32_t>::max();
        const uint32_t repathTicks = context.ticksPerSecond > maximum / 2 ? maximum : context.ticksPerSecond * 2;
        if (feedback->blockedTicks < repathTicks)
            return AIStateStepResult::continueState();
        return beginRepath(data, context) ? AIStateStepResult::continueState() : AIStateStepResult::unsupported();
    }
    case MovementFeedbackStatus::Stuck:
        return beginRepath(data, context) ? AIStateStepResult::continueState() : AIStateStepResult::unsupported();
    case MovementFeedbackStatus::Cancelled:
        return AIStateStepResult::failure();
    case MovementFeedbackStatus::Unsupported:
        return AIStateStepResult::unsupported();
    }
    return AIStateStepResult::unsupported();
}

[[nodiscard]] inline AIStateStepResult enterMoveTo(AIStateData& data, const AIStateContext& context)
{
    if (context.effectivelyDead)
        return AIStateStepResult::transitionTo(AIStateId::Dead);
    if (!context.mobile)
        return AIStateStepResult::failure();

    auto* payload = std::get_if<AIMoveToStatePayload>(&data.activePayload);
    if (!payload || !context.subject || data.parameters.sourceOrderRevision == 0)
        return AIStateStepResult::unsupported();

    payload->sourceOrderRevision = data.parameters.sourceOrderRevision;
    payload->adjustDestinations = data.parameters.adjustDestinations;
    if (data.parameters.goalObject)
    {
        if (!context.moveTargetValid)
            return AIStateStepResult::failure();
        data.parameters.goalPosition = context.resolvedMoveTarget;
        data.parameters.hasGoalPosition = true;
    }
    else
    {
        if (!data.parameters.hasGoalPosition)
            return AIStateStepResult::failure();
    }
    payload->resolvedGoal = data.parameters.goalPosition;

    return emitPathRequest(data, context, PathRequestKind::New) ? AIStateStepResult::continueState()
                                                                : AIStateStepResult::unsupported();
}

inline void exitMoveTo(AIStateData& data, const AIStateContext& context) noexcept
{
    auto* payload = std::get_if<AIMoveToStatePayload>(&data.activePayload);
    if (!payload)
        return;

    if (payload->pathRequestIssued && context.pathServices.pathRequests)
    {
        static_cast<void>(emitPathRequest(data, context, PathRequestKind::Cancel));
    }
    if (context.pathServices.movementCommands)
    {
        static_cast<void>(context.pathServices.movementCommands->push({
            .correlation = moveCorrelation(context, *payload),
            .kind = MovementCommandKind::EndMovement,
            .path = payload->path,
            .clearGoal = payload->adjustDestinations,
            .preserveUltraAccurateFinalPosition = true,
            .confirmedTick = context.confirmedTick,
        }));
    }
}

} // namespace engine::ai
