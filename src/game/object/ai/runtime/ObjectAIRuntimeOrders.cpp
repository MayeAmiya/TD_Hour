#include "game/object/ai/runtime/ObjectAIRuntime.h"

namespace engine::ai
{

ObjectAIOrderAdmissionResult ObjectAIRuntime::setOrderCapabilities(ObjectId subject,
                                                                   ObjectAIOrderCapability capabilities) noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return result;
    }
    const std::optional<ObjectAIRecipeActorView> recipe =
        recipeBinding(subject);
    const bool capabilityAllowed = recipe &&
        ((recipe->state == ObjectAIRecipeBindingState::Bound &&
          isObjectAIRecipeCapabilitySubset(recipe->recipe, capabilities)) ||
         (recipe->state ==
              ObjectAIRecipeBindingState::ContentUnavailable &&
          capabilities == ObjectAIOrderCapability::None) ||
         (recipe->state == ObjectAIRecipeBindingState::Unbound &&
          isObjectAIRecipeInitialCapabilityMask(capabilities)));
    if (!capabilityAllowed)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::InvalidCapabilityMask;
        return result;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderAdmissionResult result = admission.setCapabilities(admission.handle(actor->slot), capabilities);
    if (!result.succeeded())
        return result;
    return result;
}

ObjectAIOrderAdmissionStorage* ObjectAIRuntime::orderAdmission(AIActorHandle handle) noexcept
{
    return resolve(handle) && handle.batch < m_orderAdmissions.size() ? &m_orderAdmissions[handle.batch] : nullptr;
}

const ObjectAIOrderAdmissionStorage* ObjectAIRuntime::orderAdmission(AIActorHandle handle) const noexcept
{
    return resolve(handle) && handle.batch < m_orderAdmissions.size() ? &m_orderAdmissions[handle.batch] : nullptr;
}

std::optional<ObjectAIOrderCapability> ObjectAIRuntime::orderCapabilities(
    ObjectId subject) const noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    const ObjectAIOrderAdmissionStorage* admission =
        actor ? orderAdmission(*actor) : nullptr;
    ObjectAIOrderAdmissionSlotView slot;
    if (!actor || !admission ||
        admission->readSlot(actor->slot, slot) !=
            ObjectAIOrderAdmissionStatus::Success ||
        !slot.bound || slot.handle.subject != subject ||
        slot.handle.generation != actor->generation) {
        return std::nullopt;
    }
    return slot.capabilities;
}

bool ObjectAIRuntime::hasOrderCapability(
    ObjectId subject, ObjectAIOrderCapability capability) const noexcept
{
    const std::optional<ObjectAIOrderCapability> capabilities =
        orderCapabilities(subject);
    return capabilities &&
        hasObjectAIOrderCapability(*capabilities, capability);
}

void ObjectAIRuntime::captureOrderCapabilitySnapshot(
    ObjectAIOrderCapabilitySnapshot& output) const
{
    output.clear();
    output.moveStopSubjects.reserve(m_subjects.size());
    output.attackSubjects.reserve(m_subjects.size());
    output.autonomousAttackSubjects.reserve(m_subjects.size());
    for (const AIStateSoASubjectSlot& actor : m_subjects) {
        if (actor.handle.batch >= m_orderAdmissions.size()) continue;
        const ObjectAIOrderAdmissionStorage& admission =
            m_orderAdmissions[actor.handle.batch];
        ObjectAIOrderAdmissionSlotView slot;
        if (admission.readSlot(actor.handle.slot, slot) !=
                ObjectAIOrderAdmissionStatus::Success ||
            !slot.bound || slot.handle.subject != actor.subject ||
            slot.handle.generation != actor.handle.generation) {
            continue;
        }
        if (hasObjectAIOrderCapability(
                slot.capabilities, ObjectAIOrderCapability::MoveStop)) {
            output.moveStopSubjects.push_back(actor.subject);
        }
        if (hasObjectAIOrderCapability(
                slot.capabilities, ObjectAIOrderCapability::Attack)) {
            output.attackSubjects.push_back(actor.subject);
            const AIStateFamilySoAStorage* actorStorage = storage(actor.handle);
            const bool pendingAutonomousCommand = std::any_of(
                m_transients.attackCommands().begin(),
                m_transients.attackCommands().end(),
                [&actor](const AIAttackCommand& command) noexcept {
                    return command.correlation.subject == actor.subject &&
                        command.correlation.state ==
                            AIStateId::AttackObject &&
                        !command.correlation.orderIdentity.isValid();
                });
            if (!slot.active && actorStorage &&
                actorStorage->parameters()[actor.handle.slot]
                        .sourceOrderRevision == 0 &&
                (actorStorage->runtimes()[actor.handle.slot].currentState ==
                     AIStateId::AttackObject || pendingAutonomousCommand)) {
                output.autonomousAttackSubjects.push_back(actor.subject);
            }
        }
    }
}

void ObjectAIRuntime::setPathfindCellSizeRaw(int64_t value) noexcept
{
    for (ObjectAIShadowBatch& shadow : m_shadowBatches)
        shadow.setPathfindCellSizeRaw(value);
}

void ObjectAIRuntime::setPathSequenceResolver(AIPathSequenceResolver resolver) noexcept
{
    m_pathSequenceResolver = resolver;
    for (ObjectAIShadowBatch& shadow : m_shadowBatches)
        shadow.setPathSequenceResolver(resolver);
}

void ObjectAIRuntime::setWaypointGraphResolver(AIWaypointGraphResolver resolver) noexcept
{
    m_waypointGraphResolver = resolver;
    for (ObjectAIShadowBatch& shadow : m_shadowBatches)
        shadow.setWaypointGraphResolver(resolver);
}

