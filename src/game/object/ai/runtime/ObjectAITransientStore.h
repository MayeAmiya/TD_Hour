#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/runtime/AIWakeScheduler.h"
#include "game/object/ai/contracts/AIAttackServices.h"
#include "game/navigation/contracts/NavigationPathContracts.h"
#include "game/object/ai/contracts/AIStateServices.h"
#include "game/object/ai/states/combat/AIGuardStateData.h"
#include "game/object/ai/states/combat/AIOpportunityAttackMoveStateData.h"
#include "game/object/ai/states/combat/AITacticalAttackStateData.h"
#include "game/object/ai/states/special/AIContainmentStateSoAKernels.h"
#include "game/object/ai/states/special/AIDockStateSoAKernels.h"
#include "game/object/ai/states/special/AIInsertionStateSoAKernels.h"

namespace engine::ai
{

enum class ObjectAITransientStatus : uint8_t
{
    Success,
    NotInitialized,
    InvalidValue,
    CapacityExceeded,
    InvalidSnapshot,
};

struct ObjectAITransientSnapshot final
{
    static constexpr uint32_t SchemaVersion = 12;

    uint32_t schemaVersion = SchemaVersion;
    container::Vector<AIWakeEvent> wakeEvents;
    container::Vector<AIStateCommand> facingCommands;
    container::Vector<AIFacingFeedback> facingFeedback;
    container::Vector<PathRequest> pathRequests;
    container::Vector<uint8_t> pathRequestSubmitted;
    container::Vector<uint64_t> pathRequestNextEligibleTick;
    container::Vector<PathFeedback> pathFeedback;
    container::Vector<MovementCommand> movementCommands;
    container::Vector<MovementFeedback> movementFeedback;
    container::Vector<AIWaypointCompletionEvent> waypointCompletions;
    container::Vector<AIAttackCommand> attackCommands;
    container::Vector<AIAttackFeedback> attackFeedback;
    container::Vector<AIAttackOrderCompletion> attackCompletions;
    container::Vector<AIGuardTacticalCommand> guardTacticalCommands;
    container::Vector<AIGuardInteractionCommand> guardInteractionCommands;
    container::Vector<AIGuardFeedback> guardFeedback;
    container::Vector<AIOpportunityAttackMoveQueryCommand> opportunityAttackMoveQueryCommands;
    container::Vector<AIOpportunityAttackMoveQueryFeedback> opportunityAttackMoveQueryFeedback;
    container::Vector<AIOpportunityAttackMoveChildCommand> opportunityAttackMoveChildCommands;
    container::Vector<AIOpportunityAttackMoveChildFeedback> opportunityAttackMoveChildFeedback;
    container::Vector<AITacticalAttackQueryCommand> tacticalAttackQueryCommands;
    container::Vector<AITacticalAttackQueryFeedback> tacticalAttackQueryFeedback;
    container::Vector<AITacticalAttackChildCommand> tacticalAttackChildCommands;
    container::Vector<AITacticalAttackChildFeedback> tacticalAttackChildFeedback;
    container::Vector<AIDockRequest> dockRequests;
    container::Vector<AIDockFeedback> dockFeedback;
    container::Vector<AIContainmentCommand> containmentCommands;
    container::Vector<AIContainmentFeedback> containmentFeedback;
    container::Vector<AIInsertionMotionCommand> insertionMotionCommands;
    container::Vector<AIInsertionMotionFeedback> insertionMotionFeedback;
    container::Vector<AIInsertionContainmentCommand> insertionContainmentCommands;
    container::Vector<AIInsertionContainmentFeedback> insertionContainmentFeedback;
    container::Vector<AIInsertionOperationCommand> insertionOperationCommands;
    container::Vector<AIInsertionOperationFeedback> insertionOperationFeedback;
    container::Vector<AIInsertionEffectCommand> insertionEffectCommands;
};

struct ObjectAITransientClearReport final
{
    size_t wakeEvents = 0;
    size_t commands = 0;
    size_t feedback = 0;
};

// Session-owned cross-phase value storage. State kernels still write their
// small per-slot bounded buffers; the runtime gathers accepted values here for
// adapters and next-visible-tick feedback. No entry retains an ECS entity or a
// service pointer.
class ObjectAITransientStore final
{
public:
    [[nodiscard]] ObjectAITransientStatus initialize(size_t actorCapacity,
                                                      size_t valueCapacity);
    void reset() noexcept;

