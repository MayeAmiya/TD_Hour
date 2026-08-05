#include "game/object/ai/runtime/ObjectAIRuntime.h"

namespace engine::ai
{

const ObjectAIShadowTickReport& ObjectAIRuntime::lastShadowReport() const noexcept
{
    return m_lastShadowReport;
}

const ObjectAIReadOnlyInputSnapshot& ObjectAIRuntime::latestInput() const noexcept
{
    return m_latestInput;
}

ObjectAIRuntimeSnapshotStatus ObjectAIRuntime::captureSnapshot(ObjectAIRuntimeSnapshot& output) const
{
    if (!m_initialized)
        return ObjectAIRuntimeSnapshotStatus::NotInitialized;

    ObjectAIRuntimeSnapshot candidate;
    candidate.config = m_config;
    candidate.batches.resize(m_batches.size());
    candidate.orderAdmissions.resize(m_orderAdmissions.size());
    for (size_t batch = 0; batch < m_batches.size(); ++batch)
    {
        if (m_batches[batch].captureSnapshot(candidate.batches[batch]) != AIStateSoASlotSnapshotStatus::Success)
        {
            return ObjectAIRuntimeSnapshotStatus::StorageRejected;
        }
        if (m_orderAdmissions[batch].captureSnapshot(candidate.orderAdmissions[batch]) !=
            ObjectAIOrderAdmissionStatus::Success)
        {
            return ObjectAIRuntimeSnapshotStatus::StorageRejected;
        }
    }
    if (m_recipeBindings.size() != m_subjects.size())
        return ObjectAIRuntimeSnapshotStatus::InvalidRecipeBinding;
    for (size_t index = 0; index < m_recipeBindings.size(); ++index)
    {
        const ObjectAIRecipeBindingSnapshot& binding =
            m_recipeBindings[index];
        const AIStateSoASubjectSlot& actor = m_subjects[index];
        ObjectAIOrderAdmissionSlotView slot;
        if (actor.handle.batch >= m_orderAdmissions.size() ||
            m_orderAdmissions[actor.handle.batch].readSlot(
                actor.handle.slot, slot) !=
                ObjectAIOrderAdmissionStatus::Success ||
            !slot.bound || slot.handle.subject != actor.subject ||
            slot.handle.generation != actor.handle.generation)
            return ObjectAIRuntimeSnapshotStatus::StorageRejected;
        if (binding.subject != m_subjects[index].subject ||
            binding.state == ObjectAIRecipeBindingState::Unbound ||
            (binding.state == ObjectAIRecipeBindingState::Bound &&
             (!aiRecipeOwnerRouteFor(binding.recipe) ||
              !isObjectAIRecipeCapabilitySubset(
                  binding.recipe, slot.capabilities))) ||
            (binding.state ==
                 ObjectAIRecipeBindingState::ContentUnavailable &&
             (binding.recipe != AIRecipeId::Invalid ||
              slot.capabilities != ObjectAIOrderCapability::None)))
            return ObjectAIRuntimeSnapshotStatus::InvalidRecipeBinding;
    }
    candidate.recipeBindings = m_recipeBindings;
    candidate.pendingMembershipEvents = m_membershipEvents;
    candidate.membershipJournalStatus = m_membershipJournalStatus;
    candidate.membershipJournalTick = m_membershipJournalTick;
    candidate.membershipJournalHasTick = m_membershipJournalHasTick;
    candidate.waypointTeamProgress = m_waypointTeamProgress;
    candidate.pendingTransitionRequests.resize(m_shadowBatches.size());
    for (size_t batch = 0; batch < m_shadowBatches.size(); ++batch)
    {
        const container::Span<const AIStateSoATransitionRequest> requests =
            m_shadowBatches[batch].transitionRequests();
        candidate.pendingTransitionRequests[batch].assign(
            requests.begin(), requests.end());
    }
    candidate.pendingInsertionEntryFeedback =
        m_pendingInsertionEntryFeedback;
    candidate.pendingContainmentEntryFeedback =
        m_pendingContainmentEntryFeedback;
    if (m_transients.captureSnapshot(candidate.transients) != ObjectAITransientStatus::Success)
    {
        return ObjectAIRuntimeSnapshotStatus::StorageRejected;
    }
    candidate.latestInput = m_latestInput;
    output = std::move(candidate);
    return ObjectAIRuntimeSnapshotStatus::Success;
}

ObjectAIRuntimeSnapshotStatus ObjectAIRuntime::restoreSnapshot(const ObjectAIRuntimeSnapshot& snapshot)
{
    if (snapshot.schemaVersion != ObjectAIRuntimeSnapshot::SchemaVersion)
        return ObjectAIRuntimeSnapshotStatus::InvalidSchema;

    ObjectAIRuntime candidate;
    if (candidate.initialize(snapshot.config) != AIStateSoASlotStatus::Success)
        return ObjectAIRuntimeSnapshotStatus::InvalidConfig;
    candidate.setPathSequenceResolver(m_pathSequenceResolver);
    candidate.setWaypointGraphResolver(m_waypointGraphResolver);
    candidate.setPathHandleReleaser(m_pathHandleReleaser);
    if (snapshot.batches.size() > candidate.m_maximumBatches)
        return ObjectAIRuntimeSnapshotStatus::InvalidBatch;
    if (snapshot.orderAdmissions.size() != snapshot.batches.size())
        return ObjectAIRuntimeSnapshotStatus::InvalidBatch;

    for (size_t batchIndex = 0; batchIndex < snapshot.batches.size(); ++batchIndex)
    {
        const AIStateSoASlotRegistrySnapshot& batchSnapshot = snapshot.batches[batchIndex];
        const size_t consumed = batchIndex * snapshot.config.slotsPerBatch;
        const size_t expectedCapacity =
            std::min(snapshot.config.slotsPerBatch, snapshot.config.maximumActors - consumed);
        if (batchSnapshot.batchIndex != batchIndex || batchSnapshot.slots.size() != expectedCapacity)
        {
            return ObjectAIRuntimeSnapshotStatus::InvalidBatch;
        }

        AIStateSoASlotRegistry batch;
        const AIStateSoASlotSnapshotStatus restored = batch.restoreSnapshot(batchSnapshot);
        if (restored != AIStateSoASlotSnapshotStatus::Success)
            return ObjectAIRuntimeSnapshotStatus::StorageRejected;
        ObjectAIShadowBatch shadow;
        if (shadow.initialize(expectedCapacity, candidate.shadowBatchConfig()) != ObjectAIShadowBatchStatus::Success)
        {
            return ObjectAIRuntimeSnapshotStatus::StorageRejected;
        }
        shadow.setPathSequenceResolver(candidate.m_pathSequenceResolver);
        shadow.setWaypointGraphResolver(candidate.m_waypointGraphResolver);
        ObjectAIOrderAdmissionStorage admission;
        if (!admission.restoreSnapshot(snapshot.orderAdmissions[batchIndex]).succeeded() ||
            admission.capacity() != expectedCapacity)
        {
            return ObjectAIRuntimeSnapshotStatus::StorageRejected;
        }
        candidate.m_batches.push_back(std::move(batch));
        candidate.m_shadowBatches.push_back(std::move(shadow));
        candidate.m_orderAdmissions.push_back(std::move(admission));
    }

    candidate.m_subjects.clear();
    for (const AIStateSoASlotRegistry& batch : candidate.m_batches)
    {
        for (const AIStateSoASubjectSlot& record : batch.orderedSubjects())
            candidate.m_subjects.push_back(record);
    }
    if (candidate.m_subjects.size() > snapshot.config.maximumActors)
        return ObjectAIRuntimeSnapshotStatus::CapacityExceeded;
    std::sort(candidate.m_subjects.begin(),
              candidate.m_subjects.end(),
              [](const AIStateSoASubjectSlot& left, const AIStateSoASubjectSlot& right)
              { return left.subject < right.subject; });
    for (size_t index = 1; index < candidate.m_subjects.size(); ++index)
    {
        if (candidate.m_subjects[index - 1].subject == candidate.m_subjects[index].subject)
        {
            return ObjectAIRuntimeSnapshotStatus::DuplicateSubject;
        }
    }
    if (snapshot.recipeBindings.size() != candidate.m_subjects.size())
        return ObjectAIRuntimeSnapshotStatus::InvalidRecipeBinding;
    candidate.m_recipeBindings = snapshot.recipeBindings;
    for (size_t actorIndex = 0;
         actorIndex < candidate.m_subjects.size(); ++actorIndex)
    {
        const AIStateSoASubjectSlot& actor =
            candidate.m_subjects[actorIndex];
        const ObjectAIRecipeBindingSnapshot& binding =
            candidate.m_recipeBindings[actorIndex];
        ObjectAIOrderAdmissionSlotView slot;
        if (candidate.m_orderAdmissions[actor.handle.batch].readSlot(actor.handle.slot, slot) !=
                ObjectAIOrderAdmissionStatus::Success ||
            !slot.bound || slot.handle.subject != actor.subject || slot.handle.generation != actor.handle.generation)
        {
            return ObjectAIRuntimeSnapshotStatus::StorageRejected;
        }
        if (binding.subject != actor.subject ||
            binding.state == ObjectAIRecipeBindingState::Unbound ||
            (binding.state == ObjectAIRecipeBindingState::Bound &&
             (!aiRecipeOwnerRouteFor(binding.recipe) ||
              !isObjectAIRecipeCapabilitySubset(binding.recipe,
                                                slot.capabilities))) ||
            (binding.state ==
                 ObjectAIRecipeBindingState::ContentUnavailable &&
             (binding.recipe != AIRecipeId::Invalid ||
              slot.capabilities != ObjectAIOrderCapability::None)))
            return ObjectAIRuntimeSnapshotStatus::InvalidRecipeBinding;
    }

    if (snapshot.pendingTransitionRequests.size() !=
            candidate.m_shadowBatches.size() ||
        snapshot.pendingInsertionEntryFeedback.size() >
            snapshot.config.maximumActors ||
        snapshot.pendingContainmentEntryFeedback.size() >
            snapshot.config.maximumActors)
        return ObjectAIRuntimeSnapshotStatus::InvalidPendingTransition;
    for (size_t batchIndex = 0;
         batchIndex < snapshot.pendingTransitionRequests.size();
         ++batchIndex)
    {
        ObjectAIShadowBatch& shadow = candidate.m_shadowBatches[batchIndex];
        for (const AIStateSoATransitionRequest& request :
             snapshot.pendingTransitionRequests[batchIndex])
        {
            const std::optional<AIActorHandle> actor =
                candidate.find(request.subject);
            AIStateFamilySoAStorage* actorStorage = actor
                ? candidate.storage(*actor) : nullptr;
            const bool validOperation = static_cast<uint8_t>(
                    request.operation) <= static_cast<uint8_t>(
                    AIStateSoATransitionOperation::BeginTemporary);
            const bool validAuthority = static_cast<uint8_t>(
                    request.authority) <= static_cast<uint8_t>(
                    AIStateTransitionAuthority::Terminal);
            const bool targetRequired =
                request.operation == AIStateSoATransitionOperation::Direct ||
                request.operation ==
                    AIStateSoATransitionOperation::BeginTemporary;
            const bool correlationTarget =
                request.operation == AIStateSoATransitionOperation::Direct &&
                (request.target == AIStateId::RappelInto ||
                 request.target == AIStateId::CombatDrop ||
                 request.target == AIStateId::Exit ||
                 request.target == AIStateId::ExitInstantly);
            if (!actor || actor->batch != batchIndex ||
                actor->slot != request.slot || !actorStorage ||
                request.slot >= actorStorage->runtimes().size() ||
                !isValidState(request.expectedState) || !validOperation ||
                !validAuthority || (targetRequired &&
                                     !isValidState(request.target)) ||
                (request.correlationIssuedTick != 0 &&
                 !correlationTarget))
                return ObjectAIRuntimeSnapshotStatus::InvalidPendingTransition;
            const AIStateMachineRuntime& runtime =
                actorStorage->runtimes()[request.slot];
            if (runtime.currentState != request.expectedState ||
                runtime.revision != request.expectedRevision ||
                shadow.stageTransitionRequest(request) !=
                    ObjectAIShadowBatchStatus::Success)
                return ObjectAIRuntimeSnapshotStatus::InvalidPendingTransition;
        }
    }
    const auto hasPendingTarget =
        [&candidate](ObjectId subject, AIStateId state,
                     uint64_t issuedTick) noexcept {
            const std::optional<AIActorHandle> actor =
                candidate.find(subject);
            if (!actor || actor->batch >= candidate.m_shadowBatches.size())
                return false;
            const container::Span<const AIStateSoATransitionRequest> requests =
                candidate.m_shadowBatches[actor->batch]
                    .transitionRequests();
            return std::any_of(
                requests.begin(), requests.end(),
                [subject, state, issuedTick](
                    const AIStateSoATransitionRequest& request) noexcept {
                    return request.subject == subject &&
                        request.operation ==
                            AIStateSoATransitionOperation::Direct &&
                        request.target == state &&
                        request.correlationIssuedTick == issuedTick;
                });
        };
    const auto expectedActivationSequence =
        [&candidate](ObjectId subject) noexcept
            -> std::optional<uint32_t> {
            const std::optional<AIActorHandle> actor =
                candidate.find(subject);
            const AIStateFamilySoAStorage* actorStorage = actor
                ? candidate.storage(*actor) : nullptr;
            if (!actorStorage ||
                actor->slot >= actorStorage->activationSequences().size())
                return std::nullopt;
            uint32_t sequence =
                actorStorage->activationSequences()[actor->slot] + 1u;
            if (sequence == 0) ++sequence;
            return sequence;
        };
    container::Vector<ObjectId> pendingFeedbackSubjects;
    pendingFeedbackSubjects.reserve(
        snapshot.pendingInsertionEntryFeedback.size() +
        snapshot.pendingContainmentEntryFeedback.size());
    for (size_t index = 0;
         index < snapshot.pendingInsertionEntryFeedback.size(); ++index)
    {
        const AIInsertionMotionFeedback& feedback =
            snapshot.pendingInsertionEntryFeedback[index];
        const std::optional<uint32_t> sequence =
            expectedActivationSequence(feedback.correlation.subject);
        if (!feedback.correlation.isValid() ||
            feedback.kind !=
                AIInsertionMotionFeedbackKind::RappelEntryReady ||
            !sequence ||
            feedback.correlation.stateRequest.sequence != *sequence ||
            !hasPendingTarget(feedback.correlation.subject,
                              feedback.correlation.state,
                              feedback.correlation.stateRequest.issuedTick))
            return ObjectAIRuntimeSnapshotStatus::InvalidPendingTransition;
        pendingFeedbackSubjects.push_back(feedback.correlation.subject);
    }
    for (size_t index = 0;
         index < snapshot.pendingContainmentEntryFeedback.size(); ++index)
    {
        const AIContainmentFeedback& feedback =
            snapshot.pendingContainmentEntryFeedback[index];
        const std::optional<uint32_t> sequence =
            expectedActivationSequence(feedback.correlation.subject);
        if (!feedback.correlation.isValid() ||
            feedback.kind != AIContainmentFeedbackKind::ExitEntryReady ||
            !sequence ||
            feedback.correlation.stateRequest.sequence != *sequence ||
            !hasPendingTarget(feedback.correlation.subject,
                              feedback.correlation.state,
                              feedback.correlation.stateRequest.issuedTick))
            return ObjectAIRuntimeSnapshotStatus::InvalidPendingTransition;
        pendingFeedbackSubjects.push_back(feedback.correlation.subject);
    }
    std::sort(pendingFeedbackSubjects.begin(),
              pendingFeedbackSubjects.end());
    if (std::adjacent_find(pendingFeedbackSubjects.begin(),
                           pendingFeedbackSubjects.end()) !=
            pendingFeedbackSubjects.end())
        return ObjectAIRuntimeSnapshotStatus::InvalidPendingTransition;
    candidate.m_pendingInsertionEntryFeedback =
        snapshot.pendingInsertionEntryFeedback;
    candidate.m_pendingContainmentEntryFeedback =
        snapshot.pendingContainmentEntryFeedback;

    if (snapshot.pendingMembershipEvents.size() > snapshot.config.membershipEventCapacity)
    {
        return ObjectAIRuntimeSnapshotStatus::CapacityExceeded;
    }
    const bool validJournalStatus =
        snapshot.membershipJournalStatus ==
            AIObjectMembershipStatus::Success ||
        snapshot.membershipJournalStatus ==
            AIObjectMembershipStatus::InvalidEvent ||
        snapshot.membershipJournalStatus ==
            AIObjectMembershipStatus::JournalCapacityExceeded;
    if (!validJournalStatus ||
        (!snapshot.membershipJournalHasTick &&
         snapshot.membershipJournalTick != 0) ||
        (!snapshot.pendingMembershipEvents.empty() &&
         !snapshot.membershipJournalHasTick) ||
        (snapshot.membershipJournalStatus ==
             AIObjectMembershipStatus::Success &&
         snapshot.pendingMembershipEvents.empty() &&
         snapshot.membershipJournalHasTick) ||
        snapshot.membershipJournalStatus ==
            AIObjectMembershipStatus::NotInitialized)
        return ObjectAIRuntimeSnapshotStatus::InvalidMembershipEvent;
    for (const AIObjectMembershipEvent& event : snapshot.pendingMembershipEvents)
    {
        if (!event.subject || event.sequence == 0 ||
            !isObjectAIRecipeInitialCapabilityMask(event.initialCapabilities) ||
            (event.operation == AIObjectMembershipOperation::Remove &&
             event.initialCapabilities != ObjectAIOrderCapability::None))
            return ObjectAIRuntimeSnapshotStatus::InvalidMembershipEvent;
        if (snapshot.membershipJournalHasTick &&
            event.confirmedTick != snapshot.membershipJournalTick)
            return ObjectAIRuntimeSnapshotStatus::InvalidMembershipEvent;
    }
    container::Vector<AIObjectMembershipEvent> membershipValidation =
        snapshot.pendingMembershipEvents;
    std::sort(
        membershipValidation.begin(), membershipValidation.end(),
        [](const AIObjectMembershipEvent& left,
           const AIObjectMembershipEvent& right) {
            if (left.subject != right.subject)
                return left.subject < right.subject;
            if (left.sequence != right.sequence)
                return left.sequence < right.sequence;
            if (left.operation != right.operation)
                return left.operation < right.operation;
            return static_cast<uint32_t>(left.initialCapabilities) <
                static_cast<uint32_t>(right.initialCapabilities);
        });
    for (size_t index = 1; index < membershipValidation.size(); ++index)
    {
        const AIObjectMembershipEvent& previous =
            membershipValidation[index - 1];
        const AIObjectMembershipEvent& current = membershipValidation[index];
        if (previous.subject == current.subject &&
            previous.sequence == current.sequence &&
            (previous.operation != current.operation ||
             previous.initialCapabilities != current.initialCapabilities))
            return ObjectAIRuntimeSnapshotStatus::InvalidMembershipEvent;
    }
    candidate.m_membershipEvents = snapshot.pendingMembershipEvents;
    candidate.m_effectiveMembershipEvents.clear();
    candidate.m_membershipJournalStatus = snapshot.membershipJournalStatus;
    candidate.m_membershipJournalTick = snapshot.membershipJournalTick;
    candidate.m_membershipJournalHasTick =
        snapshot.membershipJournalHasTick;
    for (size_t index = 0; index < snapshot.waypointTeamProgress.size(); ++index)
    {
        const ObjectAIWaypointTeamProgressState& progress = snapshot.waypointTeamProgress[index];
        if (!progress.isValid() ||
            (index != 0 && !(snapshot.waypointTeamProgress[index - 1].team.value < progress.team.value)))
        {
            return ObjectAIRuntimeSnapshotStatus::InvalidWaypointTeamProgress;
        }
    }
    candidate.m_waypointTeamProgress = snapshot.waypointTeamProgress;
    if (candidate.m_transients.restoreSnapshot(snapshot.transients) != ObjectAITransientStatus::Success)
    {
        return ObjectAIRuntimeSnapshotStatus::StorageRejected;
    }
    if (!candidate.validateInputSnapshot(snapshot.latestInput))
        return ObjectAIRuntimeSnapshotStatus::InvalidInputSnapshot;
    candidate.m_latestInput.confirmedTick = snapshot.latestInput.confirmedTick;
    candidate.m_latestInput.ticksPerSecond = snapshot.latestInput.ticksPerSecond;
    candidate.m_latestInput.facts.assign(snapshot.latestInput.facts.begin(), snapshot.latestInput.facts.end());
    candidate.m_latestInput.valid = snapshot.latestInput.valid;

    *this = std::move(candidate);
    return ObjectAIRuntimeSnapshotStatus::Success;
}

ObjectAITransientStore& ObjectAIRuntime::transients() noexcept
{
    return m_transients;
}

const ObjectAITransientStore& ObjectAIRuntime::transients() const noexcept
{
    return m_transients;
}

ObjectAITransientClearReport ObjectAIRuntime::clearSubjectTransients(ObjectId subject) noexcept
{
    return clearTransientSubject(subject);
}

} // namespace engine::ai
