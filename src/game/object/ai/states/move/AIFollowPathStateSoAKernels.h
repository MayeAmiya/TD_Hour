#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include "core/container/container_types.h"

#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"
#include "game/object/ai/runtime/AIStateStep.h"

namespace engine::ai
{

struct AIFollowPathStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    int64_t pathfindCellSizeRaw = 0;
    uint32_t maximumSkippedPoints = 1024;
    container::Span<const uint8_t> scheduled{};
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> mobile;
    container::Span<const uint8_t> groundMovement;
    container::Span<const uint8_t> projectile;
    container::Span<const AIFixedPosition> subjectPosition;
    container::Span<const uint32_t> ticksPerSecond;
    const AIPathSequenceResolver* sequences = nullptr;
    container::Span<const PathFeedback> pathFeedback;
    container::Span<const MovementFeedback> movementFeedback;
    container::Span<PathRequestBuffer> pathRequests;
    container::Span<MovementCommandBuffer> movementCommands;
    container::Span<AIStateStepResult> results;
};

namespace detail
{

[[nodiscard]] constexpr bool isFollowPathSoAState(AIStateId state) noexcept
{
    return state == AIStateId::FollowPath || state == AIStateId::FollowExitProductionPath;
}

[[nodiscard]] inline bool hasAlignedFollowPathSoASpans(const AIStateFamilySoAStorage& storage,
                                                       const AIFollowPathStateSoAKernelInput& input) noexcept
{
    const size_t count = storage.size();
    return input.pathfindCellSizeRaw >= 0 && input.sequences != nullptr &&
           (input.scheduled.empty() || input.scheduled.size() == count) &&
           input.effectivelyDead.size() == count && input.mobile.size() == count &&
           input.groundMovement.size() == count && input.projectile.size() == count &&
           input.subjectPosition.size() == count && input.ticksPerSecond.size() == count &&
           input.pathFeedback.size() == count &&
           input.movementFeedback.size() == count && input.pathRequests.size() == count &&
           input.movementCommands.size() == count && input.results.size() == count;
}

[[nodiscard]] constexpr bool followPathSoAScheduled(const AIFollowPathStateSoAKernelInput& input,
                                                     size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}

[[nodiscard]] constexpr bool followPathSoAFact(uint8_t value) noexcept
{
    return value != 0;
}

struct FollowPathPayloadStoreGuard final
{
    AIFollowPathSoAColumns& columns;
    size_t slot = 0;
    AIFollowPathStatePayload& payload;

