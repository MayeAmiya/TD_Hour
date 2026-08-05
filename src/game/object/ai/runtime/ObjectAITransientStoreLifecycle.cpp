#include "game/object/ai/runtime/ObjectAITransientStore.h"

namespace engine::ai
{

ObjectAITransientStatus ObjectAITransientStore::initialize(
    size_t actorCapacity, size_t valueCapacity)
{
    if (actorCapacity == 0 || valueCapacity == 0)
        return ObjectAITransientStatus::CapacityExceeded;
    m_actorCapacity = actorCapacity;
    m_valueCapacity = valueCapacity;
    m_wakeEvents.clear();
    m_wakeEvents.reserve(actorCapacity);
    reserveValues(valueCapacity);
    m_initialized = true;
    return ObjectAITransientStatus::Success;
}

void ObjectAITransientStore::reset() noexcept
{
    m_wakeEvents.clear();
    m_facingCommands.clear();
    m_facingFeedback.clear();
    m_pathRequests.clear();
    m_pathRequestSubmitted.clear();
    m_pathRequestNextEligibleTick.clear();
    m_pathFeedback.clear();
    m_movementCommands.clear();
    m_movementFeedback.clear();
    m_waypointCompletions.clear();
    m_attackCommands.clear();
    m_attackFeedback.clear();
    m_attackCompletions.clear();
    m_guardTacticalCommands.clear();
    m_guardInteractionCommands.clear();
    m_guardFeedback.clear();
    m_opportunityAttackMoveQueryCommands.clear();
    m_opportunityAttackMoveQueryFeedback.clear();
    m_opportunityAttackMoveChildCommands.clear();
    m_opportunityAttackMoveChildFeedback.clear();
    m_tacticalAttackQueryCommands.clear();
    m_tacticalAttackQueryFeedback.clear();
    m_tacticalAttackChildCommands.clear();
    m_tacticalAttackChildFeedback.clear();
    m_dockRequests.clear();
    m_dockFeedback.clear();
    m_containmentCommands.clear();
    m_containmentFeedback.clear();
    m_insertionMotionCommands.clear();
    m_insertionMotionFeedback.clear();
    m_insertionContainmentCommands.clear();
    m_insertionContainmentFeedback.clear();
    m_insertionOperationCommands.clear();
    m_insertionOperationFeedback.clear();
    m_insertionEffectCommands.clear();
    m_actorCapacity = 0;
    m_valueCapacity = 0;
    m_initialized = false;
}

AIWakeScheduleResult ObjectAITransientStore::scheduleWake(
    ObjectId subject, uint64_t wakeTick)
{
    if (!subject)
        return AIWakeScheduleResult::InvalidObjectId;
    const auto found = findWake(subject);
    if (found != m_wakeEvents.end())
        return AIWakeScheduleResult::AlreadyScheduled;
    if (m_wakeEvents.size() == m_actorCapacity)
        return AIWakeScheduleResult::CapacityExceeded;
    insertWake({subject, wakeTick});
    return AIWakeScheduleResult::Scheduled;
}

AIWakeScheduleResult ObjectAITransientStore::rescheduleWake(
    ObjectId subject, uint64_t wakeTick)
{
    if (!subject)
        return AIWakeScheduleResult::InvalidObjectId;
    const auto found = findWake(subject);
    if (found == m_wakeEvents.end())
        return AIWakeScheduleResult::NotScheduled;
    m_wakeEvents.erase(found);
    insertWake({subject, wakeTick});
    return AIWakeScheduleResult::Rescheduled;
}

bool ObjectAITransientStore::cancelWake(ObjectId subject) noexcept
{
    const auto found = findWake(subject);
    if (found == m_wakeEvents.end())
        return false;
    m_wakeEvents.erase(found);
    return true;
}

bool ObjectAITransientStore::hasWake(ObjectId subject) const noexcept
{
    return findWake(subject) != m_wakeEvents.end();
}

ObjectAITransientStatus ObjectAITransientStore::replaceWakeEvents(
    container::Span<const AIWakeEvent> events)
{
    if (!m_initialized)
        return ObjectAITransientStatus::NotInitialized;
    if (events.size() > m_actorCapacity)
        return ObjectAITransientStatus::CapacityExceeded;

    for (size_t index = 0; index < events.size(); ++index)
    {
        const AIWakeEvent& event = events[index];
        if (!event.object || event.wakeTick == 0)
            return ObjectAITransientStatus::InvalidValue;
        if (index != 0)
        {
            const AIWakeEvent& previous = events[index - 1];
            if (event.wakeTick < previous.wakeTick ||
                (event.wakeTick == previous.wakeTick &&
                 event.object < previous.object))
            {
                return ObjectAITransientStatus::InvalidValue;
            }
        }
    }
    m_wakeEvents.assign(events.begin(), events.end());
    return ObjectAITransientStatus::Success;
}

ObjectAITransientClearReport ObjectAITransientStore::clearSubject(
    ObjectId subject)
{
    ObjectAITransientClearReport report;
    report.wakeEvents = cancelWake(subject) ? 1 : 0;
    report.commands += eraseSubject(
        m_facingCommands, subject,
        [](const AIStateCommand& value) { return value.subject; });
    report.commands += cancelOrErasePathRequest(subject);
    report.commands += eraseSubject(
        m_movementCommands, subject,
        [](const MovementCommand& value) { return value.correlation.subject; });
    report.commands += eraseSubject(
        m_attackCommands, subject,
        [](const AIAttackCommand& value) { return value.correlation.subject; });
    report.commands += eraseSubject(
        m_guardTacticalCommands, subject,
        [](const AIGuardTacticalCommand& value) {
            return value.correlation.subject;
        });
    report.commands += eraseSubject(
        m_guardInteractionCommands, subject,
        [](const AIGuardInteractionCommand& value) {
            return value.correlation.subject;
        });
    report.commands += eraseSubject(
        m_opportunityAttackMoveQueryCommands, subject,
        [](const AIOpportunityAttackMoveQueryCommand& value) {
            return value.correlation.subject;
        });
    report.commands += eraseSubject(
        m_opportunityAttackMoveChildCommands, subject,
        [](const AIOpportunityAttackMoveChildCommand& value) {
            return value.correlation.subject;
        });
    report.commands += eraseSubject(
        m_tacticalAttackQueryCommands, subject,
        [](const AITacticalAttackQueryCommand& value) {
            return value.correlation.subject;
        });
    report.commands += eraseSubject(
        m_tacticalAttackChildCommands, subject,
        [](const AITacticalAttackChildCommand& value) {
            return value.correlation.subject;
        });
    report.commands += eraseSubject(
        m_dockRequests, subject,
        [](const AIDockRequest& value) {
            return value.correlation.token.subject;
        });
    report.commands += eraseSubject(
        m_containmentCommands, subject,
        [](const AIContainmentCommand& value) {
            return value.correlation.subject;
        });
    report.commands += eraseSubject(
        m_insertionMotionCommands, subject,
        [](const AIInsertionMotionCommand& value) {
            return value.correlation.subject;
        });
    report.commands += eraseSubject(
        m_insertionContainmentCommands, subject,
        [](const AIInsertionContainmentCommand& value) {
            return value.correlation.subject;
        });
    report.commands += eraseSubject(
        m_insertionOperationCommands, subject,
        [](const AIInsertionOperationCommand& value) {
            return value.correlation.subject;
        });
    report.commands += eraseSubject(
        m_insertionEffectCommands, subject,
        [](const AIInsertionEffectCommand& value) {
            return value.correlation.subject;
        });

    report.feedback += eraseSubject(
        m_facingFeedback, subject,
        [](const AIFacingFeedback& value) { return value.subject; });
    report.feedback += eraseSubject(
        m_pathFeedback, subject,
        [](const PathFeedback& value) { return value.correlation.subject; });
    report.feedback += eraseSubject(
        m_movementFeedback, subject,
        [](const MovementFeedback& value) { return value.correlation.subject; });
    report.feedback += eraseSubject(
        m_waypointCompletions, subject,
        [](const AIWaypointCompletionEvent& value) { return value.subject; });
    report.feedback += eraseSubject(
        m_attackFeedback, subject,
        [](const AIAttackFeedback& value) { return value.correlation.subject; });
    report.feedback += eraseSubject(
        m_attackCompletions, subject,
        [](const AIAttackOrderCompletion& value) {
            return value.correlation.subject;
        });
    report.feedback += eraseSubject(
        m_guardFeedback, subject,
        [](const AIGuardFeedback& value) { return value.correlation.subject; });
    report.feedback += eraseSubject(
        m_opportunityAttackMoveQueryFeedback, subject,
        [](const AIOpportunityAttackMoveQueryFeedback& value) {
            return value.correlation.subject;
        });
    report.feedback += eraseSubject(
        m_opportunityAttackMoveChildFeedback, subject,
        [](const AIOpportunityAttackMoveChildFeedback& value) {
            return value.correlation.subject;
        });
    report.feedback += eraseSubject(
        m_tacticalAttackQueryFeedback, subject,
        [](const AITacticalAttackQueryFeedback& value) {
            return value.correlation.subject;
        });
    report.feedback += eraseSubject(
        m_tacticalAttackChildFeedback, subject,
        [](const AITacticalAttackChildFeedback& value) {
            return value.correlation.subject;
        });
    report.feedback += eraseSubject(
        m_dockFeedback, subject,
        [](const AIDockFeedback& value) {
            return value.correlation.token.subject;
        });
    report.feedback += eraseSubject(
        m_containmentFeedback, subject,
        [](const AIContainmentFeedback& value) {
            return value.correlation.subject;
        });
    report.feedback += eraseSubject(
        m_insertionMotionFeedback, subject,
        [](const AIInsertionMotionFeedback& value) {
            return value.correlation.subject;
        });
    report.feedback += eraseSubject(
        m_insertionContainmentFeedback, subject,
        [](const AIInsertionContainmentFeedback& value) {
            return value.correlation.subject;
        });
    report.feedback += eraseSubject(
        m_insertionOperationFeedback, subject,
        [](const AIInsertionOperationFeedback& value) {
            return value.correlation.subject;
        });
    return report;
}

bool ObjectAITransientStore::emptyCorrelatedValuesFor(
    ObjectId subject) const noexcept
{
    return countSubject(m_facingCommands, subject, [](const AIStateCommand& value) { return value.subject; }) == 0 &&
        countSubject(m_facingFeedback, subject, [](const AIFacingFeedback& value) { return value.subject; }) == 0 &&
        countSubject(m_pathRequests, subject, [](const PathRequest& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_pathFeedback, subject, [](const PathFeedback& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_movementCommands, subject, [](const MovementCommand& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_movementFeedback, subject, [](const MovementFeedback& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_waypointCompletions, subject, [](const AIWaypointCompletionEvent& value) { return value.subject; }) == 0 &&
        countSubject(m_attackCommands, subject, [](const AIAttackCommand& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_attackFeedback, subject, [](const AIAttackFeedback& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_attackCompletions, subject, [](const AIAttackOrderCompletion& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_guardTacticalCommands, subject, [](const AIGuardTacticalCommand& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_guardInteractionCommands, subject, [](const AIGuardInteractionCommand& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_guardFeedback, subject, [](const AIGuardFeedback& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_opportunityAttackMoveQueryCommands, subject, [](const AIOpportunityAttackMoveQueryCommand& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_opportunityAttackMoveQueryFeedback, subject, [](const AIOpportunityAttackMoveQueryFeedback& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_opportunityAttackMoveChildCommands, subject, [](const AIOpportunityAttackMoveChildCommand& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_opportunityAttackMoveChildFeedback, subject, [](const AIOpportunityAttackMoveChildFeedback& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_tacticalAttackQueryCommands, subject, [](const AITacticalAttackQueryCommand& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_tacticalAttackQueryFeedback, subject, [](const AITacticalAttackQueryFeedback& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_tacticalAttackChildCommands, subject, [](const AITacticalAttackChildCommand& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_tacticalAttackChildFeedback, subject, [](const AITacticalAttackChildFeedback& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_dockRequests, subject, [](const AIDockRequest& value) { return value.correlation.token.subject; }) == 0 &&
        countSubject(m_dockFeedback, subject, [](const AIDockFeedback& value) { return value.correlation.token.subject; }) == 0 &&
        countSubject(m_containmentCommands, subject, [](const AIContainmentCommand& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_containmentFeedback, subject, [](const AIContainmentFeedback& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_insertionMotionCommands, subject, [](const AIInsertionMotionCommand& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_insertionMotionFeedback, subject, [](const AIInsertionMotionFeedback& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_insertionContainmentCommands, subject, [](const AIInsertionContainmentCommand& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_insertionContainmentFeedback, subject, [](const AIInsertionContainmentFeedback& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_insertionOperationCommands, subject, [](const AIInsertionOperationCommand& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_insertionOperationFeedback, subject, [](const AIInsertionOperationFeedback& value) { return value.correlation.subject; }) == 0 &&
        countSubject(m_insertionEffectCommands, subject, [](const AIInsertionEffectCommand& value) { return value.correlation.subject; }) == 0;
}

bool ObjectAITransientStore::emptyFor(ObjectId subject) const noexcept
{
    return !hasWake(subject) && emptyCorrelatedValuesFor(subject);
}

void ObjectAITransientStore::discardFeedback() noexcept
{
    m_facingFeedback.clear();
    m_pathFeedback.clear();
    m_movementFeedback.clear();
    m_attackFeedback.clear();
    m_guardFeedback.clear();
    m_opportunityAttackMoveQueryFeedback.clear();
    m_opportunityAttackMoveChildFeedback.clear();
    m_tacticalAttackQueryFeedback.clear();
    m_tacticalAttackChildFeedback.clear();
    m_dockFeedback.clear();
    m_containmentFeedback.clear();
    m_insertionMotionFeedback.clear();
    m_insertionContainmentFeedback.clear();
    m_insertionOperationFeedback.clear();
}

void ObjectAITransientStore::reserveValues(size_t capacity)
{
    m_facingCommands.clear(); m_facingCommands.reserve(capacity);
    m_facingFeedback.clear(); m_facingFeedback.reserve(capacity);
    m_pathRequests.clear(); m_pathRequests.reserve(capacity);
    m_pathRequestSubmitted.clear(); m_pathRequestSubmitted.reserve(capacity);
    m_pathRequestNextEligibleTick.clear();
    m_pathRequestNextEligibleTick.reserve(capacity);
    m_pathFeedback.clear(); m_pathFeedback.reserve(capacity);
    m_movementCommands.clear(); m_movementCommands.reserve(capacity);
    m_movementFeedback.clear(); m_movementFeedback.reserve(capacity);
    m_waypointCompletions.clear(); m_waypointCompletions.reserve(capacity);
    m_attackCommands.clear(); m_attackCommands.reserve(capacity);
    m_attackFeedback.clear(); m_attackFeedback.reserve(capacity);
    m_attackCompletions.clear(); m_attackCompletions.reserve(capacity);
    m_guardTacticalCommands.clear(); m_guardTacticalCommands.reserve(capacity);
    m_guardInteractionCommands.clear(); m_guardInteractionCommands.reserve(capacity);
    m_guardFeedback.clear(); m_guardFeedback.reserve(capacity);
    m_opportunityAttackMoveQueryCommands.clear();
    m_opportunityAttackMoveQueryCommands.reserve(capacity);
    m_opportunityAttackMoveQueryFeedback.clear();
    m_opportunityAttackMoveQueryFeedback.reserve(capacity);
    m_opportunityAttackMoveChildCommands.clear();
    m_opportunityAttackMoveChildCommands.reserve(capacity);
    m_opportunityAttackMoveChildFeedback.clear();
    m_opportunityAttackMoveChildFeedback.reserve(capacity);
    m_tacticalAttackQueryCommands.clear();
    m_tacticalAttackQueryCommands.reserve(capacity);
    m_tacticalAttackQueryFeedback.clear();
    m_tacticalAttackQueryFeedback.reserve(capacity);
    m_tacticalAttackChildCommands.clear();
    m_tacticalAttackChildCommands.reserve(capacity);
    m_tacticalAttackChildFeedback.clear();
    m_tacticalAttackChildFeedback.reserve(capacity);
    m_dockRequests.clear(); m_dockRequests.reserve(capacity);
    m_dockFeedback.clear(); m_dockFeedback.reserve(capacity);
    m_containmentCommands.clear(); m_containmentCommands.reserve(capacity);
    m_containmentFeedback.clear(); m_containmentFeedback.reserve(capacity);
    m_insertionMotionCommands.clear();
    m_insertionMotionCommands.reserve(capacity);
    m_insertionMotionFeedback.clear();
    m_insertionMotionFeedback.reserve(capacity);
    m_insertionContainmentCommands.clear();
    m_insertionContainmentCommands.reserve(capacity);
    m_insertionContainmentFeedback.clear();
    m_insertionContainmentFeedback.reserve(capacity);
    m_insertionOperationCommands.clear();
    m_insertionOperationCommands.reserve(capacity);
    m_insertionOperationFeedback.clear();
    m_insertionOperationFeedback.reserve(capacity);
    m_insertionEffectCommands.clear();
    m_insertionEffectCommands.reserve(capacity);
}

container::Vector<AIWakeEvent>::iterator ObjectAITransientStore::findWake(
    ObjectId subject) noexcept
{
    return std::find_if(
        m_wakeEvents.begin(), m_wakeEvents.end(),
        [subject](const AIWakeEvent& event) { return event.object == subject; });
}

container::Vector<AIWakeEvent>::const_iterator ObjectAITransientStore::findWake(
    ObjectId subject) const noexcept
{
    return std::find_if(
        m_wakeEvents.begin(), m_wakeEvents.end(),
        [subject](const AIWakeEvent& event) { return event.object == subject; });
}

void ObjectAITransientStore::insertWake(AIWakeEvent event)
{
    const auto position = std::lower_bound(
        m_wakeEvents.begin(), m_wakeEvents.end(), event,
        [](const AIWakeEvent& left, const AIWakeEvent& right) {
            return left.wakeTick != right.wakeTick
                ? left.wakeTick < right.wakeTick
                : left.object < right.object;
        });
    m_wakeEvents.insert(position, event);
}

size_t ObjectAITransientStore::cancelOrErasePathRequest(
    ObjectId subject) noexcept
{
    for (size_t index = 0; index < m_pathRequests.size(); ++index)
    {
        PathRequest& request = m_pathRequests[index];
        if (request.correlation.subject != subject)
            continue;
        if (request.kind == PathRequestKind::Cancel)
            return 0;
        if (m_pathRequestSubmitted[index] != 0)
        {
            request.kind = PathRequestKind::Cancel;
            request.currentPath = {};
            m_pathRequestSubmitted[index] = 0;
            m_pathRequestNextEligibleTick[index] = 0;
            return 1;
        }
        m_pathRequests.erase(
            m_pathRequests.begin() + static_cast<std::ptrdiff_t>(index));
        m_pathRequestSubmitted.erase(
            m_pathRequestSubmitted.begin() + static_cast<std::ptrdiff_t>(index));
        m_pathRequestNextEligibleTick.erase(
            m_pathRequestNextEligibleTick.begin() +
            static_cast<std::ptrdiff_t>(index));
        return 1;
    }
    return 0;
}

} // namespace engine::ai