void ObjectAIRuntime::setPathHandleReleaser(AIPathHandleReleaser releaser) noexcept
{
    m_pathHandleReleaser = releaser;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::synchronizeOrderExternalRevision(ObjectId subject,
                                                                               uint64_t externalRevision) noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return result;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    AIStateMachineRuntime* runtime = actorStorage ? &actorStorage->runtimes()[actor->slot] : nullptr;
    const bool returnToIdle =
        runtime && (runtime->currentState == AIStateId::MoveTo || runtime->currentState == AIStateId::AttackMoveTo ||
                    runtime->currentState == AIStateId::FollowPath ||
                    runtime->currentState == AIStateId::FollowExitProductionPath ||
                    runtime->currentState == AIStateId::FollowWaypointPathAsIndividuals ||
                    runtime->currentState == AIStateId::FollowWaypointPathAsTeam ||
                    runtime->currentState == AIStateId::FollowWaypointPathAsIndividualsExact ||
                    runtime->currentState == AIStateId::FollowWaypointPathAsTeamExact ||
                    runtime->currentState == AIStateId::AttackFollowWaypointPathAsIndividuals ||
                    runtime->currentState == AIStateId::AttackFollowWaypointPathAsTeam ||
                    attackPolicyFor(runtime->currentState).valid || guardPolicyFor(runtime->currentState).valid ||
                    tacticalAttackPolicyFor(runtime->currentState).valid);
    if (returnToIdle)
    {
        const ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
        // Refuse before the admission is revoked when another system already
        // staged a transition for this subject on this tick: two same-subject
        // requests arbitrate as ConflictingNormalRequests, so neither commits
        // while the order is gone, leaving guard/tactical states running
        // against an admission that no longer exists. The caller keeps its
        // queue untouched on failure and retries once the batch is drained.
        const bool subjectAlreadyStaged = std::any_of(
            shadow.transitionRequests().begin(), shadow.transitionRequests().end(),
            [subject](const AIStateSoATransitionRequest& request) { return request.subject == subject; });
        if (subjectAlreadyStaged ||
            shadow.transitionRequests().size() >= shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
        {
            ObjectAIOrderAdmissionResult result;
            result.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
            return result;
        }
    }
    ObjectAIOrderAdmissionResult result =
        admission.synchronizeExternalRevision(admission.handle(actor->slot), externalRevision);
    if (result.succeeded() && result.action == ObjectAIOrderAdmissionAction::ExternalRevisionSynchronized)
    {
        static_cast<void>(clearTransientSubject(subject));
        if (returnToIdle)
        {
            const ObjectAIShadowBatchStatus staged = m_shadowBatches[actor->batch].stageTransitionRequest({
                .slot = actor->slot,
                .subject = subject,
                .expectedState = runtime->currentState,
                .expectedRevision = runtime->revision,
                .operation = AIStateSoATransitionOperation::Direct,
                .target = AIStateId::Idle,
                .authority = AIStateTransitionAuthority::External,
            });
            if (staged != ObjectAIShadowBatchStatus::Success)
                result.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
        }
        if (runtime)
            AIStateMachine::wake(*runtime, AIWakeReason::ExternalCommand);
    }
    if (result.succeeded() && actorStorage && runtime &&
        runtime->currentState == AIStateId::Idle &&
        !result.hasCurrentOrder)
    {
        // The ECS queue has released its last external owner.  Idle
        // auto-acquisition uses sourceOrderRevision == 0 as the explicit
        // ownership boundary; retaining the completed Move/Attack revision
        // here made an otherwise idle unit permanently ineligible to acquire
        // another target after its first command.  Clear the entire stale
        // parameter packet so old goals and path handles cannot leak into the
        // next internal Idle -> AttackObject transition either.
        actorStorage->parameters()[actor->slot] = {};
    }
    return result;
}

bool ObjectAIRuntime::acceptsMoveToCompletion(const MovementFeedback& feedback) const noexcept
{
    if (feedback.status != MovementFeedbackStatus::Completed)
        return false;
    const std::optional<AIActorHandle> actor = find(feedback.correlation.subject);
    if (!actor || !acceptsAsyncOrder(*actor, feedback.correlation.orderIdentity))
        return false;
    const AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage)
        return false;
    const AIStateId movementState = actorStorage->runtimes()[actor->slot].currentState;
    if ((movementState != AIStateId::MoveTo &&
         movementState != AIStateId::MoveAndTighten &&
         movementState != AIStateId::MoveOutOfTheWay &&
         movementState != AIStateId::AttackMoveTo) ||
        actorStorage->payloadStates()[actor->slot] != movementState)
        return false;
    ObjectAIOrderAdmissionRequest active;
    const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    if (admission.activeOrder(admission.handle(actor->slot), active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::Move ||
        active.attackMove != (movementState == AIStateId::AttackMoveTo) ||
        active.moveRouteSubtype !=
            (movementState == AIStateId::MoveOutOfTheWay
                 ? ObjectAIMoveRouteSubtype::MoveAside
             : movementState == AIStateId::MoveAndTighten
                 ? ObjectAIMoveRouteSubtype::Tighten
                 : ObjectAIMoveRouteSubtype::Direct))
        return false;
    const bool approachPayload =
        movementState == AIStateId::MoveAndTighten;
    const bool moveAside = movementState == AIStateId::MoveOutOfTheWay;
    const AIStateRequestId request = moveAside
        ? actorStorage->moveOutOfWay().load(actor->slot).request
        : approachPayload
        ? actorStorage->approachPath().load(actor->slot).request
        : actorStorage->moveTo().load(actor->slot).request;
    const uint32_t generation = moveAside
        ? actorStorage->moveOutOfWay().load(actor->slot).generation
        : approachPayload
        ? actorStorage->approachPath().load(actor->slot).generation
        : actorStorage->moveTo().load(actor->slot).generation;
    const uint64_t sourceOrderRevision = moveAside
        ? actorStorage->moveOutOfWay().load(actor->slot).sourceOrderRevision
        : approachPayload
        ? actorStorage->approachPath().load(actor->slot).sourceOrderRevision
        : actorStorage->moveTo().load(actor->slot).sourceOrderRevision;
    return feedback.correlation.subject == actorStorage->subjects()[actor->slot] &&
           feedback.correlation.stateRequest == request &&
           feedback.correlation.generation == generation &&
           feedback.correlation.sourceOrderRevision == sourceOrderRevision;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::completeMoveOrder(ObjectId subject,
                                                                const AIAsyncOrderIdentity& expectedIdentity,
                                                                ObjectAIOrderCompletion completion) noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return result;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    ObjectAIOrderAdmissionRequest active;
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    if (admission.activeOrder(handle, active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::Move || !matchesAIAsyncOrderIdentity(expectedIdentity, active.identity))
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::StaleIdentity;
        return result;
    }
    return admission.complete(handle, active.identity, completion, true);
}

std::optional<ObjectAIOrderCompletion> ObjectAIRuntime::moveOrderOutcome(
    ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity) const noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
        return std::nullopt;
    const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(admission.handle(actor->slot), active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::Move || active.identity != expectedIdentity)
        return std::nullopt;
    const AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage)
        return std::nullopt;
    const AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    if (runtime.currentState != AIStateId::Idle ||
        (runtime.previousState != AIStateId::MoveTo &&
         runtime.previousState != AIStateId::MoveAndTighten &&
         runtime.previousState != AIStateId::MoveOutOfTheWay &&
         runtime.previousState != AIStateId::AttackMoveTo))
        return std::nullopt;
    if (runtime.lastTransitionReason == AIStateTransitionReason::Success)
        return ObjectAIOrderCompletion::Success;
    if (runtime.lastTransitionReason == AIStateTransitionReason::Failure)
        return ObjectAIOrderCompletion::Failed;
    return std::nullopt;
}

std::optional<ObjectAIOrderCompletion>
ObjectAIRuntime::combatDropOrderOutcome(
    ObjectId subject,
    const AIAsyncOrderIdentity& expectedIdentity) const noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
        return std::nullopt;
    const ObjectAIOrderAdmissionStorage& admission =
        m_orderAdmissions[actor->batch];
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(admission.handle(actor->slot), active) !=
            ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::Move ||
        !matchesAIAsyncOrderIdentity(expectedIdentity, active.identity))
        return std::nullopt;
    const AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage)
        return std::nullopt;
    const AIStateMachineRuntime& runtime =
        actorStorage->runtimes()[actor->slot];
    if (runtime.currentState != AIStateId::Idle ||
        runtime.previousState != AIStateId::CombatDrop)
        return std::nullopt;
    if (runtime.lastTransitionReason == AIStateTransitionReason::Success)
        return ObjectAIOrderCompletion::Success;
    if (runtime.lastTransitionReason == AIStateTransitionReason::Failure)
        return ObjectAIOrderCompletion::Failed;
    return std::nullopt;
}

std::optional<ObjectAIOrderCompletion> ObjectAIRuntime::attackOrderOutcome(
    ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity) const noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
        return std::nullopt;
    const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(admission.handle(actor->slot), active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::Attack || active.identity != expectedIdentity)
        return std::nullopt;
    const AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage)
        return std::nullopt;
    const AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    if (runtime.currentState != AIStateId::Idle || !attackPolicyFor(runtime.previousState).valid)
        return std::nullopt;
    if (runtime.lastTransitionReason == AIStateTransitionReason::Success)
        return ObjectAIOrderCompletion::Success;
    if (runtime.lastTransitionReason == AIStateTransitionReason::Failure)
        return ObjectAIOrderCompletion::Failed;
    return std::nullopt;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::completeAttackOrder(ObjectId subject,
                                                                  const AIAsyncOrderIdentity& expectedIdentity,
                                                                  ObjectAIOrderCompletion completion) noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return result;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    ObjectAIOrderAdmissionRequest active;
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    if (admission.activeOrder(handle, active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::Attack || !matchesAIAsyncOrderIdentity(expectedIdentity, active.identity))
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::StaleIdentity;
        return result;
    }
    return admission.complete(handle, active.identity, completion, true);
}

bool ObjectAIRuntime::huntOrderTerminal(ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity) const noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
        return false;
    const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(admission.handle(actor->slot), active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::TacticalAttack ||
        active.tacticalAttackSubtype != ObjectAITacticalAttackSubtype::Hunt || active.identity != expectedIdentity)
        return false;
    const AIStateFamilySoAStorage* actorStorage = storage(*actor);
    return actorStorage && actorStorage->runtimes()[actor->slot].currentState == AIStateId::Idle &&
           actorStorage->payloadStates()[actor->slot] == AIStateId::Idle;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::completeHuntOrder(ObjectId subject,
                                                                const ObjectAIOrderIdentity& expectedIdentity,
                                                                ObjectAIOrderCompletion completion) noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return result;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(handle, active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::TacticalAttack ||
        active.tacticalAttackSubtype != ObjectAITacticalAttackSubtype::Hunt || active.identity != expectedIdentity)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::StaleIdentity;
        return result;
    }
    return admission.complete(handle, active.identity, completion, true);
}