    ~FollowPathPayloadStoreGuard()
    {
        columns.store(slot, payload);
    }
};

[[nodiscard]] constexpr uint64_t followPathAbsDifference(int64_t left, int64_t right) noexcept
{
    const uint64_t leftBits = static_cast<uint64_t>(left);
    const uint64_t rightBits = static_cast<uint64_t>(right);
    return left >= right ? leftBits - rightBits : rightBits - leftBits;
}

[[nodiscard]] inline bool followPathWithinXYRadius(const AIFixedPosition& left,
                                                   const AIFixedPosition& right,
                                                   int64_t radiusRaw) noexcept
{
    if (radiusRaw <= 0)
        return false;
    uint64_t dx = followPathAbsDifference(left.xRaw, right.xRaw);
    uint64_t dy = followPathAbsDifference(left.yRaw, right.yRaw);
    uint64_t radius = static_cast<uint64_t>(radiusRaw);
    if (dx >= radius || dy >= radius)
        return false;
    // Normalize before squaring so signed 32.32 world coordinates cannot
    // overflow. The same right shifts are deterministic on every platform.
    while (radius > 0x7fffffffULL)
    {
        dx >>= 1;
        dy >>= 1;
        radius >>= 1;
    }
    return dx * dx + dy * dy < radius * radius;
}

[[nodiscard]] constexpr int64_t followPathSaturatingAdd(int64_t left, int64_t right) noexcept
{
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
        return std::numeric_limits<int64_t>::max();
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
        return std::numeric_limits<int64_t>::min();
    return left + right;
}

[[nodiscard]] constexpr int64_t followPathFourCells(int64_t cellSizeRaw) noexcept
{
    return cellSizeRaw > std::numeric_limits<int64_t>::max() / 4
               ? std::numeric_limits<int64_t>::max()
               : cellSizeRaw * 4;
}

[[nodiscard]] inline PathCorrelation followPathCorrelation(ObjectId subject,
                                                            const AIFollowPathStatePayload& payload) noexcept
{
    return {
        .subject = subject,
        .stateRequest = payload.request,
        .generation = payload.generation,
        .sourceOrderRevision = payload.sourceOrderRevision,
    };
}

[[nodiscard]] inline bool emitFollowPathRequest(PathRequestBuffer& output,
                                                ObjectId subject,
                                                const AIFixedPosition& start,
                                                const AIStateParameters& parameters,
                                                AIFollowPathStatePayload& payload,
                                                PathRequestKind kind,
                                                int64_t extraDistanceRaw,
                                                bool preciseFinalZ,
                                                bool quickPath) noexcept
{
    const PathRequest request{
        .correlation = followPathCorrelation(subject, payload),
        .start = start,
        .originalGoal = payload.segmentGoal,
        .adjustDestinations = payload.adjustDestinations,
        .ignoredObstacle = payload.ignoredObstacle,
        .surfaceMask = parameters.pathSurfaceMask,
        .arrivalRadiusRaw = parameters.arrivalRadiusRaw,
        .kind = kind,
        .currentPath = payload.path,
        .traversalMode = quickPath ||
                                 (payload.exitProduction &&
                                  payload.allowThroughUnits &&
                                  payload.ignoredObstacle)
            ? AIPathTraversalMode::DirectLine
            : AIPathTraversalMode::Navmesh,
        .waypointStart = {},
        .waypointGraphRevision = 0,
        .waypointHopLimit = 0,
        .polylineOffset = {},
        .extraDistanceRaw = extraDistanceRaw,
        .pathThroughUnits = payload.allowThroughUnits,
        .preciseFinalZ = preciseFinalZ,
    };
    if (!request.correlation.isValid() || !output.push(request))
        return false;
    payload.pathRequestIssued = kind != PathRequestKind::Cancel;
    if (kind != PathRequestKind::Cancel)
        payload.allowThroughUnits = false;
    return true;
}

[[nodiscard]] inline bool emitFollowPathInstall(MovementCommandBuffer& output,
                                                ObjectId subject,
                                                uint64_t confirmedTick,
                                                const AIFollowPathStatePayload& payload) noexcept
{
    if (!payload.path)
        return false;
    return output.push({
        .correlation = followPathCorrelation(subject, payload),
        .kind = MovementCommandKind::InstallPath,
        .path = payload.path,
        .ignoredObstacle = payload.ignoredObstacle,
        .extraDistanceRaw = payload.extraDistanceRaw,
        .clearGoal = false,
        .preserveUltraAccurateFinalPosition = false,
        .confirmedTick = confirmedTick,
    });
}

[[nodiscard]] inline AIStateStepResult beginFollowPathSegment(
    const AIFollowPathStateSoAKernelInput& input,
    ObjectId subject,
    const AIStateParameters& parameters,
    AIFollowPathStatePayload& payload,
    size_t slot,
    bool skipClosePoints,
    bool emptyIsFailure) noexcept
{
    AIFollowPathStatePayload candidate = payload;
    uint32_t skipped = 0;
    while (true)
    {
        const AIPathSequenceQuery query =
            input.sequences->query(candidate.sequence, candidate.sequenceRevision, candidate.index);
        if (query.status == AIPathSequenceQueryStatus::End)
        {
            payload = candidate;
            return emptyIsFailure ? AIStateStepResult::failure() : AIStateStepResult::success();
        }
        if (query.status == AIPathSequenceQueryStatus::Missing)
            return AIStateStepResult::failure();
        if (query.status != AIPathSequenceQueryStatus::Point)
            return AIStateStepResult::unsupported();
        if (query.point.hasNext && query.point.distanceToNextRaw < 0)
            return AIStateStepResult::unsupported();
        if (!skipClosePoints || !followPathWithinXYRadius(
                                    input.subjectPosition[slot], query.point.position, input.pathfindCellSizeRaw))
        {
            candidate.segmentGoal = query.point.position;
            candidate.finalSegment = !query.point.hasNext;
            candidate.adjustDestinations = candidate.finalSegment && parameters.adjustDestinations &&
                                           (candidate.exitProduction || followPathSoAFact(input.groundMovement[slot]));
            candidate.phase = AIMoveToPhase::WaitingForPath;
            candidate.path = {};
            candidate.pathRequestIssued = false;
            const int64_t extra = query.point.hasNext
                                      ? followPathSaturatingAdd(
                                            query.point.distanceToNextRaw,
                                            query.point.hasFollowing ? followPathFourCells(input.pathfindCellSizeRaw) : 0)
                                      : 0;
            candidate.extraDistanceRaw = extra;
            candidate.preciseFinalZ = candidate.finalSegment && followPathSoAFact(input.projectile[slot]);
            if (!emitFollowPathRequest(input.pathRequests[slot],
                                       subject,
                                       input.subjectPosition[slot],
                                       parameters,
                                       candidate,
                                        PathRequestKind::New,
                                        extra,
                                        candidate.preciseFinalZ,
                                        !followPathSoAFact(input.groundMovement[slot])))
            {
                return AIStateStepResult::unsupported();
            }
            payload = candidate;
            return AIStateStepResult::continueState();
        }
        if (++skipped > input.maximumSkippedPoints || candidate.index == std::numeric_limits<uint32_t>::max())
            return AIStateStepResult::unsupported();
        ++candidate.index;
    }
}

[[nodiscard]] inline AIStateStepResult finishFollowPathSegment(
    const AIFollowPathStateSoAKernelInput& input,
    ObjectId subject,
    const AIStateParameters& parameters,
    AIFollowPathStatePayload& payload,
    size_t slot,
    bool succeeded) noexcept
{
    AIFollowPathStatePayload candidate = payload;
    candidate.ignoredObstacle = INVALID_OBJECT_ID;
    if (!succeeded && candidate.retriesRemaining != 0)
    {
        --candidate.retriesRemaining;
    }
    else
    {
        if (candidate.index == std::numeric_limits<uint32_t>::max())
            return AIStateStepResult::unsupported();
        ++candidate.index;
    }
    ++candidate.generation;
    if (candidate.generation == 0)
        ++candidate.generation;
    candidate.path = {};
    candidate.pathRequestIssued = false;
    const AIStateStepResult result = beginFollowPathSegment(
        input, subject, parameters, candidate, slot, true, false);
    if (result.kind != AIStateStepKind::Unsupported)
        payload = candidate;
    return result;
}

[[nodiscard]] inline bool beginFollowPathRepath(PathRequestBuffer& output,
                                                ObjectId subject,
                                                 const AIFixedPosition& start,
                                                 const AIStateParameters& parameters,
                                                 AIFollowPathStatePayload& payload,
                                                 bool quickPath) noexcept
{
    AIFollowPathStatePayload candidate = payload;
    ++candidate.generation;
    if (candidate.generation == 0)
        ++candidate.generation;
    candidate.phase = AIMoveToPhase::WaitingForPath;
    candidate.pathRequestIssued = false;
    if (!emitFollowPathRequest(
            output,
            subject,
            start,
            parameters,
            candidate,
            PathRequestKind::Patch,
            candidate.extraDistanceRaw,
            candidate.preciseFinalZ,
            quickPath))
        return false;
    payload = candidate;
    return true;
}

} // namespace detail

