#include "game/object/ai/runtime/ObjectAITransientStore.h"

#include <iterator>

namespace engine::ai
{

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIStateCommand& value)
{
    if (!value.subject || !value.request.isValid())
        return ObjectAITransientStatus::InvalidValue;
    const auto found = std::find_if(
        m_facingCommands.begin(), m_facingCommands.end(),
        [&value](const AIStateCommand& command) {
            return command.subject == value.subject &&
                command.request == value.request;
        });
    if (found != m_facingCommands.end())
    {
        return *found == value
            ? ObjectAITransientStatus::Success
            : ObjectAITransientStatus::InvalidValue;
    }
    return push(m_facingCommands, value);
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIFacingFeedback& value)
{
    return value.subject && value.request.isValid()
        ? push(m_facingFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(const PathRequest& value)
{
    if (!value.correlation.isValid())
        return ObjectAITransientStatus::InvalidValue;
    if (!m_initialized)
        return ObjectAITransientStatus::NotInitialized;

    // Navigation owns at most one queued/active/feedback operation per
    // subject. Mirror that ownership here so a sleeping path state can
    // harmlessly repeat the same value without growing the transient store,
    // while a repath generation deterministically supersedes the previous
    // correlation and becomes eligible for one new submit.
    const auto found = std::lower_bound(
        m_pathRequests.begin(), m_pathRequests.end(),
        value.correlation.subject,
        [](const PathRequest& request, ObjectId subject) {
            return request.correlation.subject < subject;
        });
    const size_t index = static_cast<size_t>(
        std::distance(m_pathRequests.begin(), found));
    if (found != m_pathRequests.end() &&
        found->correlation.subject == value.correlation.subject)
    {
        if (found->correlation == value.correlation)
        {
            if (*found == value)
                return ObjectAITransientStatus::Success;
            // A state may keep its correlation while changing the desired
            // destination or path policy (for example after a script order
            // is retargeted). Treat the new value as the authoritative
            // replacement instead of rejecting the whole AI output. Reset
            // submission gates so the navigation adapter sees the update.
            *found = value;
            m_pathRequestSubmitted[index] = 0;
            m_pathRequestNextEligibleTick[index] = 0;
            return ObjectAITransientStatus::Success;
        }
        *found = value;
        m_pathRequestSubmitted[index] = 0;
        m_pathRequestNextEligibleTick[index] = 0;
        return ObjectAITransientStatus::Success;
    }
    if (m_pathRequests.size() == m_valueCapacity)
        return ObjectAITransientStatus::CapacityExceeded;
    m_pathRequests.insert(found, value);
    m_pathRequestSubmitted.insert(
        m_pathRequestSubmitted.begin() + static_cast<std::ptrdiff_t>(index), 0);
    m_pathRequestNextEligibleTick.insert(
        m_pathRequestNextEligibleTick.begin() +
            static_cast<std::ptrdiff_t>(index),
        0);
    return ObjectAITransientStatus::Success;
}

ObjectAITransientStatus ObjectAITransientStore::stage(const PathFeedback& value)
{
    return value.correlation.isValid()
        ? push(m_pathFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const MovementCommand& value)
{
    if (!value.correlation.isValid() ||
        static_cast<uint8_t>(value.mode) >
            static_cast<uint8_t>(AIMovementMode::Panic) ||
        value.panicking != (value.mode == AIMovementMode::Panic))
        return ObjectAITransientStatus::InvalidValue;
    if (!m_initialized)
        return ObjectAITransientStatus::NotInitialized;
    const auto found = std::lower_bound(
        m_movementCommands.begin(), m_movementCommands.end(),
        value.correlation.subject,
        [](const MovementCommand& command, ObjectId subject) {
            return command.correlation.subject < subject;
        });
    if (found != m_movementCommands.end() &&
        found->correlation.subject == value.correlation.subject)
    {
        if (found->correlation == value.correlation && *found == value)
            return ObjectAITransientStatus::Success;
        *found = value;
        return ObjectAITransientStatus::Success;
    }
    if (m_movementCommands.size() == m_valueCapacity)
        return ObjectAITransientStatus::CapacityExceeded;
    m_movementCommands.insert(found, value);
    return ObjectAITransientStatus::Success;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const MovementFeedback& value)
{
    if (!value.correlation.isValid())
        return ObjectAITransientStatus::InvalidValue;
    if (!m_initialized)
        return ObjectAITransientStatus::NotInitialized;
    const auto found = std::lower_bound(
        m_movementFeedback.begin(), m_movementFeedback.end(),
        value.correlation.subject,
        [](const MovementFeedback& feedback, ObjectId subject) {
            return feedback.correlation.subject < subject;
        });
    if (found != m_movementFeedback.end() &&
        found->correlation.subject == value.correlation.subject)
    {
        *found = value;
        return ObjectAITransientStatus::Success;
    }
    if (m_movementFeedback.size() == m_valueCapacity)
        return ObjectAITransientStatus::CapacityExceeded;
    m_movementFeedback.insert(found, value);
    return ObjectAITransientStatus::Success;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIWaypointCompletionEvent& value)
{
    return value.isValid()
        ? push(m_waypointCompletions, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIAttackCommand& value)
{
    return value.correlation.isValid()
        ? push(m_attackCommands, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIAttackFeedback& value)
{
    return value.correlation.isValid()
        ? push(m_attackFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIAttackOrderCompletion& value)
{
    return value.isValid()
        ? push(m_attackCompletions, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIGuardTacticalCommand& value)
{
    return value.correlation.isValid()
        ? push(m_guardTacticalCommands, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIGuardInteractionCommand& value)
{
    return value.correlation.isValid()
        ? push(m_guardInteractionCommands, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIGuardFeedback& value)
{
    return value.correlation.isValid() &&
            value.kind != AIGuardFeedbackKind::None
        ? push(m_guardFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIOpportunityAttackMoveQueryCommand& value)
{
    return value.correlation.isValid()
        ? stageSortedBySubject(m_opportunityAttackMoveQueryCommands, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIOpportunityAttackMoveQueryFeedback& value)
{
    return value.correlation.isValid()
        ? stageSortedBySubject(m_opportunityAttackMoveQueryFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIOpportunityAttackMoveChildCommand& value)
{
    return value.correlation.isValid()
        ? stageSortedBySubject(m_opportunityAttackMoveChildCommands, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIOpportunityAttackMoveChildFeedback& value)
{
    return value.correlation.isValid()
        ? stageSortedBySubject(m_opportunityAttackMoveChildFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AITacticalAttackQueryCommand& value)
{
    return value.correlation.isValid()
        ? stageSortedBySubject(m_tacticalAttackQueryCommands, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AITacticalAttackQueryFeedback& value)
{
    return value.correlation.isValid()
        ? stageSortedBySubject(m_tacticalAttackQueryFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AITacticalAttackChildCommand& value)
{
    const bool valid = value.kind == AITacticalAttackChildCommandKind::EndWrapper
        ? value.correlation.isWrapperValid()
        : value.correlation.isChildValid();
    return valid
        ? stageSortedBySubject(m_tacticalAttackChildCommands, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AITacticalAttackChildFeedback& value)
{
    return value.correlation.isChildValid()
        ? stageSortedBySubject(m_tacticalAttackChildFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIDockRequest& value)
{
    return value.correlation.isValid() && value.kind != AIDockRequestKind::None
        ? stageSortedByCorrelation(m_dockRequests, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIDockFeedback& value)
{
    return value.correlation.isValid() &&
            value.request != AIDockRequestKind::None &&
            value.status != AIDockFeedbackStatus::None
        ? stageSortedByCorrelation(m_dockFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIContainmentCommand& value)
{
    return value.correlation.isValid()
        ? stageSortedByCorrelation(m_containmentCommands, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIContainmentFeedback& value)
{
    return value.correlation.isValid() &&
            value.kind != AIContainmentFeedbackKind::None
        ? stageSortedByCorrelation(m_containmentFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIInsertionMotionCommand& value)
{
    return value.correlation.isValid()
        ? stageSortedByCorrelation(m_insertionMotionCommands, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIInsertionMotionFeedback& value)
{
    return value.correlation.isValid() &&
            value.kind != AIInsertionMotionFeedbackKind::None
        ? stageSortedByCorrelation(m_insertionMotionFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIInsertionContainmentCommand& value)
{
    return value.correlation.isValid()
        ? stageSortedByCorrelation(m_insertionContainmentCommands, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIInsertionContainmentFeedback& value)
{
    return value.correlation.isValid() &&
            value.kind != AIInsertionContainmentFeedbackKind::None
        ? stageSortedByCorrelation(m_insertionContainmentFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIInsertionOperationCommand& value)
{
    return value.correlation.isValid()
        ? stageSortedByCorrelation(m_insertionOperationCommands, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIInsertionOperationFeedback& value)
{
    return value.correlation.isValid() &&
            value.kind != AIInsertionOperationFeedbackKind::None
        ? stageSortedByCorrelation(m_insertionOperationFeedback, value)
        : ObjectAITransientStatus::InvalidValue;
}

ObjectAITransientStatus ObjectAITransientStore::stage(
    const AIInsertionEffectCommand& value)
{
    return value.correlation.isValid()
        ? stageSortedByCorrelation(m_insertionEffectCommands, value)
        : ObjectAITransientStatus::InvalidValue;
}

container::Span<const AIWakeEvent> ObjectAITransientStore::wakeEvents() const noexcept
{
    return m_wakeEvents;
}

container::Span<const AIStateCommand> ObjectAITransientStore::facingCommands() const noexcept
{
    return m_facingCommands;
}

void ObjectAITransientStore::discardFacingCommands() noexcept
{
    m_facingCommands.clear();
}

bool ObjectAITransientStore::removeFacingCommand(
    ObjectId subject, AIStateRequestId request) noexcept
{
    const auto found = std::find_if(
        m_facingCommands.begin(), m_facingCommands.end(),
        [subject, request](const AIStateCommand& command) {
            return command.subject == subject && command.request == request;
        });
    if (found == m_facingCommands.end())
        return false;
    m_facingCommands.erase(found);
    return true;
}

container::Span<const AIFacingFeedback> ObjectAITransientStore::facingFeedback() const noexcept
{
    return m_facingFeedback;
}

container::Span<const PathRequest> ObjectAITransientStore::pathRequests() const noexcept
{
    return m_pathRequests;
}

container::Span<const uint8_t> ObjectAITransientStore::pathRequestSubmitted() const noexcept
{
    return m_pathRequestSubmitted;
}

container::Span<const uint64_t>
ObjectAITransientStore::pathRequestNextEligibleTicks() const noexcept
{
    return m_pathRequestNextEligibleTick;
}

bool ObjectAITransientStore::canStagePathFeedback() const noexcept
{
    return m_initialized && m_pathFeedback.size() < m_valueCapacity;
}

bool ObjectAITransientStore::markPathRequestSubmitted(
    const PathCorrelation& correlation) noexcept
{
    for (size_t index = 0; index < m_pathRequests.size(); ++index)
    {
        if (m_pathRequests[index].correlation == correlation)
        {
            m_pathRequestSubmitted[index] = 1;
            m_pathRequestNextEligibleTick[index] = 0;
            return true;
        }
    }
    return false;
}

bool ObjectAITransientStore::deferPathRequest(
    const PathCorrelation& correlation,
    uint64_t nextEligibleTick,
    uint64_t confirmedTick) noexcept
{
    if (nextEligibleTick <= confirmedTick)
        return false;
    for (size_t index = 0; index < m_pathRequests.size(); ++index)
    {
        if (!(m_pathRequests[index].correlation == correlation))
            continue;
        // The retained request keeps the complete original payload and
        // correlation. Eligibility is an absolute confirmed-tick boundary,
        // so replay/restore does not depend on wall-clock timing.
        m_pathRequestSubmitted[index] = 0;
        m_pathRequestNextEligibleTick[index] = nextEligibleTick;
        return true;
    }
    return false;
}

bool ObjectAITransientStore::removePathRequest(
    const PathCorrelation& correlation) noexcept
{
    for (size_t index = 0; index < m_pathRequests.size(); ++index)
    {
        if (!(m_pathRequests[index].correlation == correlation))
            continue;
        m_pathRequests.erase(
            m_pathRequests.begin() + static_cast<std::ptrdiff_t>(index));
        m_pathRequestSubmitted.erase(
            m_pathRequestSubmitted.begin() + static_cast<std::ptrdiff_t>(index));
        m_pathRequestNextEligibleTick.erase(
            m_pathRequestNextEligibleTick.begin() +
            static_cast<std::ptrdiff_t>(index));
        return true;
    }
    return false;
}

container::Span<const PathFeedback> ObjectAITransientStore::pathFeedback() const noexcept
{
    return m_pathFeedback;
}

container::Span<const MovementCommand> ObjectAITransientStore::movementCommands() const noexcept
{
    return m_movementCommands;
}

void ObjectAITransientStore::discardMovementCommands() noexcept
{
    m_movementCommands.clear();
}

container::Span<const MovementFeedback> ObjectAITransientStore::movementFeedback() const noexcept
{
    return m_movementFeedback;
}

container::Span<const AIWaypointCompletionEvent>
ObjectAITransientStore::waypointCompletions() const noexcept
{
    return m_waypointCompletions;
}

void ObjectAITransientStore::discardWaypointCompletions() noexcept
{
    m_waypointCompletions.clear();
}

container::Span<const AIAttackCommand> ObjectAITransientStore::attackCommands() const noexcept
{
    return m_attackCommands;
}

void ObjectAITransientStore::discardAttackCommands() noexcept
{
    m_attackCommands.clear();
}

container::Span<const AIAttackFeedback> ObjectAITransientStore::attackFeedback() const noexcept
{
    return m_attackFeedback;
}

container::Span<const AIAttackOrderCompletion> ObjectAITransientStore::attackCompletions() const noexcept
{
    return m_attackCompletions;
}

void ObjectAITransientStore::discardAttackCompletions() noexcept
{
    m_attackCompletions.clear();
}

container::Span<const AIGuardTacticalCommand> ObjectAITransientStore::guardTacticalCommands() const noexcept
{
    return m_guardTacticalCommands;
}

void ObjectAITransientStore::discardGuardTacticalCommands() noexcept
{
    m_guardTacticalCommands.clear();
}

bool ObjectAITransientStore::removeGuardTacticalCommand(
    const AIGuardCorrelation& correlation,
    AIGuardTacticalCommandKind kind) noexcept
{
    const auto found = std::find_if(
        m_guardTacticalCommands.begin(), m_guardTacticalCommands.end(),
        [&correlation, kind](const AIGuardTacticalCommand& value) {
            return value.correlation == correlation && value.kind == kind;
        });
    if (found == m_guardTacticalCommands.end())
        return false;
    m_guardTacticalCommands.erase(found);
    return true;
}

container::Span<const AIGuardInteractionCommand> ObjectAITransientStore::guardInteractionCommands() const noexcept
{
    return m_guardInteractionCommands;
}

void ObjectAITransientStore::discardGuardInteractionCommands() noexcept
{
    m_guardInteractionCommands.clear();
}

bool ObjectAITransientStore::removeGuardInteractionCommand(
    const AIGuardCorrelation& correlation,
    AIGuardInteractionCommandKind kind) noexcept
{
    const auto found = std::find_if(
        m_guardInteractionCommands.begin(), m_guardInteractionCommands.end(),
        [&correlation, kind](const AIGuardInteractionCommand& value) {
            return value.correlation == correlation && value.kind == kind;
        });
    if (found == m_guardInteractionCommands.end())
        return false;
    m_guardInteractionCommands.erase(found);
    return true;
}

container::Span<const AIGuardFeedback> ObjectAITransientStore::guardFeedback() const noexcept
{
    return m_guardFeedback;
}

container::Span<const AIOpportunityAttackMoveQueryCommand>
ObjectAITransientStore::opportunityAttackMoveQueryCommands() const noexcept
{
    return m_opportunityAttackMoveQueryCommands;
}

void ObjectAITransientStore::discardOpportunityAttackMoveQueryCommands() noexcept
{
    m_opportunityAttackMoveQueryCommands.clear();
}

container::Span<const AIOpportunityAttackMoveQueryFeedback>
ObjectAITransientStore::opportunityAttackMoveQueryFeedback() const noexcept
{
    return m_opportunityAttackMoveQueryFeedback;
}

container::Span<const AIOpportunityAttackMoveChildCommand>
ObjectAITransientStore::opportunityAttackMoveChildCommands() const noexcept
{
    return m_opportunityAttackMoveChildCommands;
}

void ObjectAITransientStore::discardOpportunityAttackMoveChildCommands() noexcept
{
    m_opportunityAttackMoveChildCommands.clear();
}

container::Span<const AIOpportunityAttackMoveChildFeedback>
ObjectAITransientStore::opportunityAttackMoveChildFeedback() const noexcept
{
    return m_opportunityAttackMoveChildFeedback;
}

container::Span<const AITacticalAttackQueryCommand>
ObjectAITransientStore::tacticalAttackQueryCommands() const noexcept
{
    return m_tacticalAttackQueryCommands;
}

void ObjectAITransientStore::discardTacticalAttackQueryCommands() noexcept
{
    m_tacticalAttackQueryCommands.clear();
}

container::Span<const AITacticalAttackQueryFeedback>
ObjectAITransientStore::tacticalAttackQueryFeedback() const noexcept
{
    return m_tacticalAttackQueryFeedback;
}

container::Span<const AITacticalAttackChildCommand>
ObjectAITransientStore::tacticalAttackChildCommands() const noexcept
{
    return m_tacticalAttackChildCommands;
}

void ObjectAITransientStore::discardTacticalAttackChildCommands() noexcept
{
    m_tacticalAttackChildCommands.clear();
}

container::Span<const AITacticalAttackChildFeedback>
ObjectAITransientStore::tacticalAttackChildFeedback() const noexcept
{
    return m_tacticalAttackChildFeedback;
}

container::Span<const AIDockRequest> ObjectAITransientStore::dockRequests() const noexcept
{
    return m_dockRequests;
}

void ObjectAITransientStore::discardDockRequests() noexcept
{
    m_dockRequests.clear();
}

bool ObjectAITransientStore::removeDockRequest(
    const AIDockCorrelation& correlation, AIDockRequestKind kind) noexcept
{
    const auto found = std::find_if(
        m_dockRequests.begin(), m_dockRequests.end(),
        [&correlation, kind](const AIDockRequest& request) {
            return request.correlation == correlation && request.kind == kind;
        });
    if (found == m_dockRequests.end())
        return false;
    m_dockRequests.erase(found);
    return true;
}

container::Span<const AIDockFeedback> ObjectAITransientStore::dockFeedback() const noexcept
{
    return m_dockFeedback;
}

container::Span<const AIContainmentCommand> ObjectAITransientStore::containmentCommands() const noexcept
{
    return m_containmentCommands;
}

void ObjectAITransientStore::discardContainmentCommands() noexcept
{
    m_containmentCommands.clear();
}

container::Span<const AIContainmentFeedback> ObjectAITransientStore::containmentFeedback() const noexcept
{
    return m_containmentFeedback;
}

container::Span<const AIInsertionMotionCommand> ObjectAITransientStore::insertionMotionCommands() const noexcept
{
    return m_insertionMotionCommands;
}

void ObjectAITransientStore::discardInsertionMotionCommands() noexcept
{
    m_insertionMotionCommands.clear();
}

container::Span<const AIInsertionMotionFeedback> ObjectAITransientStore::insertionMotionFeedback() const noexcept
{
    return m_insertionMotionFeedback;
}

container::Span<const AIInsertionContainmentCommand>
ObjectAITransientStore::insertionContainmentCommands() const noexcept
{
    return m_insertionContainmentCommands;
}

void ObjectAITransientStore::discardInsertionContainmentCommands() noexcept
{
    m_insertionContainmentCommands.clear();
}

container::Span<const AIInsertionContainmentFeedback>
ObjectAITransientStore::insertionContainmentFeedback() const noexcept
{
    return m_insertionContainmentFeedback;
}

container::Span<const AIInsertionOperationCommand>
ObjectAITransientStore::insertionOperationCommands() const noexcept
{
    return m_insertionOperationCommands;
}

void ObjectAITransientStore::discardInsertionOperationCommands() noexcept
{
    m_insertionOperationCommands.clear();
}

container::Span<const AIInsertionOperationFeedback>
ObjectAITransientStore::insertionOperationFeedback() const noexcept
{
    return m_insertionOperationFeedback;
}

container::Span<const AIInsertionEffectCommand>
ObjectAITransientStore::insertionEffectCommands() const noexcept
{
    return m_insertionEffectCommands;
}

void ObjectAITransientStore::discardInsertionEffectCommands() noexcept
{
    m_insertionEffectCommands.clear();
}

} // namespace engine::ai