std::optional<ObjectAIOrderCompletion>
ObjectAIRuntime::tacticalAttackOrderOutcome(
    ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity,
    ObjectAITacticalAttackSubtype subtype) const noexcept
{
    if (subtype != ObjectAITacticalAttackSubtype::Hunt && subtype != ObjectAITacticalAttackSubtype::AttackSquad &&
        subtype != ObjectAITacticalAttackSubtype::AttackArea &&
        subtype != ObjectAITacticalAttackSubtype::GuardRetaliate &&
        subtype != ObjectAITacticalAttackSubtype::GuardTunnelNetwork)
        return std::nullopt;
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
        return std::nullopt;
    const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(admission.handle(actor->slot), active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::TacticalAttack || active.tacticalAttackSubtype != subtype ||
        active.identity != expectedIdentity)
        return std::nullopt;
    const AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage) return std::nullopt;
    const AIStateMachineRuntime& runtime =
        actorStorage->runtimes()[actor->slot];
    if (runtime.currentState != AIStateId::Idle ||
        actorStorage->payloadStates()[actor->slot] != AIStateId::Idle)
        return std::nullopt;
    const AIStateId expectedPrevious =
        subtype == ObjectAITacticalAttackSubtype::Hunt
        ? AIStateId::Hunt
        : subtype == ObjectAITacticalAttackSubtype::AttackSquad
        ? AIStateId::AttackSquad
        : subtype == ObjectAITacticalAttackSubtype::AttackArea
        ? AIStateId::AttackArea
        : subtype == ObjectAITacticalAttackSubtype::GuardRetaliate
        ? AIStateId::GuardRetaliate
        : AIStateId::GuardTunnelNetwork;
    if (runtime.previousState != expectedPrevious) return std::nullopt;
    if (runtime.lastTransitionReason == AIStateTransitionReason::Success)
        return ObjectAIOrderCompletion::Success;
    if (runtime.lastTransitionReason == AIStateTransitionReason::Failure)
        return ObjectAIOrderCompletion::Failed;
    return std::nullopt;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::completeTacticalAttackOrder(ObjectId subject,
                                                                          const ObjectAIOrderIdentity& expectedIdentity,
                                                                          ObjectAITacticalAttackSubtype subtype,
                                                                          ObjectAIOrderCompletion completion) noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return result;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(handle, active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::TacticalAttack || active.tacticalAttackSubtype != subtype ||
        active.identity != expectedIdentity)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::StaleIdentity;
        return result;
    }
    return admission.complete(handle, active.identity, completion, true);
}

std::optional<ObjectAIOrderCompletion> ObjectAIRuntime::waypointOrderOutcome(
    ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity, ObjectAIMoveRouteSubtype subtype) const noexcept
{
    if (!isObjectAIWaypointRouteSubtype(subtype))
        return std::nullopt;
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
        return std::nullopt;
    const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(admission.handle(actor->slot), active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::Move || active.moveRouteSubtype != subtype ||
        active.identity != expectedIdentity)
    {
        return std::nullopt;
    }
    const AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage || actorStorage->runtimes()[actor->slot].currentState != AIStateId::Idle ||
        actorStorage->payloadStates()[actor->slot] != AIStateId::Idle)
    {
        return std::nullopt;
    }
    const AIWaypointPathStatePayload payload = actorStorage->waypointPath().load(actor->slot);
    return payload.completionTerminal ? ObjectAIOrderCompletion::Success : ObjectAIOrderCompletion::Failed;
}

std::optional<AIWaypointHandle> ObjectAIRuntime::waypointOrderCompletedWaypoint(
    ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity, ObjectAIMoveRouteSubtype subtype) const noexcept
{
    if (!isObjectAIWaypointRouteSubtype(subtype))
        return std::nullopt;
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
        return std::nullopt;
    const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(admission.handle(actor->slot), active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::Move || active.moveRouteSubtype != subtype ||
        active.identity != expectedIdentity)
    {
        return std::nullopt;
    }
    const AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage || actorStorage->runtimes()[actor->slot].currentState != AIStateId::Idle ||
        actorStorage->payloadStates()[actor->slot] != AIStateId::Idle)
    {
        return std::nullopt;
    }
    const AIWaypointHandle terminal = actorStorage->waypointPath().load(actor->slot).completionTerminal;
    return terminal ? std::optional<AIWaypointHandle>{terminal} : std::nullopt;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::completeWaypointOrder(ObjectId subject,
                                                                    const ObjectAIOrderIdentity& expectedIdentity,
                                                                    ObjectAIOrderCompletion completion,
                                                                    ObjectAIMoveRouteSubtype subtype) noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return result;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(handle, active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::Move || active.moveRouteSubtype != subtype ||
        active.identity != expectedIdentity)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::StaleIdentity;
        return result;
    }
    return admission.complete(handle, active.identity, completion, true);
}

std::optional<ObjectAIOrderCompletion> ObjectAIRuntime::followPathOrderOutcome(
    ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity, ObjectAIMoveRouteSubtype subtype) const noexcept
{
    if (subtype != ObjectAIMoveRouteSubtype::FollowPath &&
        subtype != ObjectAIMoveRouteSubtype::FollowExitProductionPath)
        return std::nullopt;
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
        return std::nullopt;
    const ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(admission.handle(actor->slot), active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::Move || active.moveRouteSubtype != subtype ||
        active.identity != expectedIdentity)
    {
        return std::nullopt;
    }
    const AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage || actorStorage->runtimes()[actor->slot].currentState != AIStateId::Idle ||
        actorStorage->payloadStates()[actor->slot] != AIStateId::Idle)
    {
        return std::nullopt;
    }
    const AIFollowPathStatePayload payload = actorStorage->followPath().load(actor->slot);
    return m_pathSequenceResolver.query(payload.sequence, payload.sequenceRevision, payload.index).status ==
                   AIPathSequenceQueryStatus::End
               ? ObjectAIOrderCompletion::Success
               : ObjectAIOrderCompletion::Failed;
}

