#include "game/object/ai/runtime/ObjectAIRuntime.h"

namespace engine::ai
{

void ObjectAIRuntime::scatterFeedback(ObjectAIShadowTickReport& report) noexcept
{
    for (const AIFacingFeedback& value : m_transients.facingFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.subject);
        if (!handle)
            continue;
        if (value.orderIdentity.isValid() && !acceptsAsyncOrder(*handle, value.orderIdentity))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        m_shadowBatches[handle->batch].columns().facingFeedback[handle->slot] = value;
        ++report.feedbackValuesRead;
    }
    for (const PathFeedback& value : m_transients.pathFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.subject);
        if (!handle)
            continue;
        if (!acceptsAsyncOrder(*handle, value.correlation.orderIdentity))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        PathFeedback kernelValue = value;
        kernelValue.correlation.orderIdentity = {};
        m_shadowBatches[handle->batch].columns().pathFeedback[handle->slot] = kernelValue;
        ++report.feedbackValuesRead;
    }
    for (const MovementFeedback& value : m_transients.movementFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.subject);
        if (!handle)
            continue;
        if (!acceptsAsyncOrder(*handle, value.correlation.orderIdentity))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        MovementFeedback kernelValue = value;
        kernelValue.correlation.orderIdentity = {};
        m_shadowBatches[handle->batch].columns().movementFeedback[handle->slot] = kernelValue;
        ++report.feedbackValuesRead;
    }
    for (const AIAttackFeedback& value : m_transients.attackFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.subject);
        if (!handle)
            continue;
        if (!acceptsAsyncOrder(*handle, value.correlation.orderIdentity))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        AIAttackFeedbackBuffer& inbox = m_shadowBatches[handle->batch].columns().attackFeedback[handle->slot];
        AIAttackFeedback kernelValue = value;
        kernelValue.correlation.orderIdentity = {};
        if (!inbox.push(kernelValue))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
    for (const AITacticalAttackQueryFeedback& value : m_transients.tacticalAttackQueryFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.subject);
        if (!handle || !acceptsAsyncOrder(*handle, value.correlation.orderIdentity))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        AITacticalAttackQueryFeedbackBuffer& inbox =
            m_shadowBatches[handle->batch].columns().tacticalQueryFeedback[handle->slot];
        AITacticalAttackQueryFeedback kernelValue = value;
        kernelValue.correlation.orderIdentity = {};
        if (!inbox.push(kernelValue))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
    for (const AIGuardFeedback& value : m_transients.guardFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.subject);
        if (!handle || !acceptsAsyncOrder(*handle, value.correlation.orderIdentity))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        AIGuardFeedbackBuffer& inbox = m_shadowBatches[handle->batch].columns().guardFeedback[handle->slot];
        AIGuardFeedback kernelValue = value;
        kernelValue.correlation.orderIdentity = {};
        if (!inbox.push(kernelValue))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
    for (const AITacticalAttackChildFeedback& value : m_transients.tacticalAttackChildFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.subject);
        if (!handle || !acceptsAsyncOrder(*handle, value.correlation.orderIdentity))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        AITacticalAttackChildFeedbackBuffer& inbox =
            m_shadowBatches[handle->batch].columns().tacticalChildFeedback[handle->slot];
        AITacticalAttackChildFeedback kernelValue = value;
        kernelValue.correlation.orderIdentity = {};
        if (!inbox.push(kernelValue))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
    for (const AIOpportunityAttackMoveQueryFeedback& value : m_transients.opportunityAttackMoveQueryFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.subject);
        if (!handle || !acceptsAsyncOrder(*handle, value.correlation.orderIdentity))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        AIOpportunityAttackMoveQueryFeedbackBuffer& inbox =
            m_shadowBatches[handle->batch].columns().opportunityQueryFeedback[handle->slot];
        AIOpportunityAttackMoveQueryFeedback kernelValue = value;
        kernelValue.correlation.orderIdentity = {};
        if (!inbox.push(kernelValue))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
    for (const AIOpportunityAttackMoveChildFeedback& value : m_transients.opportunityAttackMoveChildFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.subject);
        if (!handle || !acceptsAsyncOrder(*handle, value.correlation.orderIdentity))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        AIOpportunityAttackMoveChildFeedbackBuffer& inbox =
            m_shadowBatches[handle->batch].columns().opportunityChildFeedback[handle->slot];
        AIOpportunityAttackMoveChildFeedback kernelValue = value;
        kernelValue.correlation.orderIdentity = {};
        if (!inbox.push(kernelValue))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
    for (const AIDockFeedback& value : m_transients.dockFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.token.subject);
        if (!handle || !acceptsDockCorrelation(*handle, value.correlation))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        AIDockFeedbackBuffer& inbox = m_shadowBatches[handle->batch].columns().dockFeedback[handle->slot];
        if (!inbox.push(value))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
    for (const AIContainmentFeedback& value : m_transients.containmentFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.subject);
        if (!handle || !acceptsContainmentCorrelation(*handle, value.correlation))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        AIContainmentFeedbackBuffer& inbox = m_shadowBatches[handle->batch].columns().containmentFeedback[handle->slot];
        if (!inbox.push(value))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
    for (const AIInsertionMotionFeedback& value : m_transients.insertionMotionFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.subject);
        if (!handle || !acceptsInsertionCorrelation(*handle, value.correlation))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        AIInsertionMotionFeedbackBuffer& inbox =
            m_shadowBatches[handle->batch].columns().insertionMotionFeedback[handle->slot];
        if (!inbox.push(value))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
    for (const AIInsertionContainmentFeedback& value : m_transients.insertionContainmentFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.subject);
        if (!handle || !acceptsInsertionCorrelation(*handle, value.correlation))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        AIInsertionContainmentFeedbackBuffer& inbox =
            m_shadowBatches[handle->batch].columns().insertionContainmentFeedback[handle->slot];
        if (!inbox.push(value))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
    for (const AIInsertionOperationFeedback& value : m_transients.insertionOperationFeedback())
    {
        const std::optional<AIActorHandle> handle = find(value.correlation.subject);
        if (!handle || !acceptsInsertionCorrelation(*handle, value.correlation))
        {
            ++report.feedbackRejected;
            continue;
        }
        wakeForServiceResult(*handle);
        AIInsertionOperationFeedbackBuffer& inbox =
            m_shadowBatches[handle->batch].columns().insertionOperationFeedback[handle->slot];
        if (!inbox.push(value))
            ++report.feedbackOverflows;
        else
            ++report.feedbackValuesRead;
    }
}

