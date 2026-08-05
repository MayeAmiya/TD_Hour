#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include "core/container/container_types.h"

#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

struct AIWaypointStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    uint32_t maximumWaypointHops = 1024;
    container::Span<const uint8_t> scheduled{};
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> mobile;
    container::Span<const uint8_t> groundMovement;
    container::Span<const uint8_t> projectile;
    container::Span<const AIFixedPosition> subjectPosition;
    container::Span<const AIFixedPosition> groupOffset;
    container::Span<const uint32_t> ticksPerSecond;
    container::Span<const uint32_t> branchChoice;
    container::Span<const AITeamHandle> teamProgressTeam;
    container::Span<const AIWaypointHandle> teamProgressCurrent;
    container::Span<const uint64_t> teamProgressRevision;
    const AIWaypointGraphResolver* waypoints = nullptr;
    container::Span<const PathFeedback> pathFeedback;
    container::Span<const MovementFeedback> movementFeedback;
    container::Span<PathRequestBuffer> pathRequests;
    container::Span<MovementCommandBuffer> movementCommands;
    container::Span<AIWaypointTeamProgressBuffer> teamProgressRequests;
    container::Span<AIWaypointCompletionBuffer> completions;
    container::Span<AIStateStepResult> results;
};

namespace detail
{

[[nodiscard]] constexpr bool isWaypointState(AIStateId state) noexcept
{
    return state == AIStateId::FollowWaypointPathAsTeam ||
           state == AIStateId::FollowWaypointPathAsIndividuals ||
           state == AIStateId::FollowWaypointPathAsTeamExact ||
           state == AIStateId::FollowWaypointPathAsIndividualsExact ||
           state == AIStateId::AttackFollowWaypointPathAsIndividuals ||
           state == AIStateId::AttackFollowWaypointPathAsTeam;
}
[[nodiscard]] constexpr bool waypointTeamState(AIStateId state) noexcept
{
    return state == AIStateId::FollowWaypointPathAsTeam ||
           state == AIStateId::FollowWaypointPathAsTeamExact ||
           state == AIStateId::AttackFollowWaypointPathAsTeam;
}
[[nodiscard]] constexpr bool waypointExactState(AIStateId state) noexcept
{
    return state == AIStateId::FollowWaypointPathAsTeamExact ||
           state == AIStateId::FollowWaypointPathAsIndividualsExact;
}
[[nodiscard]] constexpr bool waypointScheduled(const AIWaypointStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}
[[nodiscard]] constexpr bool waypointFact(uint8_t value) noexcept { return value != 0; }

[[nodiscard]] constexpr int64_t waypointSaturatingAdd(int64_t left, int64_t right) noexcept
{
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
        return std::numeric_limits<int64_t>::max();
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
        return std::numeric_limits<int64_t>::min();
    return left + right;
}

[[nodiscard]] inline bool hasAlignedWaypointSpans(const AIStateFamilySoAStorage& storage,
                                                  const AIWaypointStateSoAKernelInput& input) noexcept
{
    const size_t count = storage.size();
    return input.waypoints != nullptr && (input.scheduled.empty() || input.scheduled.size() == count) &&
           input.effectivelyDead.size() == count && input.mobile.size() == count &&
           input.groundMovement.size() == count && input.projectile.size() == count &&
           input.subjectPosition.size() == count && input.groupOffset.size() == count &&
           input.ticksPerSecond.size() == count && input.branchChoice.size() == count &&
           input.teamProgressTeam.size() == count && input.teamProgressCurrent.size() == count &&
           input.teamProgressRevision.size() == count && input.pathFeedback.size() == count &&
           input.movementFeedback.size() == count && input.pathRequests.size() == count &&
           input.movementCommands.size() == count && input.teamProgressRequests.size() == count &&
           input.completions.size() == count && input.results.size() == count;
}

struct WaypointPayloadStoreGuard final
{
    AIWaypointPathSoAColumns& columns;
    size_t slot;
    AIWaypointPathStatePayload& payload;
    ~WaypointPayloadStoreGuard() { columns.store(slot, payload); }
};

[[nodiscard]] inline PathCorrelation waypointCorrelation(ObjectId subject,
                                                          const AIWaypointPathStatePayload& payload) noexcept
{
    return {.subject = subject,
            .stateRequest = payload.request,
            .generation = payload.generation,
            .sourceOrderRevision = payload.sourceOrderRevision};
}

[[nodiscard]] inline bool waypointOutputAvailable(const PathRequestBuffer& paths,
                                                   const AIWaypointTeamProgressBuffer& progress,
                                                   bool needsProgress) noexcept
{
    return paths.count < paths.values.size() && (!needsProgress || progress.count < progress.values.size());
}

[[nodiscard]] inline bool emitWaypointPath(PathRequestBuffer& output,
                                           ObjectId subject,
                                           const AIFixedPosition& start,
                                           const AIStateParameters& parameters,
                                           AIWaypointPathStatePayload& payload,
                                           PathRequestKind kind,
                                           bool quickPath) noexcept
{
    const PathRequest request{
        .correlation = waypointCorrelation(subject, payload),
        .start = start,
        .originalGoal = payload.goal,
        .adjustDestinations = payload.adjustDestinations,
        .ignoredObstacle = parameters.ignoredObstacle,
        .surfaceMask = parameters.pathSurfaceMask,
        .arrivalRadiusRaw = parameters.arrivalRadiusRaw,
        .kind = kind,
        .currentPath = payload.path,
        .traversalMode = payload.exactPolyline ? AIPathTraversalMode::WaypointPolyline
                                               : quickPath ? AIPathTraversalMode::DirectLine
                                                           : AIPathTraversalMode::Navmesh,
        .waypointStart = payload.exactPolyline ? payload.current : AIWaypointHandle{},
        .waypointGraphRevision = payload.exactPolyline ? payload.graphRevision : 0,
        .waypointHopLimit = payload.exactPolyline ? payload.waypointHopLimit : 0,
        .polylineOffset = payload.exactPolyline ? payload.groupOffset : AIFixedPosition{},
        .extraDistanceRaw = payload.extraDistanceRaw,
        .pathThroughUnits = payload.exactPolyline,
        .preciseFinalZ = payload.preciseFinalZ,
    };
    if (!request.correlation.isValid() || !output.push(request))
        return false;
    payload.pathRequestIssued = kind != PathRequestKind::Cancel;
    return true;
}

[[nodiscard]] inline bool emitWaypointInstall(MovementCommandBuffer& output,
                                              ObjectId subject,
                                              uint64_t tick,
                                              const AIWaypointPathStatePayload& payload,
                                              ObjectId ignoredObstacle,
                                              int64_t groupSpeedRaw) noexcept
{
    return payload.path && output.push({.correlation = waypointCorrelation(subject, payload),
                                        .kind = MovementCommandKind::InstallPath,
                                        .path = payload.path,
                                        .ignoredObstacle = ignoredObstacle,
                                        .speedLimitRaw = payload.moveAsTeam
                                            ? groupSpeedRaw : 0,
                                        .extraDistanceRaw =
                                            payload.extraDistanceRaw,
                                        .clearGoal = false,
                                        .preserveUltraAccurateFinalPosition = false,
                                        .allowPathThroughUnits =
                                            payload.exactPolyline,
                                        .confirmedTick = tick});
}

[[nodiscard]] inline bool prepareWaypointGoal(const AIWaypointStateSoAKernelInput& input,
                                              const AIStateParameters& parameters,
                                              AIWaypointPathStatePayload& payload,
                                              size_t slot) noexcept
{
    const AIWaypointQuery query = input.waypoints->node(payload.current, payload.graphRevision);
    if (query.status != AIWaypointQueryStatus::Node)
        return false;
    payload.goal = query.node.position;
    if (payload.moveAsTeam)
    {
        payload.goal.xRaw = waypointSaturatingAdd(payload.goal.xRaw, payload.groupOffset.xRaw);
        payload.goal.yRaw = waypointSaturatingAdd(payload.goal.yRaw, payload.groupOffset.yRaw);
    }
    payload.extraDistanceRaw = query.node.lookAheadDistanceRaw;
    payload.adjustDestinations = !payload.exactPolyline && query.node.linkCount == 0 &&
                                 parameters.adjustDestinations && waypointFact(input.groundMovement[slot]);
    payload.preciseFinalZ = query.node.linkCount == 0 && waypointFact(input.projectile[slot]);
    payload.phase = AIMoveToPhase::WaitingForPath;
    payload.path = {};
    payload.pathRequestIssued = false;
    return true;
}

[[nodiscard]] inline AIStateStepResult completeWaypointPath(AIWaypointPathStatePayload& payload,
                                                            AIWaypointHandle terminal) noexcept
{
    payload.completionTerminal = terminal;
    payload.completionPending = true;
    return AIStateStepResult::success();
}

[[nodiscard]] inline AIStateStepResult advanceOrdinaryWaypoint(
    const AIWaypointStateSoAKernelInput& input,
    ObjectId subject,
    const AIStateParameters& parameters,
    AIWaypointPathStatePayload& payload,
    size_t slot) noexcept
{
    const AIWaypointQuery current = input.waypoints->node(payload.current, payload.graphRevision);
    if (current.status != AIWaypointQueryStatus::Node)
        return AIStateStepResult::unsupported();
    if (payload.moveAsTeam)
    {
        if (payload.awaitingTeamProgress)
            return AIStateStepResult::continueState();
        if (input.teamProgressRequests[slot].count >= input.teamProgressRequests[slot].values.size())
            return AIStateStepResult::unsupported();
        static_cast<void>(input.teamProgressRequests[slot].push({.correlation = waypointCorrelation(subject, payload),
                                                                 .team = payload.team,
                                                                 .subject = subject,
                                                                 .arrived = payload.current,
                                                                 .expectedRevision = payload.teamRevision}));
        payload.awaitingTeamProgress = true;
        return AIStateStepResult::continueState();
    }
    if (current.node.linkCount == 0)
        return completeWaypointPath(payload, payload.current);

    const uint32_t selected = input.branchChoice[slot] % current.node.linkCount;
    const AIWaypointLinkQuery link = input.waypoints->link(payload.current, payload.graphRevision, selected);
    if (link.status != AIWaypointQueryStatus::Node || !link.target)
        return AIStateStepResult::unsupported();
    AIWaypointPathStatePayload candidate = payload;
    candidate.prior = candidate.current;
    candidate.current = link.target;
    ++candidate.generation;
    if (candidate.generation == 0) ++candidate.generation;
    if (!prepareWaypointGoal(input, parameters, candidate, slot))
        return AIStateStepResult::unsupported();
    if (!waypointOutputAvailable(input.pathRequests[slot], input.teamProgressRequests[slot], false))
        return AIStateStepResult::unsupported();
    if (!emitWaypointPath(input.pathRequests[slot],
                          subject,
                          input.subjectPosition[slot],
                           parameters,
                           candidate,
                           PathRequestKind::New,
                           !waypointFact(input.groundMovement[slot])))
        return AIStateStepResult::unsupported();
    payload = candidate;
    return AIStateStepResult::continueState();
}

[[nodiscard]] inline AIWaypointHandle exactTerminal(const AIWaypointStateSoAKernelInput& input,
                                                    AIWaypointHandle start,
                                                    uint64_t revision) noexcept
{
    AIWaypointHandle current = start;
    for (uint32_t hop = 0; hop < input.maximumWaypointHops; ++hop)
    {
        const AIWaypointQuery node = input.waypoints->node(current, revision);
        if (node.status != AIWaypointQueryStatus::Node)
            return {};
        if (node.node.linkCount == 0)
            return current;
        const AIWaypointLinkQuery link = input.waypoints->link(current, revision, 0);
        if (link.status != AIWaypointQueryStatus::Node || !link.target || link.target == current)
            return {};
        current = link.target;
    }
    // RefCode truncates an overlong/cyclic authored exact path at its
    // WAYPOINT_PATH_LIMIT and still runs the retained prefix. Do not turn a
    // malformed tail into a whole-order failure after valid points exist.
    return current;
}

} // namespace detail

