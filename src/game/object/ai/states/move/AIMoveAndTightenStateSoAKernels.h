#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include "core/container/container_types.h"

#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{
struct AIMoveAndTightenStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    container::Span<const uint8_t> scheduled{};
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> mobile;
    container::Span<const uint8_t> groundMovement;
    container::Span<const AIFixedPosition> subjectPosition;
    container::Span<const uint32_t> ticksPerSecond;
    container::Span<const PathFeedback> pathFeedback;
    container::Span<const MovementFeedback> movementFeedback;
    container::Span<PathRequestBuffer> pathRequests;
    container::Span<MovementCommandBuffer> movementCommands;
    container::Span<AIStateStepResult> results;
};

namespace detail
{
[[nodiscard]] inline bool hasAlignedTightenSpans(const AIStateFamilySoAStorage& storage,
                                                 const AIMoveAndTightenStateSoAKernelInput& input) noexcept
{
    const size_t n = storage.size();
    return (input.scheduled.empty() || input.scheduled.size() == n) && input.effectivelyDead.size() == n &&
           input.mobile.size() == n && input.groundMovement.size() == n &&
           input.subjectPosition.size() == n && input.ticksPerSecond.size() == n &&
           input.pathFeedback.size() == n && input.movementFeedback.size() == n && input.pathRequests.size() == n &&
           input.movementCommands.size() == n && input.results.size() == n;
}
[[nodiscard]] constexpr bool tightenScheduled(const AIMoveAndTightenStateSoAKernelInput& input, size_t slot) noexcept
{ return input.scheduled.empty() || input.scheduled[slot] != 0; }
[[nodiscard]] inline PathCorrelation tightenCorrelation(ObjectId subject, const AIApproachPathStatePayload& payload) noexcept
{ return {.subject = subject, .stateRequest = payload.request, .generation = payload.generation,
          .sourceOrderRevision = payload.sourceOrderRevision}; }
[[nodiscard]] inline bool emitTightenPath(PathRequestBuffer& output, ObjectId subject,
                                          const AIFixedPosition& start, const AIStateParameters& parameters,
                                          AIApproachPathStatePayload& payload, PathRequestKind kind,
                                          bool quickPath) noexcept
{
    // RefCode clears m_isApproachPath for an AIR locomotor before it reaches
    // computePath(). DirectLine is intentionally limited to New/Patch at the
    // navigation boundary, so normalise only the air-side initial Approach.
    const PathRequestKind requestKind = quickPath && kind == PathRequestKind::Approach
        ? PathRequestKind::New
        : kind;
    const PathRequest request{.correlation = tightenCorrelation(subject, payload), .start = start,
                              .originalGoal = payload.goal, .adjustDestinations = payload.adjustDestinations,
                              .ignoredObstacle = parameters.ignoredObstacle, .surfaceMask = parameters.pathSurfaceMask,
                              .arrivalRadiusRaw = parameters.arrivalRadiusRaw, .kind = requestKind, .currentPath = payload.path,
                              .traversalMode = quickPath ? AIPathTraversalMode::DirectLine : AIPathTraversalMode::Navmesh, .waypointStart = {},
                              .waypointGraphRevision = 0, .waypointHopLimit = 0, .polylineOffset = {},
                              .extraDistanceRaw = 0, .pathThroughUnits = false, .preciseFinalZ = false};
    if (!request.correlation.isValid() || !output.push(request)) return false;
    payload.pathRequestIssued = kind != PathRequestKind::Cancel; return true;
}
[[nodiscard]] inline bool emitTightenMovement(MovementCommandBuffer& output, ObjectId subject, uint64_t tick,
                                              const AIApproachPathStatePayload& payload,
                                              ObjectId ignoredObstacle,
                                              MovementCommandKind kind) noexcept
{
    return output.push({.correlation = tightenCorrelation(subject, payload), .kind = kind, .path = payload.path,
                        .ignoredObstacle = ignoredObstacle,
                        .clearGoal = kind == MovementCommandKind::EndMovement,
                        .preserveUltraAccurateFinalPosition = kind == MovementCommandKind::EndMovement,
                        .allowPathThroughUnits = false, .confirmedTick = tick});
}
} // namespace detail