bool ObjectAIRuntime::acceptsDockCorrelation(AIActorHandle handle, const AIDockCorrelation& correlation) const noexcept
{
    const AIStateFamilySoAStorage* actorStorage = storage(handle);
    if (!actorStorage ||
        actorStorage->payloadStates()[handle.slot] != actorStorage->runtimes()[handle.slot].currentState)
    {
        return false;
    }
    const AIStateId state = actorStorage->runtimes()[handle.slot].currentState;
    if (state != AIStateId::Dock && state != AIStateId::GetRepaired)
        return false;
    const AIDockStatePayload payload = actorStorage->dock().load(handle.slot);
    const AIDockMoveStage expectedMoveStage = payload.pendingRequest == AIDockRequestKind::BeginMove
                                                  ? dock_detail::moveStageFor(payload.phase)
                                                  : AIDockMoveStage::None;
    return correlation.token == payload.token && correlation.phase == payload.phase &&
           correlation.moveStage == expectedMoveStage && correlation.phaseRevision == payload.phaseRevision &&
           correlation.exchangeSequence == payload.exchangeSequence;
}

bool ObjectAIRuntime::acceptsContainmentCorrelation(AIActorHandle handle,
                                                    const AIContainmentCorrelation& correlation) const noexcept
{
    const AIStateFamilySoAStorage* actorStorage = storage(handle);
    if (!actorStorage || actorStorage->runtimes()[handle.slot].currentState != correlation.state ||
        actorStorage->payloadStates()[handle.slot] != correlation.state)
    {
        return false;
    }
    return actorStorage->containmentRequestTick()[handle.slot] == correlation.stateRequest.issuedTick &&
           actorStorage->containmentRequestSequence()[handle.slot] == correlation.stateRequest.sequence;
}

