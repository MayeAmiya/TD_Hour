#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIStateData.h"
#include "game/object/ai/runtime/AIStateStep.h"
#include "game/object/ai/states/special/AIInsertionStateData.h"

namespace engine::ai
{

struct AIInsertionCorrelation final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId stateRequest{};
    AIStateId state = AIStateId::Invalid;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return subject && stateRequest.isValid() &&
               (state == AIStateId::RappelInto || state == AIStateId::CombatDrop);
    }
    constexpr auto operator<=>(const AIInsertionCorrelation&) const noexcept = default;
};

// This is an identity for a caller-owned combat-drop operation. Rope state,
// drawables, child lists, random timing and presentation expiry stay behind
// the operation boundary and are never retained by AI storage.

template <typename T, size_t N>
struct AIInsertionBoundedBuffer final
{
    static constexpr size_t Capacity = N;
    container::Array<T, N> values{};
    size_t count = 0;
    bool overflowed = false;

    [[nodiscard]] constexpr bool hasCapacity(size_t additional) const noexcept
    {
        return count <= values.size() && additional <= values.size() - count;
    }

    [[nodiscard]] bool push(const T& value) noexcept
    {
        if (!hasCapacity(1))
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

enum class AIInsertionMotionFeedbackKind : uint8_t
{
    None,
    RappelEntryReady,
    RappelEntryRejected,
    RappelFrame,
    CombatDropApproachReady,
    Unsupported,
};

struct AIInsertionMotionFeedback final
{
    AIInsertionCorrelation correlation{};
    AIInsertionMotionFeedbackKind kind = AIInsertionMotionFeedbackKind::None;
    ObjectId goal = INVALID_OBJECT_ID;
    AIFixedPosition subjectPosition{};
    int64_t layerHeightRaw = 0;
    int64_t groundHeightRaw = 0;
    int64_t buildingTopRaw = 0;
    int64_t desiredSpeedRaw = 0;
    int64_t maximumRappelSpeedRaw = 0;
    int64_t previousPreferredHeightRaw = 0;
    int64_t approachPreferredHeightRaw = 0;
    uint32_t destinationLayer = 0;
    bool canRappel = false;
    bool goalIsStructure = false;
    bool goalAlive = false;
};

using AIInsertionMotionFeedbackBuffer = AIInsertionBoundedBuffer<AIInsertionMotionFeedback, 4>;

enum class AIInsertionContainmentFeedbackKind : uint8_t
{
    None,
    BuildingLandingResolved,
    BuildingMissing,
    Unsupported,
};

struct AIInsertionContainmentFeedback final
{
    AIInsertionCorrelation correlation{};
    AIInsertionContainmentFeedbackKind kind = AIInsertionContainmentFeedbackKind::None;
    ObjectId building = INVALID_OBJECT_ID;
    uint8_t enemiesKilled = 0;
    bool canContain = false;
    AIFixedPosition fallbackPosition{};
    AIFixedPosition fallbackPathEnd{};
    int64_t fallbackOrientationRaw = 0;
    bool fallbackPathFound = false;
};

using AIInsertionContainmentFeedbackBuffer = AIInsertionBoundedBuffer<AIInsertionContainmentFeedback, 4>;

enum class AIInsertionOperationFeedbackKind : uint8_t
{
    None,
    Begun,
    ChildRappelReady,
    Progress,
    Completed,
    Failed,
    Cancelled,
    Unsupported,
};

struct AIInsertionOperationFeedback final
{
    AIInsertionCorrelation correlation{};
    AIInsertionOperationHandle operation{};
    AIInsertionOperationFeedbackKind kind = AIInsertionOperationFeedbackKind::None;
    uint32_t eventSequence = 0;
    ObjectId child = INVALID_OBJECT_ID;
    int64_t childRappelSpeedRaw = 0;
};

using AIInsertionOperationFeedbackBuffer = AIInsertionBoundedBuffer<AIInsertionOperationFeedback, 4>;

enum class AIInsertionMotionCommandKind : uint8_t
{
    SetRappelling,
    ResetDynamicPhysics,
    SetLayer,
    ConstrainRappelVelocity,
    SnapAltitude,
    PlaceAtFallback,
    FollowFallbackPath,
    ClearRappelling,
    RestoreFastDesiredSpeed,
    ConfigureCombatDropApproach,
    RestoreCombatDropApproach,
};

struct AIInsertionMotionCommand final
{
    AIInsertionCorrelation correlation{};
    AIInsertionMotionCommandKind kind = AIInsertionMotionCommandKind::SetRappelling;
    AIFixedPosition position{};
    int64_t verticalSpeedRaw = 0;
    int64_t orientationRaw = 0;
    int64_t preferredHeightRaw = 0;
    uint32_t layer = 0;
    bool ultraAccurate = false;
    uint64_t confirmedTick = 0;
};

using AIInsertionMotionCommandBuffer = AIInsertionBoundedBuffer<AIInsertionMotionCommand, 4>;

enum class AIInsertionContainmentCommandKind : uint8_t
{
    ResolveBuildingLanding,
    AddToContainer,
    CancelBuildingLanding,
};

struct AIInsertionContainmentCommand final
{
    AIInsertionCorrelation correlation{};
    AIInsertionContainmentCommandKind kind = AIInsertionContainmentCommandKind::ResolveBuildingLanding;
    ObjectId building = INVALID_OBJECT_ID;
    uint8_t maximumEnemiesToKill = 0;
    uint64_t confirmedTick = 0;
};

using AIInsertionContainmentCommandBuffer = AIInsertionBoundedBuffer<AIInsertionContainmentCommand, 4>;

enum class AIInsertionOperationCommandKind : uint8_t
{
    Begin,
    Poll,
    OrderChildRappel,
    Cancel,
};

struct AIInsertionOperationCommand final
{
    AIInsertionCorrelation correlation{};
    AIInsertionOperationCommandKind kind = AIInsertionOperationCommandKind::Begin;
    AIInsertionOperationHandle operation{};
    uint32_t eventSequence = 0;
    ObjectId goal = INVALID_OBJECT_ID;
    AIFixedPosition goalPosition{};
    ObjectId child = INVALID_OBJECT_ID;
    int64_t childRappelSpeedRaw = 0;
    uint64_t confirmedTick = 0;
};

using AIInsertionOperationCommandBuffer = AIInsertionBoundedBuffer<AIInsertionOperationCommand, 4>;

enum class AIInsertionEffectCommandKind : uint8_t
{
    PlayCombatDropKillEffect,
    KillSubject,
};

struct AIInsertionEffectCommand final
{
    AIInsertionCorrelation correlation{};
    AIInsertionEffectCommandKind kind = AIInsertionEffectCommandKind::PlayCombatDropKillEffect;
    ObjectId target = INVALID_OBJECT_ID;
    uint8_t enemiesKilled = 0;
    uint64_t confirmedTick = 0;
};

using AIInsertionEffectCommandBuffer = AIInsertionBoundedBuffer<AIInsertionEffectCommand, 4>;


// Every span is one field-level SoA column. The caller owns allocation and
// snapshot integration; this independent slice does not extend shared AI
// family storage.

struct AIInsertionStateSoAKernelInput final
{
    uint64_t confirmedTick = 0;
    container::Span<const uint8_t> scheduled{};
    AIExecutionSlotRange executionSlots{};
    container::Span<const AIStateId> activeStates;
    container::Span<const ObjectId> subjects;
    container::Span<const ObjectId> goalObjects;
    container::Span<const AIFixedPosition> goalPositions;
    container::Span<const uint8_t> goalPositionValid;
    container::Span<const AIStateParameters> parameters;
    container::Span<const uint8_t> effectivelyDead;
    container::Span<const uint8_t> mobile;
    container::Span<const AIFixedPosition> subjectPositions;
    container::Span<const PathFeedback> pathFeedback;
    container::Span<const MovementFeedback> movementFeedback;
    container::Span<const AIInsertionMotionFeedbackBuffer> motionFeedback;
    container::Span<const AIInsertionContainmentFeedbackBuffer> containmentFeedback;
    container::Span<const AIInsertionOperationFeedbackBuffer> operationFeedback;
    container::Span<AIInsertionMotionCommandBuffer> motionCommands;
    container::Span<AIInsertionContainmentCommandBuffer> containmentCommands;
    container::Span<AIInsertionOperationCommandBuffer> operationCommands;
    container::Span<AIInsertionEffectCommandBuffer> effectCommands;
    container::Span<PathRequestBuffer> pathRequests;
    container::Span<MovementCommandBuffer> movementCommands;
    container::Span<AIStateStepResult> results;
};

namespace insertion_detail
{

[[nodiscard]] bool hasAlignedInputSpans(size_t count, const AIInsertionStateSoAKernelInput& input) noexcept;
[[nodiscard]] bool hasAlignedSpans(const AIInsertionStateSoAColumns& columns,
                                   const AIInsertionStateSoAKernelInput& input) noexcept;

[[nodiscard]] constexpr bool scheduled(const AIInsertionStateSoAKernelInput& input, size_t slot) noexcept
{
    return input.scheduled.empty() || input.scheduled[slot] != 0;
}

[[nodiscard]] constexpr AIInsertionCorrelation correlation(const AIInsertionStateSoAColumns& columns,
                                                            const AIInsertionStateSoAKernelInput& input,
                                                            size_t slot,
                                                            AIStateId state) noexcept
{
    return {.subject = input.subjects[slot],
            .stateRequest = {columns.requestTick[slot],columns.requestSequence[slot]}, .state = state};
}

template <typename T, size_t N, typename Predicate>
[[nodiscard]] inline const T* uniqueMatch(const AIInsertionBoundedBuffer<T, N>& buffer,
                                          Predicate&& predicate,
                                          bool& ambiguous) noexcept
{
    const size_t count = buffer.count < buffer.values.size() ? buffer.count : buffer.values.size();
    const T* match = nullptr;
    ambiguous = false;
    for (size_t index = 0; index < count; ++index)
    {
        const T& candidate = buffer.values[index];
        if (!predicate(candidate))
            continue;
        if (match)
        {
            ambiguous = true;
            return nullptr;
        }
        match = &candidate;
    }
    return match;
}

[[nodiscard]] const AIInsertionMotionFeedback* motionEvent(
    const AIInsertionMotionFeedbackBuffer& buffer,
    const AIInsertionCorrelation& expected,
    bool& ambiguous) noexcept;

[[nodiscard]] const AIInsertionContainmentFeedback* containmentEvent(
    const AIInsertionContainmentFeedbackBuffer& buffer,
    const AIInsertionCorrelation& expected,
    ObjectId building,
    bool& ambiguous) noexcept;

[[nodiscard]] const AIInsertionOperationFeedback* beginEvent(
    const AIInsertionOperationFeedbackBuffer& buffer,
    const AIInsertionCorrelation& expected,
    bool& ambiguous) noexcept;

[[nodiscard]] const AIInsertionOperationFeedback* pollEvent(
    const AIInsertionOperationFeedbackBuffer& buffer,
    const AIInsertionCorrelation& expected,
    AIInsertionOperationHandle operation,
    uint32_t sequence,
    bool& ambiguous) noexcept;

[[nodiscard]] constexpr int64_t positiveMagnitude(int64_t value) noexcept
{
    if (value >= 0)
        return value;
    if (value == std::numeric_limits<int64_t>::min())
        return std::numeric_limits<int64_t>::max();
    return -value;
}

[[nodiscard]] inline PathCorrelation combatDropPathCorrelation(
    const AIInsertionStateSoAColumns& columns,
    const AIInsertionStateSoAKernelInput& input,
    size_t slot) noexcept
{
    return {
        .subject = input.subjects[slot],
        .stateRequest = {
            columns.requestTick[slot], columns.requestSequence[slot]},
        .generation = columns.combatDropPathGeneration[slot],
        .sourceOrderRevision =
            columns.combatDropSourceOrderRevision[slot],
    };
}

[[nodiscard]] inline bool emitCombatDropPathRequest(
    AIInsertionStateSoAColumns columns,
    const AIInsertionStateSoAKernelInput& input,
    size_t slot,
    PathRequestKind kind) noexcept
{
    const AIStateParameters& parameters = input.parameters[slot];
    const PathRequest request{
        .correlation = combatDropPathCorrelation(columns, input, slot),
        .start = input.subjectPositions[slot],
        .originalGoal = parameters.goalPosition,
        .adjustDestinations = false,
        .ignoredObstacle = parameters.ignoredObstacle,
        .surfaceMask = parameters.pathSurfaceMask,
        .arrivalRadiusRaw = parameters.arrivalRadiusRaw,
        .kind = kind,
        .currentPath = columns.combatDropPath[slot],
        .traversalMode = AIPathTraversalMode::DirectLine,
        .preciseFinalZ = true,
    };
    if (!request.correlation.isValid() ||
        !input.pathRequests[slot].push(request))
        return false;
    columns.combatDropPathRequestIssued[slot] =
        kind == PathRequestKind::Cancel ? uint8_t{0} : uint8_t{1};
    return true;
}

[[nodiscard]] inline bool emitCombatDropMovement(
    AIInsertionStateSoAColumns columns,
    const AIInsertionStateSoAKernelInput& input,
    size_t slot,
    MovementCommandKind kind) noexcept
{
    return input.movementCommands[slot].push({
        .correlation = combatDropPathCorrelation(columns, input, slot),
        .kind = kind,
        .path = columns.combatDropPath[slot],
        .ignoredObstacle = input.parameters[slot].ignoredObstacle,
        .clearGoal = kind == MovementCommandKind::EndMovement,
        .preserveUltraAccurateFinalPosition =
            kind == MovementCommandKind::EndMovement,
        .confirmedTick = input.confirmedTick,
    });
}

void emitMotion(const AIInsertionStateSoAKernelInput& input,
                size_t slot,
                const AIInsertionCorrelation& expected,
                AIInsertionMotionCommandKind kind,
                AIFixedPosition position = {},
                int64_t verticalSpeedRaw = 0,
                int64_t orientationRaw = 0,
                uint32_t layer = 0,
                int64_t preferredHeightRaw = 0,
                bool ultraAccurate = false) noexcept;

void emitContainment(const AIInsertionStateSoAKernelInput& input,
                     size_t slot,
                     const AIInsertionCorrelation& expected,
                     AIInsertionContainmentCommandKind kind,
                     ObjectId building,
                     uint8_t maximumEnemiesToKill = 0) noexcept;

void emitOperation(const AIInsertionStateSoAKernelInput& input,
                   size_t slot,
                   const AIInsertionCorrelation& expected,
                   AIInsertionOperationCommandKind kind,
                   AIInsertionOperationHandle operation = {},
                   uint32_t eventSequence = 0,
                   ObjectId child = INVALID_OBJECT_ID,
                   int64_t childRappelSpeedRaw = 0) noexcept;

void emitEffect(const AIInsertionStateSoAKernelInput& input,
                size_t slot,
                const AIInsertionCorrelation& expected,
                AIInsertionEffectCommandKind kind,
                ObjectId target,
                uint8_t enemiesKilled = 0) noexcept;

} // namespace insertion_detail

[[nodiscard]] bool enterRappelIntoStateSoA(AIInsertionStateSoAColumns columns,
                                           const AIInsertionStateSoAKernelInput& input) noexcept;

[[nodiscard]] inline bool updateRappelIntoStateSoA(AIInsertionStateSoAColumns columns,
                                                   const AIInsertionStateSoAKernelInput& input) noexcept
{
    using namespace insertion_detail;
    if (!hasAlignedSpans(columns, input))
        return false;

    // Preflight every output domain for the complete scheduled batch.
    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::RappelInto)
            continue;
        const auto expected = correlation(columns,input, slot, AIStateId::RappelInto);
        if (columns.rappelPhase[slot] == AIRappelInsertionPhase::Descending)
        {
            bool ambiguous = false;
            const auto* event = motionEvent(input.motionFeedback[slot], expected, ambiguous);
            if (!ambiguous && event && event->kind == AIInsertionMotionFeedbackKind::RappelFrame &&
                (columns.rappelTargetIsBuilding[slot] == 0 || event->goal == columns.rappelTarget[slot]))
            {
                const bool building = columns.rappelTargetIsBuilding[slot] != 0 && event->goalAlive;
                const int64_t destination = building ? columns.rappelDestinationZRaw[slot] : event->layerHeightRaw;
                const bool arrived = event->subjectPosition.zRaw <= destination;
                const size_t motionRequired = 1 + static_cast<size_t>(arrived);
                const size_t containmentRequired = static_cast<size_t>(arrived && building);
                if (!input.motionCommands[slot].hasCapacity(motionRequired) ||
                    !input.containmentCommands[slot].hasCapacity(containmentRequired))
                    return false;
            }
        }
        else if (columns.rappelPhase[slot] == AIRappelInsertionPhase::AwaitingBuildingResolution)
        {
            bool ambiguous = false;
            const auto* event = containmentEvent(
                input.containmentFeedback[slot], expected, columns.rappelTarget[slot], ambiguous);
            if (!ambiguous && event && event->kind == AIInsertionContainmentFeedbackKind::BuildingLandingResolved)
            {
                const bool selfKilled = event->enemiesKilled >= 2;
                const bool fallback = !selfKilled && !event->canContain;
                const size_t motionRequired =
                    static_cast<size_t>(fallback) + static_cast<size_t>(fallback && event->fallbackPathFound);
                const size_t containmentRequired = static_cast<size_t>(!selfKilled && event->canContain);
                const size_t effectRequired = static_cast<size_t>(event->enemiesKilled != 0) +
                                              static_cast<size_t>(selfKilled);
                if (!input.motionCommands[slot].hasCapacity(motionRequired) ||
                    !input.containmentCommands[slot].hasCapacity(containmentRequired) ||
                    !input.effectCommands[slot].hasCapacity(effectRequired))
                    return false;
            }
        }
    }

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::RappelInto)
            continue;
        const auto expected = correlation(columns,input, slot, AIStateId::RappelInto);
        if (!expected.isValid())
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }

        if (columns.rappelPhase[slot] == AIRappelInsertionPhase::Descending)
        {
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
            if (event->kind != AIInsertionMotionFeedbackKind::RappelFrame ||
                (columns.rappelTargetIsBuilding[slot] != 0 && event->goal != columns.rappelTarget[slot]))
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }

            if (columns.rappelTargetIsBuilding[slot] != 0 && !event->goalAlive)
            {
                columns.rappelTargetIsBuilding[slot] = 0;
                columns.rappelTarget[slot] = INVALID_OBJECT_ID;
                columns.rappelDestinationZRaw[slot] = event->groundHeightRaw;
            }
            if (columns.rappelTargetIsBuilding[slot] == 0)
                columns.rappelDestinationZRaw[slot] = event->layerHeightRaw;

            emitMotion(input,
                       slot,
                       expected,
                       AIInsertionMotionCommandKind::ConstrainRappelVelocity,
                       {},
                       columns.rappelSpeedRaw[slot]);
            if (event->subjectPosition.zRaw > columns.rappelDestinationZRaw[slot])
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }

            AIFixedPosition snapped = event->subjectPosition;
            snapped.zRaw = columns.rappelDestinationZRaw[slot];
            emitMotion(input, slot, expected, AIInsertionMotionCommandKind::SnapAltitude, snapped);
            if (columns.rappelTargetIsBuilding[slot] != 0)
            {
                emitContainment(input,
                                slot,
                                expected,
                                AIInsertionContainmentCommandKind::ResolveBuildingLanding,
                                columns.rappelTarget[slot],
                                2);
                columns.rappelPhase[slot] = AIRappelInsertionPhase::AwaitingBuildingResolution;
                input.results[slot] = AIStateStepResult::continueState();
            }
            else
            {
                columns.rappelPhase[slot] = AIRappelInsertionPhase::Landed;
                input.results[slot] = AIStateStepResult::success();
            }
            continue;
        }

        if (columns.rappelPhase[slot] == AIRappelInsertionPhase::AwaitingBuildingResolution)
        {
            bool ambiguous = false;
            const auto* event = containmentEvent(
                input.containmentFeedback[slot], expected, columns.rappelTarget[slot], ambiguous);
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
            if (event->kind == AIInsertionContainmentFeedbackKind::BuildingMissing)
            {
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            if (event->kind != AIInsertionContainmentFeedbackKind::BuildingLandingResolved)
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }

            if (event->enemiesKilled != 0)
                emitEffect(input,
                           slot,
                           expected,
                           AIInsertionEffectCommandKind::PlayCombatDropKillEffect,
                           columns.rappelTarget[slot],
                           event->enemiesKilled);
            if (event->enemiesKilled >= 2)
            {
                emitEffect(input,
                           slot,
                           expected,
                           AIInsertionEffectCommandKind::KillSubject,
                           input.subjects[slot]);
            }
            else if (event->canContain)
            {
                emitContainment(input,
                                slot,
                                expected,
                                AIInsertionContainmentCommandKind::AddToContainer,
                                columns.rappelTarget[slot]);
            }
            else
            {
                emitMotion(input,
                           slot,
                           expected,
                           AIInsertionMotionCommandKind::PlaceAtFallback,
                           event->fallbackPosition,
                           0,
                           event->fallbackOrientationRaw);
                if (event->fallbackPathFound)
                    emitMotion(input,
                               slot,
                               expected,
                               AIInsertionMotionCommandKind::FollowFallbackPath,
                               event->fallbackPathEnd);
            }
            columns.rappelPhase[slot] = AIRappelInsertionPhase::Landed;
            input.results[slot] = AIStateStepResult::success();
            continue;
        }

        input.results[slot] = columns.rappelPhase[slot] == AIRappelInsertionPhase::Landed
                                  ? AIStateStepResult::success()
                                  : AIStateStepResult::unsupported();
    }
    return true;
}