std::optional<uint32_t> ObjectAIRuntime::followPathCurrentPointIndex(
    ObjectId subject, const ObjectAIOrderIdentity& expectedIdentity,
    ObjectAIMoveRouteSubtype subtype) const noexcept
{
    if (subtype != ObjectAIMoveRouteSubtype::FollowPath &&
        subtype != ObjectAIMoveRouteSubtype::FollowExitProductionPath)
        return std::nullopt;
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor) return std::nullopt;
    const ObjectAIOrderAdmissionStorage& admission =
        m_orderAdmissions[actor->batch];
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(admission.handle(actor->slot), active) !=
            ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::Move ||
        active.moveRouteSubtype != subtype ||
        active.identity != expectedIdentity) {
        return std::nullopt;
    }
    const AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage) return std::nullopt;
    const AIFollowPathStatePayload payload =
        actorStorage->followPath().load(actor->slot);
    return payload.sequence ? std::optional<uint32_t>{payload.index}
                            : std::nullopt;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::completeFollowPathOrder(ObjectId subject,
                                                                      const ObjectAIOrderIdentity& expectedIdentity,
                                                                      ObjectAIOrderCompletion completion,
                                                                      ObjectAIMoveRouteSubtype subtype) noexcept
{
    if (subtype != ObjectAIMoveRouteSubtype::FollowPath &&
        subtype != ObjectAIMoveRouteSubtype::FollowExitProductionPath)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::InvalidOrderKind;
        return result;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return result;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    ObjectAIOrderAdmissionRequest active;
    if (admission.activeOrder(handle, active) != ObjectAIOrderAdmissionStatus::Success ||
        active.kind != ObjectAIOrderKind::Move || active.moveRouteSubtype != subtype ||
        active.identity != expectedIdentity)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::StaleIdentity;
        return result;
    }
    return admission.complete(handle, active.identity, completion, true);
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeFollowPathOrder(ObjectId subject,
                                                                     const ObjectAIOrderIdentity& identity,
                                                                     const AIStateParameters& parameters,
                                                                     ObjectAIMoveRouteSubtype subtype)
{
    ObjectAIOrderAdmissionResult invalid;
    const bool ordinaryFollow = subtype == ObjectAIMoveRouteSubtype::FollowPath;
    const bool productionExit = subtype == ObjectAIMoveRouteSubtype::FollowExitProductionPath;
    const AIStateId target = ordinaryFollow ? AIStateId::FollowPath : AIStateId::FollowExitProductionPath;
    const bool playerFollow = ordinaryFollow &&
        identity.source == ObjectAIOrderSource::Player &&
        identity.systemPurpose == ObjectAIOrderSystemPurpose::Generic;
    const bool systemFollow = ordinaryFollow &&
        identity.source == ObjectAIOrderSource::System &&
        identity.systemPurpose == ObjectAIOrderSystemPurpose::ContainmentExit;
    const bool systemProductionExit = productionExit &&
        identity.source == ObjectAIOrderSource::System &&
        identity.systemPurpose == ObjectAIOrderSystemPurpose::ProductionExit;
    if (!ordinaryFollow && !productionExit) {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidOrderKind;
        return invalid;
    }
    if (!playerFollow && !systemFollow && !systemProductionExit) {
        invalid.status = identity.source != ObjectAIOrderSource::Player &&
                identity.source != ObjectAIOrderSource::System
            ? ObjectAIOrderAdmissionStatus::InvalidOrderSource
            : ObjectAIOrderAdmissionStatus::InvalidSystemPurpose;
        return invalid;
    }
    if (!acceptsProductionAdmission(
            target, AIProductionAdmission::MoveOrder)) {
        invalid.status = ObjectAIOrderAdmissionStatus::UnsupportedOrder;
        return invalid;
    }
    if (!parameters.pathSequence ||
        parameters.pathSequenceRevision == 0) {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidSnapshot;
        return invalid;
    }
    if (parameters.sourceOrderRevision != identity.queueRevision) {
        invalid.status = ObjectAIOrderAdmissionStatus::StaleQueueRevision;
        return invalid;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return invalid;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    const ObjectAIOrderAdmissionRequest request{
        .kind = ObjectAIOrderKind::Move,
        .identity = identity,
        .moveRouteSubtype = subtype,
    };
    ObjectAIOrderAdmissionRequest active;
    const bool hasActive = admission.activeOrder(handle, active) == ObjectAIOrderAdmissionStatus::Success;
    if (hasActive && active == request)
    {
        ObjectAIOrderAdmissionResult result;
        result.handle = handle;
        result.owner = ObjectAIOrderOwner::ObjectAIRuntime;
        result.currentOrder = active;
        result.hasCurrentOrder = true;
        return result;
    }
    const bool appendsActivePlayerPath = hasActive && playerFollow &&
        active.kind == ObjectAIOrderKind::Move &&
        active.moveRouteSubtype == ObjectAIMoveRouteSubtype::FollowPath &&
        active.identity.subject == identity.subject &&
        active.identity.source == ObjectAIOrderSource::Player &&
        active.identity.systemPurpose == ObjectAIOrderSystemPurpose::Generic &&
        active.identity.issuedTick == identity.issuedTick &&
        active.identity.sourceSequence == identity.sourceSequence &&
        active.identity.sourceScriptId == identity.sourceScriptId &&
        active.identity.systemPurposeInstance ==
            identity.systemPurposeInstance &&
        active.identity.queueRevision <= identity.queueRevision &&
        active.identity.externalRevision <= identity.externalRevision;
    if (appendsActivePlayerPath)
    {
        ObjectAIOrderAdmissionResult result = admission.replace(
            handle, active.identity, request);
        if (!result.succeeded())
            return result;
        AIStateFamilySoAStorage* actorStorage = storage(*actor);
        if (!actorStorage)
        {
            static_cast<void>(admission.cancel(handle, identity));
            result.status = ObjectAIOrderAdmissionStatus::InvalidSlot;
            return result;
        }
        // Keep the current FollowPath payload/index and all in-flight path
        // correlations. The resolver revision is intentionally stable for a
        // tail append; only future point queries observe the longer vector.
        actorStorage->parameters()[actor->slot] = parameters;
        return result;
    }
    const ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (shadow.transitionRequests().size() >= shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
        return invalid;
    }
    ObjectAIOrderAdmissionResult result =
        hasActive ? admission.replace(handle, active.identity, request) : admission.admit(handle, request);
    if (!result.succeeded())
        return result;
    if (hasActive && result.action == ObjectAIOrderAdmissionAction::Replaced)
        static_cast<void>(clearTransientSubject(subject));

    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::InvalidSlot;
        return result;
    }
    actorStorage->parameters()[actor->slot] = parameters;
    AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    const ObjectAIShadowBatchStatus staged = m_shadowBatches[actor->batch].stageTransitionRequest({
        .slot = actor->slot,
        .subject = subject,
        .expectedState = runtime.currentState,
        .expectedRevision = runtime.revision,
        .operation = AIStateSoATransitionOperation::Direct,
        .target = target,
        .authority = AIStateTransitionAuthority::External,
        .reenter =
            runtime.currentState == (ordinaryFollow ? AIStateId::FollowPath : AIStateId::FollowExitProductionPath),
    });
    if (staged != ObjectAIShadowBatchStatus::Success)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
    }
    return result;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeWaypointOrder(ObjectId subject,
                                                                   const ObjectAIOrderIdentity& identity,
                                                                   const AIStateParameters& parameters,
                                                                   ObjectAIMoveRouteSubtype subtype,
                                                                   bool attackFollow)
{
    ObjectAIOrderAdmissionResult invalid;
    const bool teamRoute = objectAIWaypointRouteMovesAsTeam(subtype);
    AIStateId target = AIStateId::Invalid;
    switch (subtype)
    {
    case ObjectAIMoveRouteSubtype::WaypointPathIndividuals:
        target = attackFollow ? AIStateId::AttackFollowWaypointPathAsIndividuals
                              : AIStateId::FollowWaypointPathAsIndividuals;
        break;
    case ObjectAIMoveRouteSubtype::WaypointPathTeam:
        target = attackFollow ? AIStateId::AttackFollowWaypointPathAsTeam : AIStateId::FollowWaypointPathAsTeam;
        break;
    case ObjectAIMoveRouteSubtype::WaypointPathIndividualsExact:
        target = AIStateId::FollowWaypointPathAsIndividualsExact;
        break;
    case ObjectAIMoveRouteSubtype::WaypointPathTeamExact:
        target = AIStateId::FollowWaypointPathAsTeamExact;
        break;
    case ObjectAIMoveRouteSubtype::WanderWaypointPath:
        target = AIStateId::Wander;
        break;
    case ObjectAIMoveRouteSubtype::PanicWaypointPath:
        target = AIStateId::Panic;
        break;
    default:
        break;
    }
    if (!isObjectAIWaypointRouteSubtype(subtype) ||
        !acceptsProductionAdmission(target, AIProductionAdmission::MoveOrder) ||
        (attackFollow && subtype != ObjectAIMoveRouteSubtype::WaypointPathIndividuals &&
         subtype != ObjectAIMoveRouteSubtype::WaypointPathTeam) ||
        identity.source != ObjectAIOrderSource::Script ||
        identity.systemPurpose != ObjectAIOrderSystemPurpose::Generic || !parameters.waypoint ||
        parameters.waypointGraphRevision == 0 || parameters.sourceOrderRevision != identity.queueRevision ||
        (teamRoute ? !parameters.waypointTeam : static_cast<bool>(parameters.waypointTeam)))
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidOrderKind;
        return invalid;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return invalid;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    const ObjectAIOrderAdmissionRequest request{
        .kind = ObjectAIOrderKind::Move,
        .identity = identity,
        .attackMove = attackFollow,
        .moveRouteSubtype = subtype,
        .waypointStart = parameters.waypoint,
        .waypointGraphRevision = parameters.waypointGraphRevision,
        .waypointTeam = parameters.waypointTeam,
    };
    ObjectAIOrderAdmissionRequest active;
    const bool hasActive = admission.activeOrder(handle, active) == ObjectAIOrderAdmissionStatus::Success;
    if (hasActive && active == request)
    {
        ObjectAIOrderAdmissionResult result;
        result.handle = handle;
        result.owner = ObjectAIOrderOwner::ObjectAIRuntime;
        result.currentOrder = active;
        result.hasCurrentOrder = true;
        return result;
    }
    const ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (shadow.transitionRequests().size() >= shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
        return invalid;
    }
    ObjectAIOrderAdmissionResult result =
        hasActive ? admission.replace(handle, active.identity, request) : admission.admit(handle, request);
    if (!result.succeeded())
        return result;
    if (hasActive && result.action == ObjectAIOrderAdmissionAction::Replaced)
        static_cast<void>(clearTransientSubject(subject));

    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::InvalidSlot;
        return result;
    }
    actorStorage->parameters()[actor->slot] = parameters;
    AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    const ObjectAIShadowBatchStatus staged = m_shadowBatches[actor->batch].stageTransitionRequest({
        .slot = actor->slot,
        .subject = subject,
        .expectedState = runtime.currentState,
        .expectedRevision = runtime.revision,
        .operation = AIStateSoATransitionOperation::Direct,
        .target = target,
        .authority = AIStateTransitionAuthority::External,
        .reenter = runtime.currentState == target,
    });
    if (staged != ObjectAIShadowBatchStatus::Success)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
    }
    else if (teamRoute)
    {
        ensureWaypointTeamProgress(identity, parameters);
    }
    return result;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeMoveOrder(ObjectId subject,
                                                               const ObjectAIOrderIdentity& identity,
                                                               const AIStateParameters& parameters,
                                                               ObjectAIMoveRouteSubtype subtype,
                                                               uint64_t /*confirmedTick*/)
{
    const AIStateId target = subtype == ObjectAIMoveRouteSubtype::WanderInPlace
        ? AIStateId::WanderInPlace
        : subtype == ObjectAIMoveRouteSubtype::MoveAside
        ? AIStateId::MoveOutOfTheWay
        : subtype == ObjectAIMoveRouteSubtype::Tighten
            ? AIStateId::MoveAndTighten
            : AIStateId::MoveTo;
    const AIProductionStateRoute* route = productionStateRouteFor(target);
    const bool specializedMoveAside =
        subtype == ObjectAIMoveRouteSubtype::MoveAside && route &&
        route->status == AIProductionRouteStatus::SpecializedOwner;
    if ((subtype != ObjectAIMoveRouteSubtype::Direct &&
         subtype != ObjectAIMoveRouteSubtype::Tighten &&
         subtype != ObjectAIMoveRouteSubtype::MoveAside &&
         subtype != ObjectAIMoveRouteSubtype::WanderInPlace) ||
        (!specializedMoveAside &&
         !acceptsProductionAdmission(
             target, AIProductionAdmission::MoveOrder)))
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::UnsupportedOrder;
        return result;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return result;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    const ObjectAIOrderAdmissionRequest request{
        .kind = ObjectAIOrderKind::Move,
        .identity = identity,
        .moveRouteSubtype = subtype,
    };
    ObjectAIOrderAdmissionRequest active;
    const bool hasActive = admission.activeOrder(handle, active) == ObjectAIOrderAdmissionStatus::Success;
    if (hasActive && active == request)
    {
        ObjectAIOrderAdmissionResult result;
        result.handle = handle;
        result.owner = ObjectAIOrderOwner::ObjectAIRuntime;
        result.currentOrder = active;
        result.hasCurrentOrder = true;
        return result;
    }

    const ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (shadow.transitionRequests().size() >= shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
    {
        ObjectAIOrderAdmissionResult result;
        result.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
        return result;
    }

    ObjectAIOrderAdmissionResult result =
        hasActive ? admission.replace(handle, active.identity, request) : admission.admit(handle, request);
    if (!result.succeeded())
        return result;
    if (hasActive && result.action == ObjectAIOrderAdmissionAction::Replaced)
        static_cast<void>(clearTransientSubject(subject));

    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::InvalidSlot;
        return result;
    }
    actorStorage->parameters()[actor->slot] = parameters;
    AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    const ObjectAIShadowBatchStatus staged = m_shadowBatches[actor->batch].stageTransitionRequest({
        .slot = actor->slot,
        .subject = subject,
        .expectedState = runtime.currentState,
        .expectedRevision = runtime.revision,
        .operation = AIStateSoATransitionOperation::Direct,
        .target = target,
        .authority = AIStateTransitionAuthority::External,
        .reenter = runtime.currentState == target,
    });
    if (staged != ObjectAIShadowBatchStatus::Success)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
    }
    return result;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeCombatDropOrder(
    ObjectId subject, const ObjectAIOrderIdentity& identity,
    const AIStateParameters& parameters, uint64_t confirmedTick,
    AIInsertionMotionFeedback entryFeedback)
{
    ObjectAIOrderAdmissionResult invalid;
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return invalid;
    }
    if (m_pendingInsertionEntryFeedback.size() >= m_config.maximumActors)
    {
        invalid.status =
            ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
        return invalid;
    }
    ObjectAIOrderAdmissionStorage& admission =
        m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    const ObjectAIOrderAdmissionRequest request{
        .kind = ObjectAIOrderKind::Move,
        .identity = identity,
        .moveRouteSubtype = ObjectAIMoveRouteSubtype::Direct,
    };
    ObjectAIOrderAdmissionRequest active;
    const bool hasActive =
        admission.activeOrder(handle, active) ==
        ObjectAIOrderAdmissionStatus::Success;
    if (hasActive && active == request)
    {
        invalid.handle = handle;
        invalid.owner = ObjectAIOrderOwner::ObjectAIRuntime;
        invalid.currentOrder = active;
        invalid.hasCurrentOrder = true;
        return invalid;
    }
    const ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (shadow.transitionRequests().size() >=
        shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
    {
        invalid.status =
            ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
        return invalid;
    }
    ObjectAIOrderAdmissionResult result = hasActive
        ? admission.replace(handle, active.identity, request)
        : admission.admit(handle, request);
    if (!result.succeeded())
        return result;
    if (hasActive)
        static_cast<void>(clearTransientSubject(subject));

    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::InvalidSlot;
        return result;
    }
    uint32_t sequence = actorStorage->activationSequences()[actor->slot];
    ++sequence;
    if (sequence == 0)
        ++sequence;
    entryFeedback.correlation = {
        .subject = subject,
        .stateRequest = {confirmedTick, sequence},
        .state = AIStateId::CombatDrop,
    };
    actorStorage->parameters()[actor->slot] = parameters;
    AIStateMachineRuntime& runtime =
        actorStorage->runtimes()[actor->slot];
    if (m_shadowBatches[actor->batch].stageTransitionRequest({
            .slot = actor->slot,
            .subject = subject,
            .expectedState = runtime.currentState,
            .expectedRevision = runtime.revision,
            .operation = AIStateSoATransitionOperation::Direct,
            .target = AIStateId::CombatDrop,
            .correlationIssuedTick = confirmedTick,
            .authority = AIStateTransitionAuthority::External,
            .reenter = runtime.currentState == AIStateId::CombatDrop,
        }) != ObjectAIShadowBatchStatus::Success)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status =
            ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
        return result;
    }
    m_pendingInsertionEntryFeedback.push_back(entryFeedback);
    return result;
}