[[nodiscard]] inline bool enterWaypointPathSoA(AIStateFamilySoAStorage& storage,
                                               AIStateId state,
                                               const AIWaypointStateSoAKernelInput& input) noexcept
{
    if (!detail::isWaypointState(state) || !detail::hasAlignedWaypointSpans(storage, input)) return false;
    const auto subjects = storage.subjects(); const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates(); const auto parameters = storage.parameters();
    auto& columns = storage.waypointPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::waypointScheduled(input, slot) || runtimes[slot].currentState != state) continue;
        if (detail::waypointFact(input.effectivelyDead[slot])) { input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead); continue; }
        if (!detail::waypointFact(input.mobile[slot])) { input.results[slot] = AIStateStepResult::failure(); continue; }
        if (payloadStates[slot] != state) { input.results[slot] = AIStateStepResult::unsupported(); continue; }
        const AIStateParameters& parameter = parameters[slot];
        AIWaypointPathStatePayload payload = columns.load(slot);
        payload.moveAsTeam = detail::waypointTeamState(state); payload.exactPolyline = detail::waypointExactState(state);
        payload.team = parameter.waypointTeam; payload.graphRevision = parameter.waypointGraphRevision;
        payload.waypointHopLimit = input.maximumWaypointHops;
        payload.sourceOrderRevision = parameter.sourceOrderRevision; payload.groupOffset = input.groupOffset[slot];
        payload.teamRevision = payload.moveAsTeam && input.teamProgressTeam[slot] == payload.team
                                   ? input.teamProgressRevision[slot]
                                   : 0;
        payload.current = parameter.waypoint;
        if (!payload.current && payload.moveAsTeam) payload.current = input.teamProgressCurrent[slot];
        if (!subjects[slot] || !payload.current || payload.graphRevision == 0 || payload.sourceOrderRevision == 0 ||
            (payload.moveAsTeam && !payload.team))
        { input.results[slot] = AIStateStepResult::failure(); continue; }
        if (!detail::prepareWaypointGoal(input, parameter, payload, slot))
        { input.results[slot] = AIStateStepResult::unsupported(); continue; }
        if (payload.exactPolyline)
        {
            payload.prior = detail::exactTerminal(input, payload.current, payload.graphRevision);
            if (!payload.prior) { input.results[slot] = AIStateStepResult::unsupported(); continue; }
            payload.adjustDestinations = false; payload.extraDistanceRaw = 0;
        }
        if (detail::emitWaypointPath(input.pathRequests[slot], subjects[slot], input.subjectPosition[slot], parameter, payload, PathRequestKind::New, !detail::waypointFact(input.groundMovement[slot])))
        {
            columns.store(slot, payload);
            input.results[slot] = AIStateStepResult::continueState();
        }
        else input.results[slot] = AIStateStepResult::unsupported();
    }
    return true;
}