[[nodiscard]] inline bool enterMoveAndTightenSoA(AIStateFamilySoAStorage& storage,
                                                 const AIMoveAndTightenStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedTightenSpans(storage, input)) return false;
    const auto subjects=storage.subjects(); const auto runtimes=storage.runtimes(); const auto states=storage.payloadStates();
    const auto parameters=storage.parameters(); auto& columns=storage.approachPath();
    for(size_t slot=0;slot<storage.size();++slot){
        if(!detail::tightenScheduled(input,slot)||runtimes[slot].currentState!=AIStateId::MoveAndTighten) continue;
        if(states[slot]!=AIStateId::MoveAndTighten){input.results[slot]=AIStateStepResult::unsupported();continue;}
        const AIStateParameters& p=parameters[slot];
        if(!input.mobile[slot]||!p.hasGoalPosition||p.sourceOrderRevision==0){input.results[slot]=AIStateStepResult::failure();continue;}
        AIApproachPathStatePayload candidate=columns.load(slot); candidate.goal=p.goalPosition;
        candidate.sourceOrderRevision=p.sourceOrderRevision; candidate.generation=1; candidate.repathsRemaining=1;
        candidate.phase=AIMoveToPhase::WaitingForPath; candidate.path={}; candidate.adjustDestinations=false;
        if(!detail::emitTightenPath(input.pathRequests[slot],subjects[slot],input.subjectPosition[slot],p,candidate,PathRequestKind::Approach,!input.groundMovement[slot]))
        {input.results[slot]=AIStateStepResult::unsupported();continue;}
        columns.store(slot,candidate); input.results[slot]=AIStateStepResult::continueState(); }
    return true;
}

[[nodiscard]] inline bool updateMoveAndTightenSoA(AIStateFamilySoAStorage& storage,
                                                  const AIMoveAndTightenStateSoAKernelInput& input) noexcept
{
    if(!detail::hasAlignedTightenSpans(storage,input)) return false;
    const auto subjects=storage.subjects(); const auto runtimes=storage.runtimes(); const auto states=storage.payloadStates();
    const auto parameters=storage.parameters(); auto& columns=storage.approachPath();
    for(size_t slot=0;slot<storage.size();++slot){
        if(!detail::tightenScheduled(input,slot)||runtimes[slot].currentState!=AIStateId::MoveAndTighten) continue;
        if(input.effectivelyDead[slot]){input.results[slot]=AIStateStepResult::transitionTo(AIStateId::Dead);continue;}
        if(states[slot]!=AIStateId::MoveAndTighten){input.results[slot]=AIStateStepResult::unsupported();continue;}
        AIApproachPathStatePayload payload=columns.load(slot); const PathCorrelation expected=detail::tightenCorrelation(subjects[slot],payload);
        if(payload.phase==AIMoveToPhase::WaitingForPath){const PathFeedback& f=input.pathFeedback[slot];
            if(!(f.correlation==expected)||f.status==PathFeedbackStatus::Pending||f.status==PathFeedbackStatus::Delayed)
            {input.results[slot]=AIStateStepResult::continueState();continue;}
            if(f.status==PathFeedbackStatus::Ready){if(!f.path){input.results[slot]=AIStateStepResult::unsupported();continue;}
                AIApproachPathStatePayload c=payload;c.path=f.path;c.pathRequestIssued=false;c.phase=AIMoveToPhase::FollowingPath;c.adjustDestinations=true;
                if(!detail::emitTightenMovement(input.movementCommands[slot],subjects[slot],input.confirmedTick,c,
                                                parameters[slot].ignoredObstacle,
                                                MovementCommandKind::InstallPath))
                {input.results[slot]=AIStateStepResult::unsupported();continue;} columns.store(slot,c);input.results[slot]=AIStateStepResult::continueState();continue;}
            input.results[slot]=f.status==PathFeedbackStatus::Unsupported?AIStateStepResult::unsupported():AIStateStepResult::failure();continue;}
        const MovementFeedback& f=input.movementFeedback[slot]; if(!(f.correlation==expected)){input.results[slot]=AIStateStepResult::continueState();continue;}
        if(isMovementActiveFeedback(f.status)){input.results[slot]=AIStateStepResult::continueState();continue;}
        if(f.status==MovementFeedbackStatus::Completed){input.results[slot]=AIStateStepResult::success();continue;}
        if(f.status==MovementFeedbackStatus::Unsupported){input.results[slot]=AIStateStepResult::unsupported();continue;}
        if(f.status==MovementFeedbackStatus::Cancelled){input.results[slot]=AIStateStepResult::failure();continue;}
        const uint32_t max=std::numeric_limits<uint32_t>::max(); const uint32_t threshold=input.ticksPerSecond[slot]>max/2?max:input.ticksPerSecond[slot]*2;
        if(f.status==MovementFeedbackStatus::Blocked&&f.blockedTicks<threshold){input.results[slot]=AIStateStepResult::continueState();continue;}
        if(payload.repathsRemaining==0){input.results[slot]=AIStateStepResult::failure();continue;}
        AIApproachPathStatePayload c=payload;--c.repathsRemaining;++c.generation;if(c.generation==0)++c.generation;c.adjustDestinations=true;
        c.phase=AIMoveToPhase::WaitingForPath;c.pathRequestIssued=false;
        if(!detail::emitTightenPath(input.pathRequests[slot],subjects[slot],input.subjectPosition[slot],parameters[slot],c,PathRequestKind::Patch,!input.groundMovement[slot]))
        {input.results[slot]=AIStateStepResult::unsupported();continue;}columns.store(slot,c);input.results[slot]=AIStateStepResult::continueState();}
    return true;
}