bool ObjectAIRuntime::activateRepairDock(
    ObjectId subject, ObjectId dock, uint64_t /*confirmedTick*/,
    uint64_t externalOrderRevision)
{
    if (!m_initialized || !subject || !dock)
        return false;
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor || actor->batch >= m_shadowBatches.size())
        return false;
    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage || actor->slot >= actorStorage->runtimes().size())
        return false;
    ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (std::any_of(shadow.transitionRequests().begin(),
                    shadow.transitionRequests().end(),
                    [subject](const AIStateSoATransitionRequest& request) { return request.subject == subject; }))
    {
        return false;
    }
    if (shadow.transitionRequests().size() >=
        shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
        return false;
    AIStateParameters parameters = actorStorage->parameters()[actor->slot];
    parameters.goalObject = dock;
    parameters.hasGoalPosition = false;
    const AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    if (externalOrderRevision != 0)
    {
        ObjectAIOrderAdmissionStorage& admission =
            m_orderAdmissions[actor->batch];
        const ObjectAIOrderAdmissionResult synchronized =
            admission.synchronizeExternalRevision(
                admission.handle(actor->slot), externalOrderRevision);
        if (!synchronized.succeeded()) return false;
        static_cast<void>(clearTransientSubject(subject));
    }
    if (shadow.stageTransitionRequest({
               .slot = actor->slot,
               .subject = subject,
               .expectedState = runtime.currentState,
               .expectedRevision = runtime.revision,
               .operation = AIStateSoATransitionOperation::Direct,
               .target = AIStateId::GetRepaired,
               .authority = AIStateTransitionAuthority::External,
               .reenter = runtime.currentState == AIStateId::GetRepaired,
           }) != ObjectAIShadowBatchStatus::Success)
        return false;
    actorStorage->parameters()[actor->slot] = parameters;
    return true;
}

