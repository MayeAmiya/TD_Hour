#include "game/object/ai/runtime/ObjectAITransientStore.h"

#include <utility>

namespace engine::ai
{

ObjectAITransientStatus ObjectAITransientStore::captureSnapshot(
    ObjectAITransientSnapshot& output) const
{
    if (!m_initialized)
        return ObjectAITransientStatus::NotInitialized;
    output = {
        .wakeEvents = m_wakeEvents,
        .facingCommands = m_facingCommands,
        .facingFeedback = m_facingFeedback,
        .pathRequests = m_pathRequests,
        .pathRequestSubmitted = m_pathRequestSubmitted,
        .pathRequestNextEligibleTick = m_pathRequestNextEligibleTick,
        .pathFeedback = m_pathFeedback,
        .movementCommands = m_movementCommands,
        .movementFeedback = m_movementFeedback,
        .waypointCompletions = m_waypointCompletions,
        .attackCommands = m_attackCommands,
        .attackFeedback = m_attackFeedback,
        .attackCompletions = m_attackCompletions,
        .guardTacticalCommands = m_guardTacticalCommands,
        .guardInteractionCommands = m_guardInteractionCommands,
        .guardFeedback = m_guardFeedback,
        .opportunityAttackMoveQueryCommands =
            m_opportunityAttackMoveQueryCommands,
        .opportunityAttackMoveQueryFeedback =
            m_opportunityAttackMoveQueryFeedback,
        .opportunityAttackMoveChildCommands =
            m_opportunityAttackMoveChildCommands,
        .opportunityAttackMoveChildFeedback =
            m_opportunityAttackMoveChildFeedback,
        .tacticalAttackQueryCommands = m_tacticalAttackQueryCommands,
        .tacticalAttackQueryFeedback = m_tacticalAttackQueryFeedback,
        .tacticalAttackChildCommands = m_tacticalAttackChildCommands,
        .tacticalAttackChildFeedback = m_tacticalAttackChildFeedback,
        .dockRequests = m_dockRequests,
        .dockFeedback = m_dockFeedback,
        .containmentCommands = m_containmentCommands,
        .containmentFeedback = m_containmentFeedback,
        .insertionMotionCommands = m_insertionMotionCommands,
        .insertionMotionFeedback = m_insertionMotionFeedback,
        .insertionContainmentCommands = m_insertionContainmentCommands,
        .insertionContainmentFeedback = m_insertionContainmentFeedback,
        .insertionOperationCommands = m_insertionOperationCommands,
        .insertionOperationFeedback = m_insertionOperationFeedback,
        .insertionEffectCommands = m_insertionEffectCommands,
    };
    return ObjectAITransientStatus::Success;
}

ObjectAITransientStatus ObjectAITransientStore::restoreSnapshot(
    const ObjectAITransientSnapshot& snapshot)
{
    if (!m_initialized)
        return ObjectAITransientStatus::NotInitialized;
    if (snapshot.schemaVersion != ObjectAITransientSnapshot::SchemaVersion ||
        snapshot.wakeEvents.size() > m_actorCapacity ||
        snapshot.facingCommands.size() > m_valueCapacity ||
        snapshot.facingFeedback.size() > m_valueCapacity ||
        snapshot.pathRequests.size() > m_valueCapacity ||
        snapshot.pathRequestSubmitted.size() != snapshot.pathRequests.size() ||
        snapshot.pathRequestNextEligibleTick.size() !=
            snapshot.pathRequests.size() ||
        snapshot.pathFeedback.size() > m_valueCapacity ||
        snapshot.movementCommands.size() > m_valueCapacity ||
        snapshot.movementFeedback.size() > m_valueCapacity ||
        snapshot.waypointCompletions.size() > m_valueCapacity ||
        snapshot.attackCommands.size() > m_valueCapacity ||
        snapshot.attackFeedback.size() > m_valueCapacity ||
        snapshot.attackCompletions.size() > m_valueCapacity ||
        snapshot.guardTacticalCommands.size() > m_valueCapacity ||
        snapshot.guardInteractionCommands.size() > m_valueCapacity ||
        snapshot.guardFeedback.size() > m_valueCapacity ||
        snapshot.opportunityAttackMoveQueryCommands.size() > m_valueCapacity ||
        snapshot.opportunityAttackMoveQueryFeedback.size() > m_valueCapacity ||
        snapshot.opportunityAttackMoveChildCommands.size() > m_valueCapacity ||
        snapshot.opportunityAttackMoveChildFeedback.size() > m_valueCapacity ||
        snapshot.tacticalAttackQueryCommands.size() > m_valueCapacity ||
        snapshot.tacticalAttackQueryFeedback.size() > m_valueCapacity ||
        snapshot.tacticalAttackChildCommands.size() > m_valueCapacity ||
        snapshot.tacticalAttackChildFeedback.size() > m_valueCapacity ||
        snapshot.dockRequests.size() > m_valueCapacity ||
        snapshot.dockFeedback.size() > m_valueCapacity ||
        snapshot.containmentCommands.size() > m_valueCapacity ||
        snapshot.containmentFeedback.size() > m_valueCapacity ||
        snapshot.insertionMotionCommands.size() > m_valueCapacity ||
        snapshot.insertionMotionFeedback.size() > m_valueCapacity ||
        snapshot.insertionContainmentCommands.size() > m_valueCapacity ||
        snapshot.insertionContainmentFeedback.size() > m_valueCapacity ||
        snapshot.insertionOperationCommands.size() > m_valueCapacity ||
        snapshot.insertionOperationFeedback.size() > m_valueCapacity ||
        snapshot.insertionEffectCommands.size() > m_valueCapacity)
    {
        return ObjectAITransientStatus::InvalidSnapshot;
    }
    for (size_t index = 1; index < snapshot.pathRequests.size(); ++index)
    {
        if (!(snapshot.pathRequests[index - 1].correlation.subject <
              snapshot.pathRequests[index].correlation.subject))
        {
            return ObjectAITransientStatus::InvalidSnapshot;
        }
    }
    if (!strictlySortedBySubject(snapshot.opportunityAttackMoveQueryCommands) ||
        !strictlySortedBySubject(snapshot.opportunityAttackMoveQueryFeedback) ||
        !strictlySortedBySubject(snapshot.opportunityAttackMoveChildCommands) ||
        !strictlySortedBySubject(snapshot.opportunityAttackMoveChildFeedback) ||
        !strictlySortedBySubject(snapshot.tacticalAttackQueryCommands) ||
        !strictlySortedBySubject(snapshot.tacticalAttackQueryFeedback) ||
        !strictlySortedBySubject(snapshot.tacticalAttackChildCommands) ||
        !strictlySortedBySubject(snapshot.tacticalAttackChildFeedback) ||
        !sortedByCorrelation(snapshot.dockRequests) ||
        !sortedByCorrelation(snapshot.dockFeedback) ||
        !sortedByCorrelation(snapshot.containmentCommands) ||
        !sortedByCorrelation(snapshot.containmentFeedback) ||
        !sortedByCorrelation(snapshot.insertionMotionCommands) ||
        !sortedByCorrelation(snapshot.insertionMotionFeedback) ||
        !sortedByCorrelation(snapshot.insertionContainmentCommands) ||
        !sortedByCorrelation(snapshot.insertionContainmentFeedback) ||
        !sortedByCorrelation(snapshot.insertionOperationCommands) ||
        !sortedByCorrelation(snapshot.insertionOperationFeedback) ||
        !sortedByCorrelation(snapshot.insertionEffectCommands))
    {
        return ObjectAITransientStatus::InvalidSnapshot;
    }

    ObjectAITransientStore candidate;
    if (candidate.initialize(m_actorCapacity, m_valueCapacity) !=
        ObjectAITransientStatus::Success)
    {
        return ObjectAITransientStatus::InvalidSnapshot;
    }
    for (const AIWakeEvent& event : snapshot.wakeEvents)
    {
        if (candidate.scheduleWake(event.object, event.wakeTick) !=
            AIWakeScheduleResult::Scheduled)
        {
            return ObjectAITransientStatus::InvalidSnapshot;
        }
    }
    const auto copyValues = [&candidate](const auto& values) {
        for (const auto& value : values)
        {
            if (candidate.stage(value) != ObjectAITransientStatus::Success)
                return false;
        }
        return true;
    };
    if (!copyValues(snapshot.facingCommands) ||
        !copyValues(snapshot.facingFeedback) ||
        !copyValues(snapshot.pathRequests) ||
        !copyValues(snapshot.pathFeedback) ||
        !copyValues(snapshot.movementCommands) ||
        !copyValues(snapshot.movementFeedback) ||
        !copyValues(snapshot.waypointCompletions) ||
        !copyValues(snapshot.attackCommands) ||
        !copyValues(snapshot.attackFeedback) ||
        !copyValues(snapshot.attackCompletions) ||
        !copyValues(snapshot.guardTacticalCommands) ||
        !copyValues(snapshot.guardInteractionCommands) ||
        !copyValues(snapshot.guardFeedback) ||
        !copyValues(snapshot.opportunityAttackMoveQueryCommands) ||
        !copyValues(snapshot.opportunityAttackMoveQueryFeedback) ||
        !copyValues(snapshot.opportunityAttackMoveChildCommands) ||
        !copyValues(snapshot.opportunityAttackMoveChildFeedback) ||
        !copyValues(snapshot.tacticalAttackQueryCommands) ||
        !copyValues(snapshot.tacticalAttackQueryFeedback) ||
        !copyValues(snapshot.tacticalAttackChildCommands) ||
        !copyValues(snapshot.tacticalAttackChildFeedback) ||
        !copyValues(snapshot.dockRequests) ||
        !copyValues(snapshot.dockFeedback) ||
        !copyValues(snapshot.containmentCommands) ||
        !copyValues(snapshot.containmentFeedback) ||
        !copyValues(snapshot.insertionMotionCommands) ||
        !copyValues(snapshot.insertionMotionFeedback) ||
        !copyValues(snapshot.insertionContainmentCommands) ||
        !copyValues(snapshot.insertionContainmentFeedback) ||
        !copyValues(snapshot.insertionOperationCommands) ||
        !copyValues(snapshot.insertionOperationFeedback) ||
        !copyValues(snapshot.insertionEffectCommands))
    {
        return ObjectAITransientStatus::InvalidSnapshot;
    }
    candidate.m_pathRequestSubmitted = snapshot.pathRequestSubmitted;
    candidate.m_pathRequestNextEligibleTick =
        snapshot.pathRequestNextEligibleTick;
    for (size_t index = 0;
         index < candidate.m_pathRequestSubmitted.size(); ++index)
    {
        const uint8_t submitted = candidate.m_pathRequestSubmitted[index];
        const uint64_t nextEligibleTick =
            candidate.m_pathRequestNextEligibleTick[index];
        if (submitted > 1 || (submitted != 0 && nextEligibleTick != 0))
            return ObjectAITransientStatus::InvalidSnapshot;
    }
    *this = std::move(candidate);
    return ObjectAITransientStatus::Success;
}

} // namespace engine::ai