[[nodiscard]] bool canExitRappelIntoStateSoA(const AIInsertionStateSoAColumns& columns,
                                             const AIInsertionStateSoAKernelInput& input) noexcept;
[[nodiscard]] bool exitRappelIntoStateSoA(AIInsertionStateSoAColumns columns,
                                          const AIInsertionStateSoAKernelInput& input) noexcept;
[[nodiscard]] bool enterCombatDropStateSoA(AIInsertionStateSoAColumns columns,
                                           const AIInsertionStateSoAKernelInput& input) noexcept;

[[nodiscard]] inline bool updateCombatDropStateSoA(AIInsertionStateSoAColumns columns,
                                                   const AIInsertionStateSoAKernelInput& input) noexcept
{
    using namespace insertion_detail;
    if (!hasAlignedSpans(columns, input))
        return false;

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::CombatDrop)
            continue;
        const auto expected = correlation(columns,input, slot, AIStateId::CombatDrop);
        bool ambiguous = false;
        if (columns.combatDropPhase[slot] ==
            AICombatDropInsertionPhase::ApproachWaitingForPath)
        {
            const PathFeedback& event = input.pathFeedback[slot];
            if (event.correlation ==
                    combatDropPathCorrelation(columns, input, slot) &&
                event.status == PathFeedbackStatus::Ready &&
                !input.movementCommands[slot].hasCapacity(1))
                return false;
        }
        else if (columns.combatDropPhase[slot] ==
                 AICombatDropInsertionPhase::ApproachFollowingPath)
        {
            const MovementFeedback& event = input.movementFeedback[slot];
            if (event.correlation ==
                combatDropPathCorrelation(columns, input, slot))
            {
                if (event.status == MovementFeedbackStatus::Completed &&
                    (!input.movementCommands[slot].hasCapacity(1) ||
                     !input.motionCommands[slot].hasCapacity(1) ||
                     !input.operationCommands[slot].hasCapacity(1)))
                    return false;
                if (event.status == MovementFeedbackStatus::Stuck &&
                    !input.pathRequests[slot].hasCapacity(1))
                    return false;
            }
        }
        else if (columns.combatDropPhase[slot] == AICombatDropInsertionPhase::BeginPending)
        {
            const auto* event = beginEvent(input.operationFeedback[slot], expected, ambiguous);
            if (!ambiguous && event && event->kind == AIInsertionOperationFeedbackKind::Begun && event->operation &&
                !input.operationCommands[slot].hasCapacity(1))
                return false;
        }
        else if (columns.combatDropPhase[slot] == AICombatDropInsertionPhase::PollPending)
        {
            const auto* event = pollEvent(input.operationFeedback[slot],
                                          expected,
                                          columns.combatDropOperation[slot],
                                          columns.combatDropNextEventSequence[slot],
                                          ambiguous);
            if (!ambiguous && event)
            {
                size_t required = 0;
                if (event->kind == AIInsertionOperationFeedbackKind::ChildRappelReady && event->child &&
                    columns.combatDropNextEventSequence[slot] != std::numeric_limits<uint32_t>::max())
                    required = 2;
                else if (event->kind == AIInsertionOperationFeedbackKind::Progress &&
                         columns.combatDropNextEventSequence[slot] != std::numeric_limits<uint32_t>::max())
                    required = 1;
                if (!input.operationCommands[slot].hasCapacity(required))
                    return false;
            }
        }
    }

    for (const size_t slot : executionSlotRange(input.executionSlots, input.activeStates.size()))
    {
        if (!scheduled(input, slot) || input.activeStates[slot] != AIStateId::CombatDrop)
            continue;
        const auto expected = correlation(columns,input, slot, AIStateId::CombatDrop);
        if (!expected.isValid())
        {
            input.results[slot] = AIStateStepResult::unsupported();
            continue;
        }
        if (input.effectivelyDead[slot] != 0)
        {
            input.results[slot] = AIStateStepResult::failure();
            continue;
        }
        bool ambiguous = false;
        if (columns.combatDropPhase[slot] ==
            AICombatDropInsertionPhase::ApproachWaitingForPath)
        {
            const PathCorrelation pathExpected =
                combatDropPathCorrelation(columns, input, slot);
            const PathFeedback& event = input.pathFeedback[slot];
            if (!(event.correlation == pathExpected))
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            switch (event.status)
            {
            case PathFeedbackStatus::Pending:
            case PathFeedbackStatus::Delayed:
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            case PathFeedbackStatus::Ready:
                if (!event.path)
                {
                    input.results[slot] = AIStateStepResult::unsupported();
                    continue;
                }
                columns.combatDropPath[slot] = event.path;
                columns.combatDropPathRequestIssued[slot] = 0;
                if (!emitCombatDropMovement(
                        columns, input, slot,
                        MovementCommandKind::InstallPath))
                {
                    input.results[slot] = AIStateStepResult::unsupported();
                    continue;
                }
                columns.combatDropPhase[slot] =
                    AICombatDropInsertionPhase::ApproachFollowingPath;
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            case PathFeedbackStatus::NoPath:
            case PathFeedbackStatus::Cancelled:
                columns.combatDropPathRequestIssued[slot] = 0;
                input.results[slot] = AIStateStepResult::failure();
                continue;
            case PathFeedbackStatus::Unsupported:
                columns.combatDropPathRequestIssued[slot] = 0;
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
        }

        if (columns.combatDropPhase[slot] ==
            AICombatDropInsertionPhase::ApproachFollowingPath)
        {
            const MovementFeedback& event = input.movementFeedback[slot];
            if (!(event.correlation ==
                  combatDropPathCorrelation(columns, input, slot)))
            {
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            }
            switch (event.status)
            {
            case MovementFeedbackStatus::Started:
            case MovementFeedbackStatus::Progress:
            case MovementFeedbackStatus::Blocked:
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            case MovementFeedbackStatus::Completed:
                static_cast<void>(emitCombatDropMovement(
                    columns, input, slot,
                    MovementCommandKind::EndMovement));
                emitMotion(
                    input, slot, expected,
                    AIInsertionMotionCommandKind::RestoreCombatDropApproach,
                    {}, 0, 0, 0,
                    columns.combatDropOldPreferredHeightRaw[slot], false);
                columns.combatDropApproachConfigured[slot] = 0;
                columns.combatDropPath[slot] = {};
                columns.combatDropPhase[slot] =
                    AICombatDropInsertionPhase::BeginPending;
                emitOperation(input, slot, expected,
                              AIInsertionOperationCommandKind::Begin);
                input.results[slot] = AIStateStepResult::continueState();
                continue;
            case MovementFeedbackStatus::Stuck:
                ++columns.combatDropPathGeneration[slot];
                if (columns.combatDropPathGeneration[slot] == 0)
                    ++columns.combatDropPathGeneration[slot];
                columns.combatDropPhase[slot] =
                    AICombatDropInsertionPhase::ApproachWaitingForPath;
                input.results[slot] = emitCombatDropPathRequest(
                    columns, input, slot, PathRequestKind::Patch)
                    ? AIStateStepResult::continueState()
                    : AIStateStepResult::unsupported();
                continue;
            case MovementFeedbackStatus::Cancelled:
                input.results[slot] = AIStateStepResult::failure();
                continue;
            case MovementFeedbackStatus::Unsupported:
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
        }

        if (columns.combatDropPhase[slot] == AICombatDropInsertionPhase::BeginPending)
        {
            const auto* event = beginEvent(input.operationFeedback[slot], expected, ambiguous);
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
            if (event->kind == AIInsertionOperationFeedbackKind::Failed ||
                event->kind == AIInsertionOperationFeedbackKind::Cancelled)
            {
                columns.combatDropPhase[slot] = AICombatDropInsertionPhase::Finished;
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            if (event->kind != AIInsertionOperationFeedbackKind::Begun || !event->operation)
            {
                columns.combatDropPhase[slot] = AICombatDropInsertionPhase::Finished;
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
            columns.combatDropOperation[slot] = event->operation;
            columns.combatDropNextEventSequence[slot] = 1;
            columns.combatDropPhase[slot] = AICombatDropInsertionPhase::PollPending;
            emitOperation(input,
                          slot,
                          expected,
                          AIInsertionOperationCommandKind::Poll,
                          event->operation,
                          1);
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }

        if (columns.combatDropPhase[slot] == AICombatDropInsertionPhase::PollPending)
        {
            const auto operation = columns.combatDropOperation[slot];
            const uint32_t sequence = columns.combatDropNextEventSequence[slot];
            if (!operation || sequence == 0)
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
            const auto* event =
                pollEvent(input.operationFeedback[slot], expected, operation, sequence, ambiguous);
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
            if (event->kind == AIInsertionOperationFeedbackKind::Completed)
            {
                columns.combatDropPhase[slot] = AICombatDropInsertionPhase::Finished;
                input.results[slot] = AIStateStepResult::success();
                continue;
            }
            if (event->kind == AIInsertionOperationFeedbackKind::Failed ||
                event->kind == AIInsertionOperationFeedbackKind::Cancelled)
            {
                columns.combatDropPhase[slot] = AICombatDropInsertionPhase::Finished;
                input.results[slot] = AIStateStepResult::failure();
                continue;
            }
            if (event->kind == AIInsertionOperationFeedbackKind::Unsupported)
            {
                columns.combatDropPhase[slot] = AICombatDropInsertionPhase::Finished;
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
            if (sequence == std::numeric_limits<uint32_t>::max())
            {
                columns.combatDropPhase[slot] = AICombatDropInsertionPhase::Finished;
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
            const uint32_t next = sequence + 1;
            if (event->kind == AIInsertionOperationFeedbackKind::ChildRappelReady)
            {
                if (!event->child)
                {
                    input.results[slot] = AIStateStepResult::unsupported();
                    continue;
                }
                emitOperation(input,
                              slot,
                              expected,
                              AIInsertionOperationCommandKind::OrderChildRappel,
                              operation,
                              sequence,
                              event->child,
                              event->childRappelSpeedRaw);
            }
            else if (event->kind != AIInsertionOperationFeedbackKind::Progress)
            {
                input.results[slot] = AIStateStepResult::unsupported();
                continue;
            }
            emitOperation(input, slot, expected, AIInsertionOperationCommandKind::Poll, operation, next);
            columns.combatDropNextEventSequence[slot] = next;
            input.results[slot] = AIStateStepResult::continueState();
            continue;
        }

        input.results[slot] = columns.combatDropPhase[slot] == AICombatDropInsertionPhase::Finished
                                  ? AIStateStepResult::success()
                                  : AIStateStepResult::unsupported();
    }
    return true;
}

[[nodiscard]] bool canExitCombatDropStateSoA(const AIInsertionStateSoAColumns& columns,
                                             const AIInsertionStateSoAKernelInput& input) noexcept;
[[nodiscard]] bool exitCombatDropStateSoA(AIInsertionStateSoAColumns columns,
                                          const AIInsertionStateSoAKernelInput& input) noexcept;

} // namespace engine::ai