bool ObjectAIRuntime::activateRepulsorEscape(
    ObjectId subject, uint64_t /*confirmedTick*/)
{
    if (!m_initialized || !subject)
        return false;
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor || actor->batch >= m_shadowBatches.size())
        return false;
    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage || actor->slot >= actorStorage->runtimes().size())
        return false;
    ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    AIStateParameters& parameters = actorStorage->parameters()[actor->slot];
    if (runtime.currentState != AIStateId::Idle ||
        parameters.sourceOrderRevision != 0 ||
        std::any_of(shadow.transitionRequests().begin(),
                    shadow.transitionRequests().end(),
                    [subject](const AIStateSoATransitionRequest& request) {
                        return request.subject == subject;
                    }) ||
        shadow.transitionRequests().size() >=
            shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
        return false;

    // Internal children do not have an ECS ObjectOrderIntent. Their legal
    // correlation identity is the state-machine revision that the staged
    // transition will commit. Keep zero reserved for "no owner".
    const uint64_t internalRevision = runtime.revision ==
            std::numeric_limits<uint64_t>::max()
        ? std::numeric_limits<uint64_t>::max()
        : runtime.revision + 1u;
    parameters.sourceOrderRevision =
        std::max<uint64_t>(1u, internalRevision);
    if (shadow.stageTransitionRequest({
            .slot = actor->slot,
            .subject = subject,
            .expectedState = runtime.currentState,
            .expectedRevision = runtime.revision,
            .operation = AIStateSoATransitionOperation::Direct,
            .target = AIStateId::MoveAwayFromRepulsors,
            .authority = AIStateTransitionAuthority::Internal,
        }) != ObjectAIShadowBatchStatus::Success) {
        parameters.sourceOrderRevision = 0;
        return false;
    }
    return true;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeAttackOrder(ObjectId subject,
                                                                 const ObjectAIOrderIdentity& identity,
                                                                 AIStateId attackState,
                                                                 const AIStateParameters& parameters)
{
    ObjectAIOrderAdmissionResult invalid;
    if (!attackPolicyFor(attackState).valid ||
        !acceptsProductionAdmission(attackState, AIProductionAdmission::AttackOrder))
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidOrderKind;
        return invalid;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return invalid;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    const ObjectAIOrderAdmissionRequest request{.kind = ObjectAIOrderKind::Attack, .identity = identity};
    ObjectAIOrderAdmissionRequest active;
    const bool hasActive = admission.activeOrder(handle, active) == ObjectAIOrderAdmissionStatus::Success;
    if (hasActive && active == request)
    {
        invalid.handle = handle;
        invalid.owner = ObjectAIOrderOwner::ObjectAIRuntime;
        invalid.currentOrder = active;
        invalid.hasCurrentOrder = true;
        return invalid;
    }
    const ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (shadow.transitionRequests().size() >= shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
        return invalid;
    }
    ObjectAIOrderAdmissionResult result =
        hasActive ? admission.replace(handle, active.identity, request) : admission.admit(handle, request);
    if (!result.succeeded())
        return result;
    if (hasActive)
        static_cast<void>(clearTransientSubject(subject));
    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::InvalidSlot;
        return result;
    }
    actorStorage->parameters()[actor->slot] = parameters;
    AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    if (m_shadowBatches[actor->batch].stageTransitionRequest({
            .slot = actor->slot,
            .subject = subject,
            .expectedState = runtime.currentState,
            .expectedRevision = runtime.revision,
            .operation = AIStateSoATransitionOperation::Direct,
            .target = attackState,
            .authority = AIStateTransitionAuthority::External,
            .reenter = attackPolicyFor(runtime.currentState).valid,
        }) != ObjectAIShadowBatchStatus::Success)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
    }
    return result;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeAttackMoveOrder(ObjectId subject,
                                                                     const ObjectAIOrderIdentity& identity,
                                                                     const AIStateParameters& parameters,
                                                                     uint64_t /*confirmedTick*/)
{
    ObjectAIOrderAdmissionResult invalid;
    if (!acceptsProductionAdmission(AIStateId::AttackMoveTo, AIProductionAdmission::MoveOrder))
    {
        invalid.status = ObjectAIOrderAdmissionStatus::UnsupportedOrder;
        return invalid;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return invalid;
    }

    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    const ObjectAIOrderAdmissionRequest request{
        .kind = ObjectAIOrderKind::Move,
        .identity = identity,
        .attackMove = true,
    };
    ObjectAIOrderAdmissionRequest active;
    const bool hasActive = admission.activeOrder(handle, active) == ObjectAIOrderAdmissionStatus::Success;
    if (hasActive && active == request)
    {
        invalid.handle = handle;
        invalid.owner = ObjectAIOrderOwner::ObjectAIRuntime;
        invalid.currentOrder = active;
        invalid.hasCurrentOrder = true;
        return invalid;
    }

    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    const ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (!actorStorage ||
        shadow.transitionRequests().size() >= shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
    {
        invalid.status = actorStorage ? ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded
                                      : ObjectAIOrderAdmissionStatus::InvalidSlot;
        return invalid;
    }

    // Admission is performed only after transition capacity and actor
    // storage have been reserved by the checks above. stageTransitionRequest
    // can then only append to the pre-sized bounded buffer.
    ObjectAIOrderAdmissionResult result =
        hasActive ? admission.replace(handle, active.identity, request) : admission.admit(handle, request);
    if (!result.succeeded())
        return result;

    actorStorage->parameters()[actor->slot] = parameters;
    AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    const ObjectAIShadowBatchStatus staged = m_shadowBatches[actor->batch].stageTransitionRequest({
        .slot = actor->slot,
        .subject = subject,
        .expectedState = runtime.currentState,
        .expectedRevision = runtime.revision,
        .operation = AIStateSoATransitionOperation::Direct,
        .target = AIStateId::AttackMoveTo,
        .authority = AIStateTransitionAuthority::External,
        .reenter = runtime.currentState == AIStateId::AttackMoveTo,
    });
    if (staged != ObjectAIShadowBatchStatus::Success)
    {
        // The preflight makes this unreachable for an initialized runtime;
        // retain the defensive rollback used by the other admission paths.
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
        return result;
    }
    if (hasActive && result.action == ObjectAIOrderAdmissionAction::Replaced)
        static_cast<void>(clearTransientSubject(subject));
    return result;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeHuntOrder(ObjectId subject,
                                                               const ObjectAIOrderIdentity& identity,
                                                               bool allArmyHunt,
                                                               bool useTeamCommonTarget)
{
    ObjectAIOrderAdmissionResult invalid;
    const bool scriptHunt = identity.source == ObjectAIOrderSource::Script &&
        identity.systemPurpose == ObjectAIOrderSystemPurpose::Generic;
    const bool commandButtonHunt =
        identity.source == ObjectAIOrderSource::System &&
        identity.systemPurpose ==
            ObjectAIOrderSystemPurpose::CommandButtonHunt;
    if (!acceptsProductionAdmission(AIStateId::Hunt,
                                    AIProductionAdmission::TacticalOrder) ||
        (!scriptHunt && !commandButtonHunt))
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidOrderKind;
        return invalid;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return invalid;
    }

    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    const ObjectAIOrderAdmissionRequest request{
        .kind = ObjectAIOrderKind::TacticalAttack,
        .identity = identity,
        .tacticalAttackSubtype = ObjectAITacticalAttackSubtype::Hunt,
        .allArmyHunt = allArmyHunt,
        .useTeamCommonTarget = useTeamCommonTarget,
    };
    ObjectAIOrderAdmissionRequest active;
    const bool hasActive = admission.activeOrder(handle, active) == ObjectAIOrderAdmissionStatus::Success;
    if (hasActive && active == request)
    {
        invalid.handle = handle;
        invalid.owner = ObjectAIOrderOwner::ObjectAIRuntime;
        invalid.currentOrder = active;
        invalid.hasCurrentOrder = true;
        return invalid;
    }

    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    const ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (!actorStorage ||
        shadow.transitionRequests().size() >= shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
    {
        invalid.status = actorStorage ? ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded
                                      : ObjectAIOrderAdmissionStatus::InvalidSlot;
        return invalid;
    }

    ObjectAIOrderAdmissionResult result =
        hasActive ? admission.replace(handle, active.identity, request) : admission.admit(handle, request);
    if (!result.succeeded())
        return result;
    if (hasActive)
        static_cast<void>(clearTransientSubject(subject));

    AIStateParameters parameters;
    parameters.sourceOrderRevision = identity.queueRevision;
    parameters.allArmyHunt = allArmyHunt;
    parameters.useTeamCommonTarget = useTeamCommonTarget;
    actorStorage->parameters()[actor->slot] = parameters;
    AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    if (m_shadowBatches[actor->batch].stageTransitionRequest({
            .slot = actor->slot,
            .subject = subject,
            .expectedState = runtime.currentState,
            .expectedRevision = runtime.revision,
            .operation = AIStateSoATransitionOperation::Direct,
            .target = AIStateId::Hunt,
            .authority = AIStateTransitionAuthority::External,
            .reenter = runtime.currentState == AIStateId::Hunt,
        }) != ObjectAIShadowBatchStatus::Success)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
    }
    return result;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeTacticalDomainOrder(ObjectId subject,
                                                                         const ObjectAIOrderIdentity& identity,
                                                                         ObjectAITacticalAttackSubtype subtype,
                                                                         AITargetCollectionHandle collection,
                                                                         uint64_t collectionRevision,
                                                                         AIAttackAreaHandle area,
                                                                         uint64_t areaRevision,
                                                                         AISquadTargetSelection squadSelection)
{
    ObjectAIOrderAdmissionResult invalid;
    const bool squad = subtype == ObjectAITacticalAttackSubtype::AttackSquad;
    const bool attackArea = subtype == ObjectAITacticalAttackSubtype::AttackArea;
    const AIStateId target = squad ? AIStateId::AttackSquad : AIStateId::AttackArea;
    if (!acceptsProductionAdmission(target, AIProductionAdmission::TacticalOrder) ||
        identity.source != ObjectAIOrderSource::Script ||
        identity.systemPurpose != ObjectAIOrderSystemPurpose::Generic || squad == attackArea ||
        (squad && (!collection || collectionRevision == 0 || area || areaRevision != 0)) ||
        (attackArea && (!area || areaRevision == 0 || collection || collectionRevision != 0)))
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidOrderKind;
        return invalid;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return invalid;
    }
    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    const ObjectAIOrderAdmissionRequest request{
        .kind = ObjectAIOrderKind::TacticalAttack,
        .identity = identity,
        .tacticalAttackSubtype = subtype,
    };
    ObjectAIOrderAdmissionRequest active;
    const bool hasActive = admission.activeOrder(handle, active) == ObjectAIOrderAdmissionStatus::Success;
    if (hasActive && active == request)
    {
        invalid.handle = handle;
        invalid.owner = ObjectAIOrderOwner::ObjectAIRuntime;
        invalid.currentOrder = active;
        invalid.hasCurrentOrder = true;
        return invalid;
    }
    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    const ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (!actorStorage ||
        shadow.transitionRequests().size() >= shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
    {
        invalid.status = actorStorage ? ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded
                                      : ObjectAIOrderAdmissionStatus::InvalidSlot;
        return invalid;
    }
    ObjectAIOrderAdmissionResult result =
        hasActive ? admission.replace(handle, active.identity, request) : admission.admit(handle, request);
    if (!result.succeeded())
        return result;
    if (hasActive)
        static_cast<void>(clearTransientSubject(subject));

    AIStateParameters parameters;
    parameters.sourceOrderRevision = identity.queueRevision;
    parameters.tacticalTargetCollection = collection;
    parameters.tacticalTargetCollectionRevision = collectionRevision;
    parameters.tacticalAttackArea = area;
    parameters.tacticalAttackAreaRevision = areaRevision;
    parameters.tacticalSquadSelection = squadSelection;
    actorStorage->parameters()[actor->slot] = parameters;
    AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    if (m_shadowBatches[actor->batch].stageTransitionRequest({
            .slot = actor->slot,
            .subject = subject,
            .expectedState = runtime.currentState,
            .expectedRevision = runtime.revision,
            .operation = AIStateSoATransitionOperation::Direct,
            .target = target,
            .authority = AIStateTransitionAuthority::External,
            .reenter = runtime.currentState == target,
        }) != ObjectAIShadowBatchStatus::Success)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
    }
    return result;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeAttackSquadOrder(ObjectId subject,
                                                                      const ObjectAIOrderIdentity& identity,
                                                                      AITargetCollectionHandle collection,
                                                                      uint64_t collectionRevision,
                                                                      AISquadTargetSelection selection)
{
    return observeTacticalDomainOrder(subject,
                                      identity,
                                      ObjectAITacticalAttackSubtype::AttackSquad,
                                      collection,
                                      collectionRevision,
                                      {},
                                      0,
                                      selection);
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeAttackAreaOrder(ObjectId subject,
                                                                     const ObjectAIOrderIdentity& identity,
                                                                     AIAttackAreaHandle area,
                                                                     uint64_t areaRevision)
{
    return observeTacticalDomainOrder(subject,
                                      identity,
                                      ObjectAITacticalAttackSubtype::AttackArea,
                                      {},
                                      0,
                                      area,
                                      areaRevision,
                                      AISquadTargetSelection::NoTarget);
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeGuardOrder(ObjectId subject,
                                                                const ObjectAIOrderIdentity& identity,
                                                                const AIFixedPosition& anchor,
                                                                int64_t guardRangeRaw,
                                                                int64_t visionRangeRaw,
                                                                uint32_t pathSurfaceMask,
                                                                int64_t arrivalRadiusRaw,
                                                                bool enterGuardTargets,
                                                                bool tracksAnchor,
                                                                bool guardWithoutPursuit,
                                                                bool guardFlyingOnly,
                                                                AIAttackAreaHandle area,
                                                                uint64_t areaRevision)
{
    return observeGuardOrderVariant(subject,
                                    identity,
                                    anchor,
                                    guardRangeRaw,
                                    visionRangeRaw,
                                    pathSurfaceMask,
                                    arrivalRadiusRaw,
                                    enterGuardTargets,
                                    tracksAnchor,
                                    guardWithoutPursuit,
                                    guardFlyingOnly,
                                    area,
                                    areaRevision,
                                    false);
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeGuardTunnelNetworkOrder(ObjectId subject,
                                                                             const ObjectAIOrderIdentity& identity,
                                                                             const AIFixedPosition& anchor,
                                                                             int64_t guardRangeRaw,
                                                                             int64_t visionRangeRaw,
                                                                             uint32_t pathSurfaceMask,
                                                                             int64_t arrivalRadiusRaw)
{
    return observeGuardOrderVariant(subject,
                                    identity,
                                    anchor,
                                    guardRangeRaw,
                                    visionRangeRaw,
                                    pathSurfaceMask,
                                    arrivalRadiusRaw,
                                    false,
                                    false,
                                    false,
                                    false,
                                    {},
                                    0,
                                    true);
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeGuardOrderVariant(ObjectId subject,
                                                                       const ObjectAIOrderIdentity& identity,
                                                                       const AIFixedPosition& anchor,
                                                                       int64_t guardRangeRaw,
                                                                       int64_t visionRangeRaw,
                                                                       uint32_t pathSurfaceMask,
                                                                       int64_t arrivalRadiusRaw,
                                                                       bool enterGuardTargets,
                                                                       bool tracksAnchor,
                                                                       bool guardWithoutPursuit,
                                                                       bool guardFlyingOnly,
                                                                       AIAttackAreaHandle area,
                                                                       uint64_t areaRevision,
                                                                       bool tunnelNetwork)
{
    ObjectAIOrderAdmissionResult invalid;
    if (!acceptsProductionAdmission(tunnelNetwork ? AIStateId::GuardTunnelNetwork : AIStateId::Guard,
                                    AIProductionAdmission::TacticalOrder) ||
        ((tunnelNetwork && identity.source != ObjectAIOrderSource::Script) ||
         (!tunnelNetwork && identity.source != ObjectAIOrderSource::Script &&
          identity.source != ObjectAIOrderSource::Player)) ||
        identity.systemPurpose != ObjectAIOrderSystemPurpose::Generic || guardRangeRaw < 0 || visionRangeRaw < 0 ||
        pathSurfaceMask == 0 || arrivalRadiusRaw < 0 || static_cast<bool>(area) != (areaRevision != 0))
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidOrderKind;
        return invalid;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return invalid;
    }
    AIStateFamilySoAStorage* actorStorage = storage(*actor);

    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    const ObjectAIOrderAdmissionRequest request{
        .kind = ObjectAIOrderKind::TacticalAttack,
        .identity = identity,
        .tacticalAttackSubtype =
            tunnelNetwork ? ObjectAITacticalAttackSubtype::GuardTunnelNetwork : ObjectAITacticalAttackSubtype::Guard,
    };
    ObjectAIOrderAdmissionRequest active;
    const bool hasActive = admission.activeOrder(handle, active) == ObjectAIOrderAdmissionStatus::Success;
    if (hasActive && active == request)
    {
        if (!actorStorage)
        {
            invalid.status = ObjectAIOrderAdmissionStatus::InvalidSlot;
            return invalid;
        }
        // GuardObject retains one queue/admission identity while its
        // target moves. Refresh only value parameters; the running Guard
        // kernel samples currentAnchors from this storage every tick and
        // decides whether the moved anchor requires a Return phase.
        AIStateParameters& parameters = actorStorage->parameters()[actor->slot];
        parameters.guardAnchor = anchor;
        parameters.hasGuardAnchor = true;
        parameters.enterGuardTargets = enterGuardTargets;
        parameters.guardTracksAnchor = tracksAnchor;
        parameters.guardWithoutPursuit = guardWithoutPursuit;
        parameters.guardFlyingOnly = guardFlyingOnly;
        parameters.guardRangeRaw = guardRangeRaw;
        parameters.guardVisionRangeRaw = visionRangeRaw;
        parameters.pathSurfaceMask = pathSurfaceMask;
        parameters.arrivalRadiusRaw = arrivalRadiusRaw;
        parameters.tacticalAttackArea = area;
        parameters.tacticalAttackAreaRevision = areaRevision;
        invalid.handle = handle;
        invalid.owner = ObjectAIOrderOwner::ObjectAIRuntime;
        invalid.currentOrder = active;
        invalid.hasCurrentOrder = true;
        return invalid;
    }

    const ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (!actorStorage ||
        shadow.transitionRequests().size() >= shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
    {
        invalid.status = actorStorage ? ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded
                                      : ObjectAIOrderAdmissionStatus::InvalidSlot;
        return invalid;
    }

    ObjectAIOrderAdmissionResult result =
        hasActive ? admission.replace(handle, active.identity, request) : admission.admit(handle, request);
    if (!result.succeeded())
        return result;
    if (hasActive)
        static_cast<void>(clearTransientSubject(subject));

    AIStateParameters parameters;
    parameters.sourceOrderRevision = identity.queueRevision;
    parameters.guardRangeRaw = guardRangeRaw;
    parameters.guardVisionRangeRaw = visionRangeRaw;
    parameters.guardAnchor = anchor;
    parameters.hasGuardAnchor = true;
    parameters.enterGuardTargets = enterGuardTargets;
    parameters.guardTracksAnchor = tracksAnchor;
    parameters.guardWithoutPursuit = guardWithoutPursuit;
    parameters.guardFlyingOnly = guardFlyingOnly;
    parameters.pathSurfaceMask = pathSurfaceMask;
    parameters.arrivalRadiusRaw = arrivalRadiusRaw;
    parameters.adjustDestinations = true;
    parameters.tacticalAttackArea = area;
    parameters.tacticalAttackAreaRevision = areaRevision;
    actorStorage->parameters()[actor->slot] = parameters;
    AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    if (m_shadowBatches[actor->batch].stageTransitionRequest({
            .slot = actor->slot,
            .subject = subject,
            .expectedState = runtime.currentState,
            .expectedRevision = runtime.revision,
            .operation = AIStateSoATransitionOperation::Direct,
            .target = tunnelNetwork ? AIStateId::GuardTunnelNetwork : AIStateId::Guard,
            .authority = AIStateTransitionAuthority::External,
            .reenter = runtime.currentState == (tunnelNetwork ? AIStateId::GuardTunnelNetwork : AIStateId::Guard),
        }) != ObjectAIShadowBatchStatus::Success)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
    }
    return result;
}

ObjectAIOrderAdmissionResult ObjectAIRuntime::observeGuardRetaliateOrder(ObjectId subject,
                                                                         const ObjectAIOrderIdentity& identity,
                                                                         ObjectId aggressor,
                                                                         const AIFixedPosition& anchor,
                                                                         int64_t guardRangeRaw,
                                                                         int64_t visionRangeRaw,
                                                                         uint32_t pathSurfaceMask,
                                                                         int64_t arrivalRadiusRaw)
{
    ObjectAIOrderAdmissionResult invalid;
    if (!acceptsProductionAdmission(AIStateId::GuardRetaliate, AIProductionAdmission::TacticalOrder) ||
        identity.source != ObjectAIOrderSource::System ||
        identity.systemPurpose != ObjectAIOrderSystemPurpose::Retaliation || !aggressor || guardRangeRaw < 0 ||
        visionRangeRaw < 0 || pathSurfaceMask == 0 || arrivalRadiusRaw < 0)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidOrderKind;
        return invalid;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        invalid.status = ObjectAIOrderAdmissionStatus::InvalidSubject;
        return invalid;
    }
    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    const ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (!actorStorage ||
        shadow.transitionRequests().size() >= shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
    {
        invalid.status = actorStorage ? ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded
                                      : ObjectAIOrderAdmissionStatus::InvalidSlot;
        return invalid;
    }

    ObjectAIOrderAdmissionStorage& admission = m_orderAdmissions[actor->batch];
    const ObjectAIOrderSlotHandle handle = admission.handle(actor->slot);
    const ObjectAIOrderAdmissionRequest request{
        .kind = ObjectAIOrderKind::TacticalAttack,
        .identity = identity,
        .tacticalAttackSubtype = ObjectAITacticalAttackSubtype::GuardRetaliate,
    };
    ObjectAIOrderAdmissionRequest active;
    const bool hasActive = admission.activeOrder(handle, active) == ObjectAIOrderAdmissionStatus::Success;
    if (hasActive && active == request)
    {
        invalid.handle = handle;
        invalid.owner = ObjectAIOrderOwner::ObjectAIRuntime;
        invalid.currentOrder = active;
        invalid.hasCurrentOrder = true;
        return invalid;
    }
    ObjectAIOrderAdmissionResult result =
        hasActive ? admission.replace(handle, active.identity, request) : admission.admit(handle, request);
    if (!result.succeeded())
        return result;
    if (hasActive)
        static_cast<void>(clearTransientSubject(subject));

    AIStateParameters parameters;
    parameters.goalObject = aggressor;
    parameters.sourceOrderRevision = identity.queueRevision;
    parameters.guardRangeRaw = guardRangeRaw;
    parameters.guardVisionRangeRaw = visionRangeRaw;
    parameters.guardAnchor = anchor;
    parameters.hasGuardAnchor = true;
    parameters.pathSurfaceMask = pathSurfaceMask;
    parameters.arrivalRadiusRaw = arrivalRadiusRaw;
    parameters.adjustDestinations = true;
    parameters.guardRetaliateAggressor = aggressor;
    actorStorage->parameters()[actor->slot] = parameters;
    AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    if (m_shadowBatches[actor->batch].stageTransitionRequest({
            .slot = actor->slot,
            .subject = subject,
            .expectedState = runtime.currentState,
            .expectedRevision = runtime.revision,
            .operation = AIStateSoATransitionOperation::Direct,
            .target = AIStateId::GuardRetaliate,
            .authority = AIStateTransitionAuthority::External,
            .reenter = runtime.currentState == AIStateId::GuardRetaliate,
        }) != ObjectAIShadowBatchStatus::Success)
    {
        static_cast<void>(admission.cancel(handle, identity));
        result.status = ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded;
    }
    return result;
}

} // namespace engine::ai