[[nodiscard]] inline bool enterFollowPathSoA(AIStateFamilySoAStorage& storage,
                                             AIStateId state,
                                             const AIFollowPathStateSoAKernelInput& input) noexcept
{
    if (!detail::isFollowPathSoAState(state) || !detail::hasAlignedFollowPathSoASpans(storage, input))
        return false;
    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& payloads = storage.followPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::followPathSoAScheduled(input, slot) || runtimes[slot].currentState != state)
            continue;
        if (detail::followPathSoAFact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (!detail::followPathSoAFact(input.mobile[slot]))
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (payloadStates[slot] != state)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        const AIStateParameters& parameter = parameters[slot];
        AIFollowPathStatePayload payload = payloads.load(slot);
        if (!subjects[slot] || !parameter.pathSequence || parameter.pathSequenceRevision == 0 ||
            parameter.sourceOrderRevision == 0)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        payload.sequence = parameter.pathSequence;
        payload.sequenceRevision = parameter.pathSequenceRevision;
        payload.sourceOrderRevision = parameter.sourceOrderRevision;
        payload.ignoredObstacle = parameter.ignoredObstacle;
        payload.index = 0;
        payload.generation = 1;
        payload.retriesRemaining = 10;
        payload.exitProduction = state == AIStateId::FollowExitProductionPath;
        payload.allowThroughUnits = payload.exitProduction;
        input.results[slot] = detail::beginFollowPathSegment(
            input, subjects[slot], parameter, payload, slot, false, true);
        if (input.results[slot].kind == AIStateStepKind::Continue)
            payloads.store(slot, payload);
    }
    return true;
}