[[nodiscard]] inline bool updateWaypointPathSoA(AIStateFamilySoAStorage& storage,
                                                AIStateId state,
                                                const AIWaypointStateSoAKernelInput& input) noexcept
{
    if (!detail::isWaypointState(state) || !detail::hasAlignedWaypointSpans(storage, input)) return false;
    const auto subjects = storage.subjects(); const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates(); const auto parameters = storage.parameters();
    auto& columns = storage.waypointPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::waypointScheduled(input, slot) || runtimes[slot].currentState != state) continue;
        if (detail::waypointFact(input.effectivelyDead[slot])) { input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead); continue; }
        if (!detail::waypointFact(input.mobile[slot])) { input.results[slot] = AIStateStepResult::failure(); continue; }
        if (payloadStates[slot] != state) { input.results[slot] = AIStateStepResult::unsupported(); continue; }
        const AIStateParameters& parameter = parameters[slot];
        AIWaypointPathStatePayload payload = columns.load(slot);
        const detail::WaypointPayloadStoreGuard guard{columns, slot, payload};
        if (!payload.current)
        {
            AIWaypointPathStatePayload candidate = payload;
            candidate.moveAsTeam = detail::waypointTeamState(state); candidate.exactPolyline = detail::waypointExactState(state);
            candidate.team = parameter.waypointTeam; candidate.graphRevision = parameter.waypointGraphRevision;
            candidate.waypointHopLimit = input.maximumWaypointHops;
            candidate.sourceOrderRevision = parameter.sourceOrderRevision; candidate.groupOffset = input.groupOffset[slot];
            candidate.teamRevision = candidate.moveAsTeam && input.teamProgressTeam[slot] == candidate.team
                                         ? input.teamProgressRevision[slot] : 0;
            candidate.current = parameter.waypoint;
            if (!candidate.current && candidate.moveAsTeam) candidate.current = input.teamProgressCurrent[slot];
            if (!candidate.current || candidate.graphRevision == 0 || candidate.sourceOrderRevision == 0 ||
                (candidate.moveAsTeam && !candidate.team) || !detail::prepareWaypointGoal(input, parameter, candidate, slot))
            { input.results[slot] = AIStateStepResult::failure(); continue; }
            if (candidate.exactPolyline)
            {
                candidate.prior = detail::exactTerminal(input, candidate.current, candidate.graphRevision);
                if (!candidate.prior) { input.results[slot] = AIStateStepResult::unsupported(); continue; }
                candidate.adjustDestinations = false; candidate.extraDistanceRaw = 0;
            }
            if (!detail::emitWaypointPath(input.pathRequests[slot], subjects[slot], input.subjectPosition[slot], parameter, candidate, PathRequestKind::New, !detail::waypointFact(input.groundMovement[slot])))
            { input.results[slot] = AIStateStepResult::unsupported(); continue; }
            payload = candidate; input.results[slot] = AIStateStepResult::continueState(); continue;
        }
        if (payload.moveAsTeam && !payload.exactPolyline && input.teamProgressTeam[slot] == payload.team &&
            input.teamProgressRevision[slot] > payload.teamRevision)
        {
            if (!input.teamProgressCurrent[slot])
            { input.results[slot] = detail::completeWaypointPath(payload, payload.current); continue; }
            if (input.teamProgressCurrent[slot] == payload.current)
            {
                payload.teamRevision = input.teamProgressRevision[slot];
                payload.awaitingTeamProgress = false;
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            AIWaypointPathStatePayload candidate = payload;
            candidate.prior = candidate.current; candidate.current = input.teamProgressCurrent[slot];
            candidate.teamRevision = input.teamProgressRevision[slot]; candidate.awaitingTeamProgress = false;
            ++candidate.generation;
            if (candidate.generation == 0) ++candidate.generation;
            const size_t required = payload.pathRequestIssued ? size_t{2} : size_t{1};
            const size_t available = input.pathRequests[slot].values.size() - input.pathRequests[slot].count;
            if (!detail::prepareWaypointGoal(input, parameter, candidate, slot) || available < required)
            { input.results[slot] = AIStateStepResult::unsupported(); continue; }
            if (payload.pathRequestIssued)
            {
                AIWaypointPathStatePayload old = payload;
                static_cast<void>(detail::emitWaypointPath(input.pathRequests[slot],
                                                            subjects[slot],
                                                            input.subjectPosition[slot],
                                                             parameter,
                                                             old,
                                                             PathRequestKind::Cancel,
                                                             !detail::waypointFact(input.groundMovement[slot])));
            }
            static_cast<void>(detail::emitWaypointPath(input.pathRequests[slot],
                                                        subjects[slot],
                                                        input.subjectPosition[slot],
                                                         parameter,
                                                         candidate,
                                                         PathRequestKind::New,
                                                         !detail::waypointFact(input.groundMovement[slot])));
            payload = candidate; input.results[slot] = AIStateStepResult::continueState(); continue;
        }
        const PathCorrelation expected = detail::waypointCorrelation(subjects[slot], payload);
        if (payload.phase == AIMoveToPhase::WaitingForPath)
        {
            const PathFeedback& feedback = input.pathFeedback[slot];
            if (!(feedback.correlation == expected)) { input.results[slot] = AIStateStepResult::continueState(); continue; }
            if (feedback.status == PathFeedbackStatus::Pending || feedback.status == PathFeedbackStatus::Delayed)
            { input.results[slot] = AIStateStepResult::continueState(); continue; }
            if (feedback.status == PathFeedbackStatus::Ready)
            {
                if (!feedback.path) { input.results[slot] = AIStateStepResult::unsupported(); continue; }
                AIWaypointPathStatePayload candidate = payload; candidate.path = feedback.path;
                candidate.pathRequestIssued = false; candidate.phase = AIMoveToPhase::FollowingPath;
                if (!detail::emitWaypointInstall(input.movementCommands[slot], subjects[slot],
                                                  input.confirmedTick, candidate,
                                                  parameter.ignoredObstacle,
                                                  parameter.waypointGroupSpeedRaw))
                { input.results[slot] = AIStateStepResult::unsupported(); continue; }
                payload = candidate; input.results[slot] = AIStateStepResult::continueState(); continue;
            }
            if (feedback.status == PathFeedbackStatus::Unsupported)
            { input.results[slot] = AIStateStepResult::unsupported(); continue; }
            input.results[slot] = payload.exactPolyline ? AIStateStepResult::failure()
                                                       : detail::advanceOrdinaryWaypoint(input, subjects[slot], parameter, payload, slot);
            continue;
        }
        const MovementFeedback& feedback = input.movementFeedback[slot];
        if (!(feedback.correlation == expected)) { input.results[slot] = AIStateStepResult::continueState(); continue; }
        if (isMovementActiveFeedback(feedback.status)) { input.results[slot] = AIStateStepResult::continueState(); continue; }
        if (feedback.status == MovementFeedbackStatus::Completed)
        {
            input.results[slot] = payload.exactPolyline
                                      ? detail::completeWaypointPath(payload, payload.prior)
                                      : detail::advanceOrdinaryWaypoint(input, subjects[slot], parameter, payload, slot);
            continue;
        }
        if (feedback.status == MovementFeedbackStatus::Unsupported)
        { input.results[slot] = AIStateStepResult::unsupported(); continue; }
        if (feedback.status == MovementFeedbackStatus::Cancelled)
        { input.results[slot] = payload.exactPolyline ? AIStateStepResult::failure()
                                                     : detail::advanceOrdinaryWaypoint(input, subjects[slot], parameter, payload, slot); continue; }
        const uint32_t max = std::numeric_limits<uint32_t>::max();
        const uint32_t repath = input.ticksPerSecond[slot] > max / 2 ? max : input.ticksPerSecond[slot] * 2;
        if (feedback.status == MovementFeedbackStatus::Blocked && feedback.blockedTicks < repath)
        { input.results[slot] = AIStateStepResult::continueState(); continue; }
        AIWaypointPathStatePayload candidate = payload; ++candidate.generation; if (candidate.generation == 0) ++candidate.generation;
        candidate.phase = AIMoveToPhase::WaitingForPath; candidate.pathRequestIssued = false;
        if (!detail::emitWaypointPath(input.pathRequests[slot], subjects[slot], input.subjectPosition[slot], parameter, candidate, PathRequestKind::Patch, !detail::waypointFact(input.groundMovement[slot])))
        { input.results[slot] = AIStateStepResult::unsupported(); continue; }
        payload = candidate; input.results[slot] = AIStateStepResult::continueState();
    }
    return true;
}