bool ObjectAIRuntime::acceptsInsertionCorrelation(AIActorHandle handle,
                                                  const AIInsertionCorrelation& correlation) const noexcept
{
    const AIStateFamilySoAStorage* actorStorage = storage(handle);
    if (!actorStorage || actorStorage->runtimes()[handle.slot].currentState != correlation.state ||
        actorStorage->payloadStates()[handle.slot] != correlation.state)
    {
        return false;
    }
    return actorStorage->insertion().load(handle.slot).request == correlation.stateRequest;
}

void ObjectAIRuntime::wakeForServiceResult(AIActorHandle handle) noexcept
{
    AIStateFamilySoAStorage* actorStorage = storage(handle);
    if (!actorStorage)
        return;
    AIStateMachineRuntime& runtime = actorStorage->runtimes()[handle.slot];
    if (runtime.wakeTick != 0)
        AIStateMachine::wake(runtime, AIWakeReason::ServiceResult);
}

ObjectAITransientClearReport ObjectAIRuntime::clearTransientSubject(ObjectId subject) noexcept
{
    container::Array<PathHandle, 2> ownedPaths{};
    size_t ownedCount = 0;
    const auto retainOwnedPath = [&](PathHandle path) noexcept
    {
        if (!path)
            return;
        for (size_t index = 0; index < ownedCount; ++index)
        {
            if (ownedPaths[index] == path)
                return;
        }
        if (ownedCount < ownedPaths.size())
            ownedPaths[ownedCount++] = path;
    };
    for (const PathFeedback& feedback : m_transients.pathFeedback())
    {
        if (feedback.correlation.subject == subject && feedback.status == PathFeedbackStatus::Ready)
            retainOwnedPath(feedback.path);
    }
    for (const MovementCommand& command : m_transients.movementCommands())
    {
        if (command.correlation.subject == subject && command.kind == MovementCommandKind::InstallPath)
            retainOwnedPath(command.path);
    }
    for (size_t index = 0; index < ownedCount; ++index)
        m_pathHandleReleaser(ownedPaths[index]);
    return m_transients.clearSubject(subject);
}

void ObjectAIRuntime::releaseUnclaimedPathFeedback() noexcept
{
    const container::Span<const PathFeedback> feedbackValues = m_transients.pathFeedback();
    const container::Span<const MovementCommand> movementCommands = m_transients.movementCommands();
    for (size_t index = 0; index < feedbackValues.size(); ++index)
    {
        const PathFeedback& feedback = feedbackValues[index];
        if (feedback.status != PathFeedbackStatus::Ready || !feedback.path)
            continue;

        bool duplicate = false;
        for (size_t previous = 0; previous < index; ++previous)
        {
            if (feedbackValues[previous].status == PathFeedbackStatus::Ready &&
                feedbackValues[previous].path == feedback.path)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;

        const bool transferred = std::any_of(movementCommands.begin(),
                                             movementCommands.end(),
                                             [&feedback](const MovementCommand& command) noexcept
                                             {
                                                 return command.kind == MovementCommandKind::InstallPath &&
                                                        command.path == feedback.path &&
                                                        command.correlation == feedback.correlation;
                                             });
        if (!transferred)
            m_pathHandleReleaser(feedback.path);
    }
}

bool ObjectAIRuntime::acceptsAsyncOrder(AIActorHandle handle, const AIAsyncOrderIdentity& identity) const noexcept
{
    if (!identity.isValid() || handle.batch >= m_orderAdmissions.size())
        return false;
    const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[handle.batch];
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(admission.handle(handle.slot), active) != ObjectAIOrderAdmissionStatus::Success)
    {
        return false;
    }
    return matchesAIAsyncOrderIdentity(identity, active.identity);
}

ObjectAIShadowTickReport ObjectAIRuntime::remember(const ObjectAIShadowTickReport& report) noexcept
{
    m_lastShadowReport = report;
    return report;
}

bool ObjectAIRuntime::validateInputSnapshot(const ObjectAIReadOnlyInputSnapshot& input) const noexcept
{
    if (!input.valid)
        return input.confirmedTick == 0 && input.ticksPerSecond == 0 && input.facts.empty();
    if (input.ticksPerSecond == 0 || input.facts.size() > m_config.maximumActors)
        return false;
    for (size_t index = 0; index < input.facts.size(); ++index)
    {
        if (!input.facts[index].subject ||
            (index != 0 && !(input.facts[index - 1].subject < input.facts[index].subject)))
            return false;
    }
    return true;
}

} // namespace engine::ai