[[nodiscard]] inline bool updateFollowPathSoA(AIStateFamilySoAStorage& storage,
                                              AIStateId state,
                                              const AIFollowPathStateSoAKernelInput& input) noexcept
{
    if (!detail::isFollowPathSoAState(state) || !detail::hasAlignedFollowPathSoASpans(storage, input))
        return false;
    const auto subjects = storage.subjects();
    const auto runtimes = storage.runtimes();
    const auto payloadStates = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& payloads = storage.followPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::followPathSoAScheduled(input, slot) || runtimes[slot].currentState != state)
            continue;
        if (detail::followPathSoAFact(input.effectivelyDead[slot]))
        {
            input.results[slot] = AIStateStepResult::transitionTo(AIStateId::Dead);
            continue;
        }
        if (!detail::followPathSoAFact(input.mobile[slot]))
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        if (payloadStates[slot] != state)
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        const AIStateParameters& parameter = parameters[slot];
        AIFollowPathStatePayload payload = payloads.load(slot);
        const detail::FollowPathPayloadStoreGuard storeGuard{payloads, slot, payload};
        if (!payload.sequence)
        {
            if (!subjects[slot] || !parameter.pathSequence || parameter.pathSequenceRevision == 0 ||
                parameter.sourceOrderRevision == 0)
            { input.results[slot] = AIStateStepResult::failure(); continue; }
            AIFollowPathStatePayload candidate = payload;
            candidate.sequence = parameter.pathSequence; candidate.sequenceRevision = parameter.pathSequenceRevision;
            candidate.sourceOrderRevision = parameter.sourceOrderRevision; candidate.ignoredObstacle = parameter.ignoredObstacle;
            candidate.index = 0; candidate.generation = 1; candidate.retriesRemaining = 10;
            candidate.exitProduction = state == AIStateId::FollowExitProductionPath;
            candidate.allowThroughUnits = candidate.exitProduction;
            const AIStateStepResult result = detail::beginFollowPathSegment(
                input, subjects[slot], parameter, candidate, slot, false, true);
            if (result.kind == AIStateStepKind::Continue) payload = candidate;
            input.results[slot] = result;
            continue;
        }
        const PathCorrelation expected = detail::followPathCorrelation(subjects[slot], payload);
        if (payload.phase == AIMoveToPhase::WaitingForPath)
        {
            if (!payload.pathRequestIssued)
            {
                input.results[slot] = detail::emitFollowPathRequest(input.pathRequests[slot],
                                                                    subjects[slot],
                                                                    input.subjectPosition[slot],
                                                                    parameter,
                                                                     payload,
                                                                     PathRequestKind::New,
                                                                     payload.extraDistanceRaw,
                                                                     payload.preciseFinalZ,
                                                                     !detail::followPathSoAFact(
                                                                         input.groundMovement[slot]))
                                          ? AIStateStepResult::continueState()
                                          : AIStateStepResult::unsupported();
                continue;
            }
            const PathFeedback& feedback = input.pathFeedback[slot];
            if (!(feedback.correlation == expected))
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            switch (feedback.status)
            {
            case PathFeedbackStatus::Pending:
            case PathFeedbackStatus::Delayed:
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            case PathFeedbackStatus::Ready:
            {
                if (!feedback.path)
                {
                    input.results[slot] = AIStateStepResult::unsupported();
                    continue;
                }
                AIFollowPathStatePayload candidate = payload;
                candidate.pathRequestIssued = false;
                candidate.path = feedback.path;
                candidate.phase = AIMoveToPhase::FollowingPath;
                if (detail::emitFollowPathInstall(
                        input.movementCommands[slot], subjects[slot], input.confirmedTick, candidate))
                {
                    payload = candidate;
                    input.results[slot] = AIStateStepResult::continueState();
                }
                else
                {
                    input.results[slot] = AIStateStepResult::unsupported();
                }
                continue;
            }
            case PathFeedbackStatus::NoPath:
            case PathFeedbackStatus::Cancelled:
                input.results[slot] = detail::finishFollowPathSegment(
                    input, subjects[slot], parameter, payload, slot, false);
                continue;
            case PathFeedbackStatus::Unsupported:
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
        }

        const MovementFeedback& feedback = input.movementFeedback[slot];
        if (!(feedback.correlation == expected))
        {
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }
        switch (feedback.status)
        {
        case MovementFeedbackStatus::Started:
        case MovementFeedbackStatus::Moving:
            input.results[slot] = AIStateStepResult::continueState();
            break;
        case MovementFeedbackStatus::Completed:
            input.results[slot] = detail::finishFollowPathSegment(
                input, subjects[slot], parameter, payload, slot, true);
            break;
        case MovementFeedbackStatus::Blocked:
        {
            const uint32_t maximum = std::numeric_limits<uint32_t>::max();
            const uint32_t repathTicks = input.ticksPerSecond[slot] > maximum / 2
                                             ? maximum
                                             : input.ticksPerSecond[slot] * 2;
            if (feedback.blockedTicks < repathTicks)
            {
                input.results[slot] = AIStateStepResult::continueState();
                break;
            }
            input.results[slot] = detail::beginFollowPathRepath(
                                      input.pathRequests[slot],
                                      subjects[slot],
                                       input.subjectPosition[slot],
                                       parameter,
                                       payload,
                                       !detail::followPathSoAFact(
                                           input.groundMovement[slot]))
                                      ? AIStateStepResult::continueState()
                                      : AIStateStepResult::unsupported();
            break;
        }
        case MovementFeedbackStatus::Stuck:
            input.results[slot] = detail::beginFollowPathRepath(
                                      input.pathRequests[slot],
                                      subjects[slot],
                                       input.subjectPosition[slot],
                                       parameter,
                                       payload,
                                       !detail::followPathSoAFact(
                                           input.groundMovement[slot]))
                                      ? AIStateStepResult::continueState()
                                      : AIStateStepResult::unsupported();
            break;
        case MovementFeedbackStatus::Cancelled:
            input.results[slot] = detail::finishFollowPathSegment(
                input, subjects[slot], parameter, payload, slot, false);
            break;
        case MovementFeedbackStatus::Unsupported:
            input.results[slot] = AIStateStepResult::unsupported();
            break;
        }
    }
    return true;
}