[[nodiscard]] inline bool canExitWaypointPathSoA(const AIStateFamilySoAStorage& storage,
                                                 const AIWaypointStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedWaypointSpans(storage, input)) return false;
    const auto states = storage.payloadStates(); const auto& columns = storage.waypointPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::waypointScheduled(input, slot) || !detail::isWaypointState(states[slot])) continue;
        const AIWaypointPathStatePayload payload = columns.load(slot);
        if (payload.pathRequestIssued && input.pathRequests[slot].count >= input.pathRequests[slot].values.size()) return false;
        if (input.movementCommands[slot].count >= input.movementCommands[slot].values.size()) return false;
        if (payload.completionPending && input.completions[slot].count >= input.completions[slot].values.size()) return false;
    }
    return true;
}

[[nodiscard]] inline bool exitWaypointPathSoA(AIStateFamilySoAStorage& storage,
                                              const AIWaypointStateSoAKernelInput& input) noexcept
{
    if (!canExitWaypointPathSoA(storage, input)) return false;
    const auto subjects = storage.subjects(); const auto states = storage.payloadStates(); const auto parameters = storage.parameters();
    auto& columns = storage.waypointPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::waypointScheduled(input, slot) || !detail::isWaypointState(states[slot])) continue;
        AIWaypointPathStatePayload payload = columns.load(slot);
        if (payload.pathRequestIssued) static_cast<void>(detail::emitWaypointPath(input.pathRequests[slot], subjects[slot], input.subjectPosition[slot], parameters[slot], payload, PathRequestKind::Cancel, !detail::waypointFact(input.groundMovement[slot])));
        if (payload.completionPending)
        {
            static_cast<void>(input.completions[slot].push({.subject = subjects[slot],
                                                            .stateRequest = payload.request,
                                                            .terminal = payload.completionTerminal,
                                                            .confirmedTick = input.confirmedTick}));
            payload.completionPending = false;
        }
        static_cast<void>(input.movementCommands[slot].push({.correlation = detail::waypointCorrelation(subjects[slot], payload),
                                                             .kind = MovementCommandKind::EndMovement,
                                                             .path = payload.path,
                                                             .clearGoal = payload.adjustDestinations,
                                                             .preserveUltraAccurateFinalPosition = true,
                                                             .confirmedTick = input.confirmedTick}));
        columns.store(slot, payload);
    }
    return true;
}

} // namespace engine::ai