    [[nodiscard]] AIWakeScheduleResult scheduleWake(ObjectId subject,
                                                     uint64_t wakeTick);
    [[nodiscard]] AIWakeScheduleResult rescheduleWake(ObjectId subject,
                                                       uint64_t wakeTick);
    [[nodiscard]] bool cancelWake(ObjectId subject) noexcept;
    [[nodiscard]] bool hasWake(ObjectId subject) const noexcept;

    // Replaces the complete deterministic wake projection in one bounded
    // pass. The caller supplies canonical (wakeTick, ObjectId) order. This is
    // used after a shadow batch has updated runtime wakeTick values and avoids
    // the quadratic find/erase/insert path of rescheduling every actor.
    [[nodiscard]] ObjectAITransientStatus replaceWakeEvents(
        container::Span<const AIWakeEvent> events);

    [[nodiscard]] ObjectAITransientStatus stage(const AIStateCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIFacingFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(const PathRequest& value);
    [[nodiscard]] ObjectAITransientStatus stage(const PathFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(const MovementCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(const MovementFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(
        const AIWaypointCompletionEvent& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIAttackCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIAttackFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIAttackOrderCompletion& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIGuardTacticalCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIGuardInteractionCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIGuardFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(
        const AIOpportunityAttackMoveQueryCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(
        const AIOpportunityAttackMoveQueryFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(
        const AIOpportunityAttackMoveChildCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(
        const AIOpportunityAttackMoveChildFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AITacticalAttackQueryCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AITacticalAttackQueryFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AITacticalAttackChildCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AITacticalAttackChildFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIDockRequest& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIDockFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIContainmentCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIContainmentFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIInsertionMotionCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIInsertionMotionFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIInsertionContainmentCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIInsertionContainmentFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIInsertionOperationCommand& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIInsertionOperationFeedback& value);
    [[nodiscard]] ObjectAITransientStatus stage(const AIInsertionEffectCommand& value);

    [[nodiscard]] ObjectAITransientClearReport clearSubject(ObjectId subject);
    [[nodiscard]] bool emptyCorrelatedValuesFor(ObjectId subject) const noexcept;
    [[nodiscard]] bool emptyFor(ObjectId subject) const noexcept;

    // Feedback staged after one AI phase is visible exactly once at the next
    // AI phase. Commands and wake projection have independent consumers.
    void discardFeedback() noexcept;

    [[nodiscard]] ObjectAITransientStatus captureSnapshot(
        ObjectAITransientSnapshot& output) const;
    [[nodiscard]] ObjectAITransientStatus restoreSnapshot(
        const ObjectAITransientSnapshot& snapshot);

    [[nodiscard]] container::Span<const AIWakeEvent> wakeEvents() const noexcept;
    [[nodiscard]] container::Span<const AIStateCommand> facingCommands() const noexcept;
    void discardFacingCommands() noexcept;
    [[nodiscard]] bool removeFacingCommand(
        ObjectId subject, AIStateRequestId request) noexcept;
    [[nodiscard]] container::Span<const AIFacingFeedback> facingFeedback() const noexcept;
    [[nodiscard]] container::Span<const PathRequest> pathRequests() const noexcept;
    [[nodiscard]] container::Span<const uint8_t> pathRequestSubmitted() const noexcept;
    [[nodiscard]] container::Span<const uint64_t>
    pathRequestNextEligibleTicks() const noexcept;
    [[nodiscard]] bool canStagePathFeedback() const noexcept;
    [[nodiscard]] bool markPathRequestSubmitted(
        const PathCorrelation& correlation) noexcept;
    [[nodiscard]] bool deferPathRequest(
        const PathCorrelation& correlation,
        uint64_t nextEligibleTick,
        uint64_t confirmedTick) noexcept;
    [[nodiscard]] bool removePathRequest(
        const PathCorrelation& correlation) noexcept;
    [[nodiscard]] container::Span<const PathFeedback> pathFeedback() const noexcept;
    [[nodiscard]] container::Span<const MovementCommand> movementCommands() const noexcept;
    void discardMovementCommands() noexcept;
    [[nodiscard]] container::Span<const MovementFeedback> movementFeedback() const noexcept;
    [[nodiscard]] container::Span<const AIWaypointCompletionEvent>
    waypointCompletions() const noexcept;
    void discardWaypointCompletions() noexcept;
    [[nodiscard]] container::Span<const AIAttackCommand> attackCommands() const noexcept;
    void discardAttackCommands() noexcept;
    [[nodiscard]] container::Span<const AIAttackFeedback> attackFeedback() const noexcept;
    [[nodiscard]] container::Span<const AIAttackOrderCompletion> attackCompletions() const noexcept;
    void discardAttackCompletions() noexcept;
    [[nodiscard]] container::Span<const AIGuardTacticalCommand> guardTacticalCommands() const noexcept;
    void discardGuardTacticalCommands() noexcept;
    [[nodiscard]] bool removeGuardTacticalCommand(
        const AIGuardCorrelation& correlation,
        AIGuardTacticalCommandKind kind) noexcept;
    [[nodiscard]] container::Span<const AIGuardInteractionCommand> guardInteractionCommands() const noexcept;
    void discardGuardInteractionCommands() noexcept;
    [[nodiscard]] bool removeGuardInteractionCommand(
        const AIGuardCorrelation& correlation,
        AIGuardInteractionCommandKind kind) noexcept;
    [[nodiscard]] container::Span<const AIGuardFeedback> guardFeedback() const noexcept;
    [[nodiscard]] container::Span<const AIOpportunityAttackMoveQueryCommand>
    opportunityAttackMoveQueryCommands() const noexcept;
    void discardOpportunityAttackMoveQueryCommands() noexcept;
    [[nodiscard]] container::Span<const AIOpportunityAttackMoveQueryFeedback>
    opportunityAttackMoveQueryFeedback() const noexcept;
    [[nodiscard]] container::Span<const AIOpportunityAttackMoveChildCommand>
    opportunityAttackMoveChildCommands() const noexcept;
    void discardOpportunityAttackMoveChildCommands() noexcept;
    [[nodiscard]] container::Span<const AIOpportunityAttackMoveChildFeedback>
    opportunityAttackMoveChildFeedback() const noexcept;
    [[nodiscard]] container::Span<const AITacticalAttackQueryCommand>
    tacticalAttackQueryCommands() const noexcept;
    void discardTacticalAttackQueryCommands() noexcept;
    [[nodiscard]] container::Span<const AITacticalAttackQueryFeedback>
    tacticalAttackQueryFeedback() const noexcept;
    [[nodiscard]] container::Span<const AITacticalAttackChildCommand>
    tacticalAttackChildCommands() const noexcept;
    void discardTacticalAttackChildCommands() noexcept;
    [[nodiscard]] container::Span<const AITacticalAttackChildFeedback>
    tacticalAttackChildFeedback() const noexcept;
    [[nodiscard]] container::Span<const AIDockRequest> dockRequests() const noexcept;
    void discardDockRequests() noexcept;
    [[nodiscard]] bool removeDockRequest(
        const AIDockCorrelation& correlation,
        AIDockRequestKind kind) noexcept;
    [[nodiscard]] container::Span<const AIDockFeedback> dockFeedback() const noexcept;
    [[nodiscard]] container::Span<const AIContainmentCommand> containmentCommands() const noexcept;
    void discardContainmentCommands() noexcept;
    [[nodiscard]] container::Span<const AIContainmentFeedback> containmentFeedback() const noexcept;
    [[nodiscard]] container::Span<const AIInsertionMotionCommand> insertionMotionCommands() const noexcept;
    void discardInsertionMotionCommands() noexcept;
    [[nodiscard]] container::Span<const AIInsertionMotionFeedback> insertionMotionFeedback() const noexcept;
    [[nodiscard]] container::Span<const AIInsertionContainmentCommand>
    insertionContainmentCommands() const noexcept;
    void discardInsertionContainmentCommands() noexcept;
    [[nodiscard]] container::Span<const AIInsertionContainmentFeedback>
    insertionContainmentFeedback() const noexcept;
    [[nodiscard]] container::Span<const AIInsertionOperationCommand>
    insertionOperationCommands() const noexcept;
    void discardInsertionOperationCommands() noexcept;
    [[nodiscard]] container::Span<const AIInsertionOperationFeedback>
    insertionOperationFeedback() const noexcept;
    [[nodiscard]] container::Span<const AIInsertionEffectCommand>
    insertionEffectCommands() const noexcept;
    void discardInsertionEffectCommands() noexcept;

private:
    void reserveValues(size_t capacity);

    template <typename Value>
    [[nodiscard]] ObjectAITransientStatus push(container::Vector<Value>& values,
                                               const Value& value)
    {
        if (!m_initialized)
            return ObjectAITransientStatus::NotInitialized;
        if (values.size() == m_valueCapacity)
            return ObjectAITransientStatus::CapacityExceeded;
        values.push_back(value);
        return ObjectAITransientStatus::Success;
    }

    template <typename Value>
    [[nodiscard]] ObjectAITransientStatus stageSortedBySubject(
        container::Vector<Value>& values,
        const Value& value)
    {
        if (!m_initialized)
            return ObjectAITransientStatus::NotInitialized;
        const ObjectId subject = value.correlation.subject;
        const auto found = std::lower_bound(
            values.begin(), values.end(), subject,
            [](const Value& candidate, ObjectId expected) {
                return candidate.correlation.subject < expected;
            });
        if (found != values.end() && found->correlation.subject == subject)
        {
            *found = value;
            return ObjectAITransientStatus::Success;
        }
        if (values.size() == m_valueCapacity)
            return ObjectAITransientStatus::CapacityExceeded;
        values.insert(found, value);
        return ObjectAITransientStatus::Success;
    }

    template <typename Value>
    [[nodiscard]] ObjectAITransientStatus stageSortedByCorrelation(
        container::Vector<Value>& values,
        const Value& value)
    {
        if (!m_initialized)
            return ObjectAITransientStatus::NotInitialized;
        if (values.size() == m_valueCapacity)
            return ObjectAITransientStatus::CapacityExceeded;
        const auto position = std::upper_bound(
            values.begin(), values.end(), value.correlation,
            [](const auto& correlation, const Value& candidate) {
                return correlation < candidate.correlation;
            });
        values.insert(position, value);
        return ObjectAITransientStatus::Success;
    }

    template <typename Value>
    [[nodiscard]] static bool strictlySortedBySubject(
        const container::Vector<Value>& values) noexcept
    {
        for (size_t index = 1; index < values.size(); ++index)
        {
            if (!(values[index - 1].correlation.subject <
                  values[index].correlation.subject))
            {
                return false;
            }
        }
        return true;
    }

    template <typename Value>
    [[nodiscard]] static bool sortedByCorrelation(
        const container::Vector<Value>& values) noexcept
    {
        for (size_t index = 1; index < values.size(); ++index)
        {
            if (values[index].correlation < values[index - 1].correlation)
                return false;
        }
        return true;
    }

    [[nodiscard]] container::Vector<AIWakeEvent>::iterator findWake(
        ObjectId subject) noexcept;
    [[nodiscard]] container::Vector<AIWakeEvent>::const_iterator findWake(
        ObjectId subject) const noexcept;
    void insertWake(AIWakeEvent event);

    template <typename Value, typename Subject>
    [[nodiscard]] static size_t eraseSubject(container::Vector<Value>& values,
                                             ObjectId subject,
                                             Subject getSubject)
    {
        const size_t before = values.size();
        values.erase(std::remove_if(values.begin(), values.end(),
                                    [subject, getSubject](const Value& value) {
                                        return getSubject(value) == subject;
                                    }),
                     values.end());
        return before - values.size();
    }

    // A submitted request may still be queued, active, or waiting as a
    // Navigation feedback value. Preserve one unsubmitted Cancel tombstone so
    // Stop, replacement, and object removal release that external ownership.
    // Values which never crossed the adapter can be erased immediately.
    [[nodiscard]] size_t cancelOrErasePathRequest(ObjectId subject) noexcept;

    template <typename Value, typename Subject>
    [[nodiscard]] static size_t countSubject(
        const container::Vector<Value>& values,
        ObjectId subject,
        Subject getSubject) noexcept
    {
        return static_cast<size_t>(std::count_if(
            values.begin(), values.end(),
            [subject, getSubject](const Value& value) {
                return getSubject(value) == subject;
            }));
    }

    size_t m_actorCapacity = 0;
    size_t m_valueCapacity = 0;
    container::Vector<AIWakeEvent> m_wakeEvents;
    container::Vector<AIStateCommand> m_facingCommands;
    container::Vector<AIFacingFeedback> m_facingFeedback;
    container::Vector<PathRequest> m_pathRequests;
    container::Vector<uint8_t> m_pathRequestSubmitted;
    container::Vector<uint64_t> m_pathRequestNextEligibleTick;
    container::Vector<PathFeedback> m_pathFeedback;
    container::Vector<MovementCommand> m_movementCommands;
    container::Vector<MovementFeedback> m_movementFeedback;
    container::Vector<AIWaypointCompletionEvent> m_waypointCompletions;
    container::Vector<AIAttackCommand> m_attackCommands;
    container::Vector<AIAttackFeedback> m_attackFeedback;
    container::Vector<AIAttackOrderCompletion> m_attackCompletions;
    container::Vector<AIGuardTacticalCommand> m_guardTacticalCommands;
    container::Vector<AIGuardInteractionCommand> m_guardInteractionCommands;
    container::Vector<AIGuardFeedback> m_guardFeedback;
    container::Vector<AIOpportunityAttackMoveQueryCommand> m_opportunityAttackMoveQueryCommands;
    container::Vector<AIOpportunityAttackMoveQueryFeedback> m_opportunityAttackMoveQueryFeedback;
    container::Vector<AIOpportunityAttackMoveChildCommand> m_opportunityAttackMoveChildCommands;
    container::Vector<AIOpportunityAttackMoveChildFeedback> m_opportunityAttackMoveChildFeedback;
    container::Vector<AITacticalAttackQueryCommand> m_tacticalAttackQueryCommands;
    container::Vector<AITacticalAttackQueryFeedback> m_tacticalAttackQueryFeedback;
    container::Vector<AITacticalAttackChildCommand> m_tacticalAttackChildCommands;
    container::Vector<AITacticalAttackChildFeedback> m_tacticalAttackChildFeedback;
    container::Vector<AIDockRequest> m_dockRequests;
    container::Vector<AIDockFeedback> m_dockFeedback;
    container::Vector<AIContainmentCommand> m_containmentCommands;
    container::Vector<AIContainmentFeedback> m_containmentFeedback;
    container::Vector<AIInsertionMotionCommand> m_insertionMotionCommands;
    container::Vector<AIInsertionMotionFeedback> m_insertionMotionFeedback;
    container::Vector<AIInsertionContainmentCommand> m_insertionContainmentCommands;
    container::Vector<AIInsertionContainmentFeedback> m_insertionContainmentFeedback;
    container::Vector<AIInsertionOperationCommand> m_insertionOperationCommands;
    container::Vector<AIInsertionOperationFeedback> m_insertionOperationFeedback;
    container::Vector<AIInsertionEffectCommand> m_insertionEffectCommands;
    bool m_initialized = false;
};

} // namespace engine::ai