[[nodiscard]] inline bool canExitFollowPathSoA(const AIStateFamilySoAStorage& storage,
                                               const AIFollowPathStateSoAKernelInput& input) noexcept
{
    if (!detail::hasAlignedFollowPathSoASpans(storage, input))
        return false;
    const auto payloadStates = storage.payloadStates();
    const auto& payloads = storage.followPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::followPathSoAScheduled(input, slot) ||
            !detail::isFollowPathSoAState(payloadStates[slot]))
            continue;
        const AIFollowPathStatePayload payload = payloads.load(slot);
        if (payload.pathRequestIssued && input.pathRequests[slot].count >= input.pathRequests[slot].values.size())
            return false;
        if (input.movementCommands[slot].count >= input.movementCommands[slot].values.size())
            return false;
    }
    return true;
}

[[nodiscard]] inline bool exitFollowPathSoA(AIStateFamilySoAStorage& storage,
                                            const AIFollowPathStateSoAKernelInput& input) noexcept
{
    if (!canExitFollowPathSoA(storage, input))
        return false;
    const auto subjects = storage.subjects();
    const auto payloadStates = storage.payloadStates();
    const auto parameters = storage.parameters();
    auto& payloads = storage.followPath();
    for (const size_t slot : storage.executionSlots())
    {
        if (!detail::followPathSoAScheduled(input, slot) ||
            !detail::isFollowPathSoAState(payloadStates[slot]))
            continue;
        AIFollowPathStatePayload payload = payloads.load(slot);
        const detail::FollowPathPayloadStoreGuard storeGuard{payloads, slot, payload};
        if (payload.pathRequestIssued)
        {
            static_cast<void>(detail::emitFollowPathRequest(input.pathRequests[slot],
                                                             subjects[slot],
                                                             input.subjectPosition[slot],
                                                             parameters[slot],
                                                              payload,
                                                              PathRequestKind::Cancel,
                                                              0,
                                                              false,
                                                              !detail::followPathSoAFact(
                                                                  input.groundMovement[slot])));
        }
        static_cast<void>(input.movementCommands[slot].push({
            .correlation = detail::followPathCorrelation(subjects[slot], payload),
            .kind = MovementCommandKind::EndMovement,
            .path = payload.path,
            .clearGoal = payload.adjustDestinations,
            .preserveUltraAccurateFinalPosition = true,
            .confirmedTick = input.confirmedTick,
        }));
    }
    return true;
}

} // namespace engine::ai