[[nodiscard]] inline bool canExitMoveAndTightenSoA(const AIStateFamilySoAStorage& storage,
                                                   const AIMoveAndTightenStateSoAKernelInput& input) noexcept
{
    if(!detail::hasAlignedTightenSpans(storage,input))return false;
    const auto states=storage.payloadStates();const auto& columns=storage.approachPath();
    for(size_t s=0;s<storage.size();++s)
    {
        if(!detail::tightenScheduled(input,s)||states[s]!=AIStateId::MoveAndTighten)continue;
        const auto p=columns.load(s);
        if(p.pathRequestIssued&&input.pathRequests[s].count>=input.pathRequests[s].values.size())return false;
        if(input.movementCommands[s].count>=input.movementCommands[s].values.size())return false;
    }
    return true;
}
[[nodiscard]] inline bool exitMoveAndTightenSoA(AIStateFamilySoAStorage& storage,
                                                const AIMoveAndTightenStateSoAKernelInput& input) noexcept
{
    if(!canExitMoveAndTightenSoA(storage,input))return false;
    const auto subjects=storage.subjects();const auto states=storage.payloadStates();const auto parameters=storage.parameters();auto& columns=storage.approachPath();
    for(size_t s=0;s<storage.size();++s)
    {
        if(!detail::tightenScheduled(input,s)||states[s]!=AIStateId::MoveAndTighten)continue;
        auto p=columns.load(s);
        if(p.pathRequestIssued)static_cast<void>(detail::emitTightenPath(input.pathRequests[s],subjects[s],input.subjectPosition[s],parameters[s],p,PathRequestKind::Cancel,!input.groundMovement[s]));
        static_cast<void>(detail::emitTightenMovement(input.movementCommands[s],subjects[s],input.confirmedTick,p,
                                                      parameters[s].ignoredObstacle,
                                                      MovementCommandKind::EndMovement));
        columns.store(s,p);
    }
    return true;
}
} // namespace engine::ai
