#include "game/object/ai/runtime/ObjectAIRuntime.h"

namespace engine::ai
{

void ObjectAIRuntime::collectAndAuditOutputs(ObjectAIShadowBatch& shadow, ObjectAIShadowTickReport& report) noexcept
{
    ObjectAIShadowBatchOutputs outputs = shadow.outputs();
    collectFacingCommands(outputs.facingCommands, report);
    collectOwnedPathRequests(outputs.pathRequests, report);
    collectOwnedMovementCommands(outputs.movementCommands, report);
    collectWaypointCompletions(outputs.waypointCompletions, report);
    collectOwnedAttackCommands(outputs.attackCommands, report);
    collectOwnedOpportunityQueryCommands(outputs.opportunityQueryCommands, report);
    collectOwnedOpportunityChildCommands(outputs.opportunityChildCommands, report);
    collectOwnedTacticalQueryCommands(outputs.tacticalQueryCommands, report);
    collectOwnedTacticalChildCommands(outputs.tacticalChildCommands, report);
    collectOwnedGuardTacticalCommands(outputs.guardTacticalCommands, report);
    collectOwnedGuardInteractionCommands(outputs.guardInteractionCommands, report);
    collectDockRequests(outputs.dockRequests, report);
    collectContainmentCommands(outputs.containmentCommands, report);
    collectInsertionCommands(outputs.insertionMotionCommands, report);
    collectInsertionCommands(outputs.insertionContainmentCommands, report);
    collectInsertionCommands(outputs.insertionOperationCommands, report);
    collectInsertionCommands(outputs.insertionEffectCommands, report);
    auditBuffers(outputs.pathRequests, report);
    auditBuffers(outputs.movementCommands, report);
    auditBuffers(outputs.moveEvacuateCommands, report);
    auditBuffers(outputs.teamProgressRequests, report);
    auditBuffers(outputs.hackCommands, report);
    auditBuffers(outputs.attackCommands, report);
    auditBuffers(outputs.guardTacticalCommands, report);
    auditBuffers(outputs.guardInteractionCommands, report);
    auditBuffers(outputs.tacticalQueryCommands, report);
    auditBuffers(outputs.tacticalChildCommands, report);
    auditBuffers(outputs.opportunityQueryCommands, report);
    auditBuffers(outputs.opportunityChildCommands, report);
    shadow.clearTransientOutputs();
    shadow.clearFeedback();
}

void ObjectAIRuntime::collectWaypointCompletions(
    container::Span<AIWaypointCompletionBuffer> buffers,
    ObjectAIShadowTickReport& report) noexcept
{
    for (AIWaypointCompletionBuffer& buffer : buffers)
    {
        report.outputOverflows += buffer.overflowed ? 1 : 0;
        for (size_t index = 0; index < buffer.count; ++index)
        {
            const AIWaypointCompletionEvent& event = buffer.values[index];
            const std::optional<AIActorHandle> actor = find(event.subject);
            const AIStateFamilySoAStorage* actorStorage = actor
                ? storage(*actor) : nullptr;
            ObjectAIOrderAdmissionRequest active;
            const bool admitted = actor &&
                actor->batch < m_orderAdmissions.size() &&
                m_orderAdmissions[actor->batch].activeOrder(
                    m_orderAdmissions[actor->batch].handle(actor->slot),
                    active) == ObjectAIOrderAdmissionStatus::Success;
            const AIWaypointPathStatePayload payload = actorStorage
                ? actorStorage->waypointPath().load(actor->slot)
                : AIWaypointPathStatePayload{};
            if (!event.isValid() ||
                event.confirmedTick != report.confirmedTick ||
                !admitted || active.kind != ObjectAIOrderKind::Move ||
                !isObjectAIWaypointRouteSubtype(active.moveRouteSubtype) ||
                payload.request != event.stateRequest ||
                payload.completionTerminal != event.terminal ||
                payload.sourceOrderRevision != active.identity.queueRevision ||
                !stageOutput(event, report))
            {
                ++report.waypointCompletionsRejected;
                continue;
            }
            ++report.waypointCompletionsStaged;
        }
    }
}

void ObjectAIRuntime::collectFacingCommands(container::Span<AIStateCommandBuffer> buffers,
                                            ObjectAIShadowTickReport& report) noexcept
{
    for (AIStateCommandBuffer& buffer : buffers)
    {
        report.outputOverflows += buffer.overflowed ? 1 : 0;
        for (size_t index = 0; index < buffer.count; ++index)
        {
            const AIStateCommand& command = buffer.commands[index];
            const std::optional<AIActorHandle> actor = find(command.subject);
            const AIStateFamilySoAStorage* actorStorage = actor ? storage(*actor) : nullptr;
            const AIStateId expectedState =
                command.kind == AIStateCommandKind::FaceObject ? AIStateId::FaceObject : AIStateId::FacePosition;
            if (!actorStorage || actorStorage->runtimes()[actor->slot].currentState != expectedState ||
                actorStorage->face().load(actor->slot).request != command.request ||
                !stageOutput(command, report))
            {
                ++report.facingCommandsRejected;
                continue;
            }
            ++report.facingCommandsStaged;
        }
    }
}

void ObjectAIRuntime::releaseStaleFacingCommands() noexcept
{
    size_t index = 0;
    while (index < m_transients.facingCommands().size())
    {
        const AIStateCommand command = m_transients.facingCommands()[index];
        const std::optional<AIActorHandle> actor = find(command.subject);
        const AIStateFamilySoAStorage* actorStorage = actor ? storage(*actor) : nullptr;
        const AIStateId expectedState =
            command.kind == AIStateCommandKind::FaceObject ? AIStateId::FaceObject : AIStateId::FacePosition;
        const bool current = actorStorage && actorStorage->runtimes()[actor->slot].currentState == expectedState &&
                             actorStorage->face().load(actor->slot).request == command.request;
        if (current)
        {
            ++index;
            continue;
        }
        static_cast<void>(m_transients.removeFacingCommand(command.subject, command.request));
    }
}

void ObjectAIRuntime::collectOwnedPathRequests(container::Span<PathRequestBuffer> buffers,
                                               ObjectAIShadowTickReport& report) noexcept
{
    for (PathRequestBuffer& buffer : buffers)
    {
        for (size_t index = 0; index < buffer.count; ++index)
        {
            PathRequest request = buffer.values[index];
            const std::optional<AIActorHandle> actor = find(request.correlation.subject);
            if (!actor || actor->batch >= m_orderAdmissions.size())
            {
                ++report.pathRequestsRejected;
                continue;
            }
            const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
            ObjectAIOrderAdmissionRequest active;
            bool historicalCleanup = false;
            if (admission.activeOrder(admission.handle(actor->slot), active) != ObjectAIOrderAdmissionStatus::Success)
            {
                ObjectAIOrderAdmissionSlotView slot;
                if (request.kind != PathRequestKind::Cancel ||
                    admission.readSlot(actor->slot, slot) != ObjectAIOrderAdmissionStatus::Success ||
                    !slot.hasHistory || slot.active)
                {
                    ++report.pathRequestsRejected;
                    continue;
                }
                active = slot.historicalOrder;
                historicalCleanup = true;
            }
            if ((active.kind != ObjectAIOrderKind::Move && active.kind != ObjectAIOrderKind::Attack &&
                 active.kind != ObjectAIOrderKind::TacticalAttack) ||
                request.correlation.sourceOrderRevision != active.identity.queueRevision)
            {
                ++report.pathRequestsRejected;
                continue;
            }
            request.correlation.orderIdentity = toAIAsyncOrderIdentity(active.identity);
            static_cast<void>(historicalCleanup);
            if (!stageOutput(request, report))
            {
                ++report.pathRequestsRejected;
                continue;
            }
            ++report.pathRequestsStaged;
        }
    }
}

void ObjectAIRuntime::collectOwnedMovementCommands(container::Span<MovementCommandBuffer> buffers,
                                                   ObjectAIShadowTickReport& report) noexcept
{
    for (MovementCommandBuffer& buffer : buffers)
    {
        for (size_t index = 0; index < buffer.count; ++index)
        {
            MovementCommand command = buffer.values[index];
            const std::optional<AIActorHandle> actor = find(command.correlation.subject);
            if (!actor || actor->batch >= m_orderAdmissions.size())
            {
                ++report.movementCommandsRejected;
                continue;
            }
            const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
            ObjectAIOrderAdmissionRequest active;
            if (admission.activeOrder(admission.handle(actor->slot), active) != ObjectAIOrderAdmissionStatus::Success)
            {
                ObjectAIOrderAdmissionSlotView slot;
                if (command.kind != MovementCommandKind::EndMovement ||
                    admission.readSlot(actor->slot, slot) != ObjectAIOrderAdmissionStatus::Success ||
                    !slot.hasHistory || slot.active)
                {
                    ++report.movementCommandsRejected;
                    continue;
                }
                active = slot.historicalOrder;
            }
            if ((active.kind != ObjectAIOrderKind::Move && active.kind != ObjectAIOrderKind::Attack &&
                 active.kind != ObjectAIOrderKind::TacticalAttack) ||
                command.correlation.sourceOrderRevision != active.identity.queueRevision)
            {
                ++report.movementCommandsRejected;
                continue;
            }
            command.correlation.orderIdentity = toAIAsyncOrderIdentity(active.identity);
            if (!stageOutput(command, report))
            {
                ++report.movementCommandsRejected;
                continue;
            }
            ++report.movementCommandsStaged;
        }
    }
}

void ObjectAIRuntime::collectOwnedAttackCommands(container::Span<AIAttackCommandBuffer> buffers,
                                                 ObjectAIShadowTickReport& report) noexcept
{
    for (AIAttackCommandBuffer& buffer : buffers)
    {
        for (size_t index = 0; index < buffer.count; ++index)
        {
            AIAttackCommand command = buffer.values[index];
            const std::optional<AIActorHandle> actor = find(command.correlation.subject);
            if (!actor || actor->batch >= m_orderAdmissions.size() ||
                command.confirmedTick != report.confirmedTick)
            {
                ++report.attackCommandsRejected;
                continue;
            }
            const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
            ObjectAIOrderAdmissionRequest active;
            const ObjectAIOrderAdmissionStatus activeStatus =
                admission.activeOrder(admission.handle(actor->slot), active);
            if (activeStatus != ObjectAIOrderAdmissionStatus::Success)
            {
                const AIStateFamilySoAStorage* actorStorage = storage(*actor);
                const AIAttackStatePayload payload = actorStorage
                    ? actorStorage->attack().load(actor->slot)
                    : AIAttackStatePayload{};
                const AIStateParameters* parameters = actorStorage
                    ? &actorStorage->parameters()[actor->slot] : nullptr;
                const bool autonomousIdleAttack =
                    activeStatus == ObjectAIOrderAdmissionStatus::NoActiveOrder &&
                    actorStorage && parameters &&
                    parameters->sourceOrderRevision == 0 &&
                    command.correlation.state == AIStateId::AttackObject &&
                    !command.correlation.orderIdentity.isValid() &&
                    payload.request == command.correlation.stateRequest &&
                    payload.sourceOrderRevision != 0;
                if (!autonomousIdleAttack ||
                    !stageOutput(command, report))
                {
                    ++report.attackCommandsRejected;
                    continue;
                }
                ++report.attackCommandsStaged;
                continue;
            }
            const bool ordinaryAttack = active.kind == ObjectAIOrderKind::Attack;
            const bool attackMoveChild = active.kind == ObjectAIOrderKind::Move && active.attackMove &&
                                         command.correlation.state == AIStateId::AttackObject;
            const bool tacticalAttackChild =
                active.kind == ObjectAIOrderKind::TacticalAttack &&
                ([&]() noexcept {
                    switch (active.tacticalAttackSubtype) {
                    case ObjectAITacticalAttackSubtype::Hunt:
                    case ObjectAITacticalAttackSubtype::AttackSquad:
                    case ObjectAITacticalAttackSubtype::AttackArea:
                    case ObjectAITacticalAttackSubtype::GuardRetaliate:
                        return command.correlation.state ==
                            AIStateId::AttackObject;
                    case ObjectAITacticalAttackSubtype::Guard:
                    case ObjectAITacticalAttackSubtype::GuardTunnelNetwork:
                        return command.correlation.state ==
                                AIStateId::AttackObject ||
                            command.correlation.state ==
                                AIStateId::AttackAndFollowObject;
                    default:
                        return false;
                    }
                }());
            if (!ordinaryAttack && !attackMoveChild && !tacticalAttackChild)
            {
                ++report.attackCommandsRejected;
                continue;
            }
            if (attackMoveChild || tacticalAttackChild)
            {
                const AIStateFamilySoAStorage* actorStorage = storage(*actor);
                const AIAttackStatePayload payload =
                    actorStorage ? actorStorage->attack().load(actor->slot) : AIAttackStatePayload{};
                if (!actorStorage || payload.request != command.correlation.stateRequest ||
                    payload.sourceOrderRevision != active.identity.queueRevision)
                {
                    ++report.attackCommandsRejected;
                    continue;
                }
            }
            command.correlation.orderIdentity = toAIAsyncOrderIdentity(active.identity);
            if (!stageOutput(command, report))
            {
                ++report.attackCommandsRejected;
                continue;
            }
            ++report.attackCommandsStaged;
        }
    }
}

void ObjectAIRuntime::collectOwnedOpportunityQueryCommands(
    container::Span<AIOpportunityAttackMoveQueryCommandBuffer> buffers, ObjectAIShadowTickReport& report) noexcept
{
    for (AIOpportunityAttackMoveQueryCommandBuffer& buffer : buffers)
    {
        for (size_t index = 0; index < buffer.count; ++index)
        {
            AIOpportunityAttackMoveQueryCommand command = buffer.values[index];
            const std::optional<AIActorHandle> actor = find(command.correlation.subject);
            ObjectAIOrderAdmissionRequest active;
            if (!actor || actor->batch >= m_orderAdmissions.size() ||
                m_orderAdmissions[actor->batch].activeOrder(m_orderAdmissions[actor->batch].handle(actor->slot),
                                                            active) != ObjectAIOrderAdmissionStatus::Success ||
                active.kind != ObjectAIOrderKind::Move || !active.attackMove ||
                command.confirmedTick != report.confirmedTick ||
                command.correlation.sourceOrderRevision != active.identity.queueRevision)
            {
                ++report.opportunityQueryCommandsRejected;
                continue;
            }
            command.correlation.orderIdentity = toAIAsyncOrderIdentity(active.identity);
            if (!stageOutput(command, report))
            {
                ++report.opportunityQueryCommandsRejected;
                continue;
            }
            ++report.opportunityQueryCommandsStaged;
        }
    }
}

void ObjectAIRuntime::collectOwnedOpportunityChildCommands(
    container::Span<AIOpportunityAttackMoveChildCommandBuffer> buffers, ObjectAIShadowTickReport& report) noexcept
{
    for (AIOpportunityAttackMoveChildCommandBuffer& buffer : buffers)
    {
        for (size_t index = 0; index < buffer.count; ++index)
        {
            AIOpportunityAttackMoveChildCommand command = buffer.values[index];
            const std::optional<AIActorHandle> actor = find(command.correlation.subject);
            ObjectAIOrderAdmissionRequest active;
            if (!actor || actor->batch >= m_orderAdmissions.size() ||
                m_orderAdmissions[actor->batch].activeOrder(m_orderAdmissions[actor->batch].handle(actor->slot),
                                                            active) != ObjectAIOrderAdmissionStatus::Success ||
                active.kind != ObjectAIOrderKind::Move || !active.attackMove ||
                command.confirmedTick != report.confirmedTick ||
                command.correlation.sourceOrderRevision != active.identity.queueRevision)
            {
                ++report.opportunityChildCommandsRejected;
                continue;
            }
            command.correlation.orderIdentity = toAIAsyncOrderIdentity(active.identity);
            // AttackObject and PickUpCrate are in-runtime child adapters.
            // Their commands are consumed by the shared kernels in this same
            // multiwave and must not cross into GameSession as second owners.
            const bool internalAttack =
                command.correlation.operation ==
                    AIOpportunityAttackMoveOperation::Attack &&
                command.kind ==
                    AIOpportunityAttackMoveChildCommandKind::BeginAttack;
            const bool internalPickup =
                command.correlation.operation ==
                    AIOpportunityAttackMoveOperation::PickUpCrate &&
                command.kind ==
                    AIOpportunityAttackMoveChildCommandKind::BeginPickUpCrate;
            if (internalAttack || internalPickup ||
                command.kind ==
                    AIOpportunityAttackMoveChildCommandKind::Cancel)
            {
                ++report.opportunityChildCommandsStaged;
                continue;
            }
            if (!stageOutput(command, report))
            {
                ++report.opportunityChildCommandsRejected;
                continue;
            }
            ++report.opportunityChildCommandsStaged;
        }
    }
}

void ObjectAIRuntime::collectOwnedTacticalQueryCommands(container::Span<AITacticalAttackQueryCommandBuffer> buffers,
                                                        ObjectAIShadowTickReport& report) noexcept
{
    for (AITacticalAttackQueryCommandBuffer& buffer : buffers)
    {
        for (size_t index = 0; index < buffer.count; ++index)
        {
            AITacticalAttackQueryCommand command = buffer.values[index];
            const std::optional<AIActorHandle> actor = find(command.correlation.subject);
            ObjectAIOrderAdmissionRequest active;
            const auto expectedWrapper = [](ObjectAITacticalAttackSubtype subtype) noexcept
            {
                switch (subtype)
                {
                case ObjectAITacticalAttackSubtype::Hunt:
                    return AIStateId::Hunt;
                case ObjectAITacticalAttackSubtype::AttackSquad:
                    return AIStateId::AttackSquad;
                case ObjectAITacticalAttackSubtype::AttackArea:
                    return AIStateId::AttackArea;
                default:
                    return AIStateId::Invalid;
                }
            };
            if (!actor || actor->batch >= m_orderAdmissions.size() ||
                m_orderAdmissions[actor->batch].activeOrder(m_orderAdmissions[actor->batch].handle(actor->slot),
                                                            active) != ObjectAIOrderAdmissionStatus::Success ||
                active.kind != ObjectAIOrderKind::TacticalAttack ||
                command.correlation.wrapperState != expectedWrapper(active.tacticalAttackSubtype) ||
                command.confirmedTick != report.confirmedTick ||
                command.correlation.sourceOrderRevision != active.identity.queueRevision)
            {
                ++report.tacticalQueryCommandsRejected;
                continue;
            }
            command.correlation.orderIdentity = toAIAsyncOrderIdentity(active.identity);
            if (!stageOutput(command, report))
            {
                ++report.tacticalQueryCommandsRejected;
                continue;
            }
            ++report.tacticalQueryCommandsStaged;
        }
    }
}

void ObjectAIRuntime::collectOwnedTacticalChildCommands(container::Span<AITacticalAttackChildCommandBuffer> buffers,
                                                        ObjectAIShadowTickReport& report) noexcept
{
    for (AITacticalAttackChildCommandBuffer& buffer : buffers)
    {
        for (size_t index = 0; index < buffer.count; ++index)
        {
            AITacticalAttackChildCommand command = buffer.values[index];
            const std::optional<AIActorHandle> actor = find(command.correlation.subject);
            if (command.kind ==
                    AITacticalAttackChildCommandKind::EndWrapper) {
                // Exit cleanup belongs to the wrapper that just lost
                // ownership. A replacement order may already be active, so
                // validating against the new queue head would discard the
                // old Hunt state's temporary-lock release.
                if (!actor ||
                    command.confirmedTick != report.confirmedTick ||
                    !command.correlation.isWrapperValid()) {
                    ++report.tacticalChildCommandsRejected;
                    continue;
                }
                if (command.releaseTemporaryWeaponLock &&
                    !stageOutput(command, report)) {
                    ++report.tacticalChildCommandsRejected;
                    continue;
                }
                ++report.tacticalChildCommandsStaged;
                continue;
            }
            ObjectAIOrderAdmissionRequest active;
            const auto expectedWrapper = [](ObjectAITacticalAttackSubtype subtype) noexcept
            {
                switch (subtype)
                {
                case ObjectAITacticalAttackSubtype::Hunt:
                    return AIStateId::Hunt;
                case ObjectAITacticalAttackSubtype::AttackSquad:
                    return AIStateId::AttackSquad;
                case ObjectAITacticalAttackSubtype::AttackArea:
                    return AIStateId::AttackArea;
                default:
                    return AIStateId::Invalid;
                }
            };
            if (!actor || actor->batch >= m_orderAdmissions.size() ||
                m_orderAdmissions[actor->batch].activeOrder(m_orderAdmissions[actor->batch].handle(actor->slot),
                                                            active) != ObjectAIOrderAdmissionStatus::Success ||
                active.kind != ObjectAIOrderKind::TacticalAttack ||
                command.correlation.wrapperState != expectedWrapper(active.tacticalAttackSubtype) ||
                command.confirmedTick != report.confirmedTick ||
                command.correlation.sourceOrderRevision != active.identity.queueRevision)
            {
                ++report.tacticalChildCommandsRejected;
                continue;
            }
            command.correlation.orderIdentity = toAIAsyncOrderIdentity(active.identity);
            // StartOrReplace for both supported child families is consumed by
            // shared kernels inside this executor call.
            if (command.kind ==
                    AITacticalAttackChildCommandKind::StartOrReplace &&
                (command.correlation.childState == AIStateId::AttackObject ||
                 command.correlation.childState == AIStateId::PickUpCrate))
            {
                ++report.tacticalChildCommandsStaged;
                continue;
            }
            if (!stageOutput(command, report))
            {
                ++report.tacticalChildCommandsRejected;
                continue;
            }
            ++report.tacticalChildCommandsStaged;
        }
    }
}

void ObjectAIRuntime::collectOwnedGuardTacticalCommands(container::Span<AIGuardTacticalCommandBuffer> buffers,
                                                        ObjectAIShadowTickReport& report) noexcept
{
    for (AIGuardTacticalCommandBuffer& buffer : buffers)
    {
        for (size_t index = 0; index < buffer.count; ++index)
        {
            AIGuardTacticalCommand command = buffer.values[index];
            const std::optional<AIActorHandle> actor = find(command.correlation.subject);
            ObjectAIOrderAdmissionRequest active;
            if (!actor || actor->batch >= m_orderAdmissions.size() ||
                m_orderAdmissions[actor->batch].activeOrder(m_orderAdmissions[actor->batch].handle(actor->slot),
                                                            active) != ObjectAIOrderAdmissionStatus::Success ||
                active.kind != ObjectAIOrderKind::TacticalAttack ||
                active.tacticalAttackSubtype != (command.correlation.state == AIStateId::GuardRetaliate
                                                     ? ObjectAITacticalAttackSubtype::GuardRetaliate
                                                 : command.correlation.state == AIStateId::GuardTunnelNetwork
                                                     ? ObjectAITacticalAttackSubtype::GuardTunnelNetwork
                                                     : ObjectAITacticalAttackSubtype::Guard) ||
                (command.correlation.state != AIStateId::Guard &&
                 command.correlation.state != AIStateId::GuardTunnelNetwork &&
                 command.correlation.state != AIStateId::GuardRetaliate) ||
                command.confirmedTick != report.confirmedTick ||
                command.correlation.sourceOrderRevision != active.identity.queueRevision)
            {
                ++report.guardTacticalCommandsRejected;
                continue;
            }
            command.correlation.orderIdentity = toAIAsyncOrderIdentity(active.identity);
            if (!stageOutput(command, report))
            {
                ++report.guardTacticalCommandsRejected;
                continue;
            }
            ++report.guardTacticalCommandsStaged;
        }
    }
}

void ObjectAIRuntime::collectOwnedGuardInteractionCommands(container::Span<AIGuardInteractionCommandBuffer> buffers,
                                                           ObjectAIShadowTickReport& report) noexcept
{
    for (AIGuardInteractionCommandBuffer& buffer : buffers)
    {
        for (size_t index = 0; index < buffer.count; ++index)
        {
            AIGuardInteractionCommand command = buffer.values[index];
            const std::optional<AIActorHandle> actor = find(command.correlation.subject);
            ObjectAIOrderAdmissionRequest active;
            if (!actor || actor->batch >= m_orderAdmissions.size() ||
                m_orderAdmissions[actor->batch].activeOrder(m_orderAdmissions[actor->batch].handle(actor->slot),
                                                            active) != ObjectAIOrderAdmissionStatus::Success ||
                active.kind != ObjectAIOrderKind::TacticalAttack ||
                active.tacticalAttackSubtype != (command.correlation.state == AIStateId::GuardRetaliate
                                                     ? ObjectAITacticalAttackSubtype::GuardRetaliate
                                                 : command.correlation.state == AIStateId::GuardTunnelNetwork
                                                     ? ObjectAITacticalAttackSubtype::GuardTunnelNetwork
                                                     : ObjectAITacticalAttackSubtype::Guard) ||
                (command.correlation.state != AIStateId::Guard &&
                 command.correlation.state != AIStateId::GuardTunnelNetwork &&
                 command.correlation.state != AIStateId::GuardRetaliate) ||
                command.confirmedTick != report.confirmedTick ||
                command.correlation.sourceOrderRevision != active.identity.queueRevision)
            {
                ++report.guardInteractionCommandsRejected;
                continue;
            }
            command.correlation.orderIdentity = toAIAsyncOrderIdentity(active.identity);
            if (!stageOutput(command, report))
            {
                ++report.guardInteractionCommandsRejected;
                continue;
            }
            ++report.guardInteractionCommandsStaged;
        }
    }
}

void ObjectAIRuntime::collectDockRequests(container::Span<AIDockRequestBuffer> buffers,
                                          ObjectAIShadowTickReport& report) noexcept
{
    for (const AIDockRequestBuffer& buffer : buffers)
    {
        report.outputOverflows += buffer.overflowed ? 1 : 0;
        for (size_t index = 0; index < buffer.count; ++index)
        {
            const AIDockRequest& request = buffer.values[index];
            if (!find(request.correlation.token.subject) ||
                !stageOutput(request, report))
            {
                ++report.dockRequestsRejected;
                continue;
            }
            ++report.dockRequestsStaged;
        }
    }
}

void ObjectAIRuntime::collectContainmentCommands(container::Span<AIContainmentCommandBuffer> buffers,
                                                 ObjectAIShadowTickReport& report) noexcept
{
    for (const AIContainmentCommandBuffer& buffer : buffers)
    {
        report.outputOverflows += buffer.overflowed ? 1 : 0;
        for (size_t index = 0; index < buffer.count; ++index)
        {
            const AIContainmentCommand& command = buffer.values[index];
            if (!find(command.correlation.subject) ||
                !stageOutput(command, report))
            {
                ++report.containmentCommandsRejected;
                continue;
            }
            ++report.containmentCommandsStaged;
        }
    }
}

} // namespace engine::ai
