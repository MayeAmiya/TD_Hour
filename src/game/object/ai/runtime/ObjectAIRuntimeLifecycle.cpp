#include "game/object/ai/runtime/ObjectAIRuntime.h"

namespace engine::ai
{

bool AIObjectMembershipCommitReport::succeeded() const noexcept
{
    return status == AIObjectMembershipStatus::Success;
}

bool ObjectAIInsertionTransitionResult::succeeded() const noexcept
{
    return status == ObjectAIInsertionTransitionStatus::Success;
}

bool ObjectAIContainmentTransitionResult::succeeded() const noexcept
{
    return status == ObjectAIContainmentTransitionStatus::Success;
}

bool ObjectAIFacingTransitionResult::succeeded() const noexcept
{
    return status == ObjectAIFacingTransitionStatus::Success;
}

AIStateSoASlotStatus ObjectAIRuntime::initialize(ObjectAIRuntimeConfig config)
{
    if (config.maximumActors == 0 || config.slotsPerBatch == 0 ||
        config.slotsPerBatch > static_cast<size_t>(AIActorHandle::InvalidIndex))
    {
        return AIStateSoASlotStatus::InvalidCapacity;
    }

    const size_t maximumBatches = 1 + ((config.maximumActors - 1) / config.slotsPerBatch);
    if (maximumBatches > static_cast<size_t>(AIActorHandle::InvalidIndex))
        return AIStateSoASlotStatus::InvalidCapacity;

    if (config.membershipEventCapacity == 0)
    {
        if (config.maximumActors > std::numeric_limits<size_t>::max() / 2)
            return AIStateSoASlotStatus::InvalidCapacity;
        config.membershipEventCapacity = config.maximumActors * 2;
    }
    if (config.transientValueCapacity == 0)
    {
        if (config.maximumActors > std::numeric_limits<size_t>::max() / 4)
            return AIStateSoASlotStatus::InvalidCapacity;
        config.transientValueCapacity = config.maximumActors * 4;
    }

    m_config = config;
    m_pathSequenceResolver = {};
    m_waypointGraphResolver = {};
    m_maximumBatches = maximumBatches;
    m_batches.clear();
    m_batches.reserve(maximumBatches);
    m_shadowBatches.clear();
    m_shadowBatches.reserve(maximumBatches);
    m_parallelBatchReports.clear();
    m_parallelBatchReports.resize(maximumBatches);
    m_runnableBatchIndices.clear();
    m_runnableBatchIndices.reserve(maximumBatches);
    m_orderAdmissions.clear();
    m_orderAdmissions.reserve(maximumBatches);
    m_subjects.clear();
    m_subjects.reserve(config.maximumActors);
    m_recipeBindings.clear();
    m_recipeBindings.reserve(config.maximumActors);
    m_membershipEvents.clear();
    m_membershipEvents.reserve(config.membershipEventCapacity);
    m_effectiveMembershipEvents.clear();
    m_effectiveMembershipEvents.reserve(config.maximumActors);
    m_membershipJournalStatus = AIObjectMembershipStatus::Success;
    m_membershipJournalTick = 0;
    m_membershipJournalHasTick = false;
    m_latestInput = {};
    m_latestInput.facts.reserve(config.maximumActors);
    m_wakeScratch.clear();
    m_wakeScratch.reserve(config.maximumActors);
    m_attackCompletionCandidates.clear();
    m_attackCompletionCandidates.reserve(config.maximumActors);
    m_waypointTeamProgress.clear();
    m_waypointTeamProgress.reserve(config.maximumActors);
    m_waypointTeamArrivals.clear();
    m_waypointTeamArrivals.reserve(config.maximumActors);
    m_pendingInsertionEntryFeedback.clear();
    m_pendingInsertionEntryFeedback.reserve(config.maximumActors);
    m_pendingContainmentEntryFeedback.clear();
    m_pendingContainmentEntryFeedback.reserve(config.maximumActors);
    m_lastShadowReport = {};
    if (m_transients.initialize(config.maximumActors,
                                config.transientValueCapacity) !=
            ObjectAITransientStatus::Success ||
        m_stagedTransients.initialize(config.maximumActors,
                                      config.transientValueCapacity) !=
            ObjectAITransientStatus::Success)
    {
        reset();
        return AIStateSoASlotStatus::InvalidCapacity;
    }
    m_initialized = true;
    return AIStateSoASlotStatus::Success;
}

AIObjectMembershipStatus ObjectAIRuntime::queueMembership(const AIObjectMembershipEvent& event)
{
    if (!m_initialized)
        return AIObjectMembershipStatus::NotInitialized;
    if (m_membershipJournalStatus != AIObjectMembershipStatus::Success)
        return m_membershipJournalStatus;
    if (!event.subject || event.sequence == 0 ||
        !isObjectAIRecipeInitialCapabilityMask(event.initialCapabilities) ||
        (event.operation == AIObjectMembershipOperation::Remove &&
         event.initialCapabilities != ObjectAIOrderCapability::None))
    {
        m_membershipJournalStatus = AIObjectMembershipStatus::InvalidEvent;
        return AIObjectMembershipStatus::InvalidEvent;
    }
    if (m_membershipJournalHasTick &&
        event.confirmedTick != m_membershipJournalTick)
    {
        m_membershipJournalStatus = AIObjectMembershipStatus::InvalidEvent;
        return m_membershipJournalStatus;
    }
    if (m_membershipEvents.size() == m_config.membershipEventCapacity)
    {
        m_membershipJournalStatus =
            AIObjectMembershipStatus::JournalCapacityExceeded;
        return AIObjectMembershipStatus::JournalCapacityExceeded;
    }
    if (!m_membershipJournalHasTick)
    {
        m_membershipJournalTick = event.confirmedTick;
        m_membershipJournalHasTick = true;
    }
    m_membershipEvents.push_back(event);
    return AIObjectMembershipStatus::Success;
}

AIObjectMembershipCommitReport ObjectAIRuntime::commitMembership(uint64_t confirmedTick)
{
    AIObjectMembershipCommitReport report;
    report.eventsRead = m_membershipEvents.size();
    const auto finish = [this, &report](AIObjectMembershipStatus status) {
        report.status = status;
        discardMembershipJournal();
        return report;
    };
    if (!m_initialized)
        return finish(AIObjectMembershipStatus::NotInitialized);
    if (m_membershipJournalStatus != AIObjectMembershipStatus::Success)
        return finish(m_membershipJournalStatus);
    if (m_membershipEvents.empty())
        return finish(AIObjectMembershipStatus::Success);
    if (!m_membershipJournalHasTick ||
        m_membershipJournalTick != confirmedTick)
        return finish(AIObjectMembershipStatus::InvalidEvent);

    std::sort(m_membershipEvents.begin(),
              m_membershipEvents.end(),
              [](const AIObjectMembershipEvent& left, const AIObjectMembershipEvent& right)
              {
                  if (left.subject != right.subject)
                      return left.subject < right.subject;
                  if (left.sequence != right.sequence)
                      return left.sequence < right.sequence;
                  if (left.operation != right.operation)
                      return left.operation < right.operation;
                  return static_cast<uint32_t>(left.initialCapabilities) <
                      static_cast<uint32_t>(right.initialCapabilities);
              });

    m_effectiveMembershipEvents.clear();
    size_t cursor = 0;
    while (cursor < m_membershipEvents.size())
    {
        const ObjectId subject = m_membershipEvents[cursor].subject;
        size_t end = cursor;
        while (end < m_membershipEvents.size() && m_membershipEvents[end].subject == subject)
        {
            const AIObjectMembershipEvent& event = m_membershipEvents[end];
            if (event.confirmedTick != confirmedTick)
                return finish(AIObjectMembershipStatus::InvalidEvent);
            if (end != cursor &&
                event.sequence == m_membershipEvents[end - 1].sequence &&
                (event.operation != m_membershipEvents[end - 1].operation ||
                 event.initialCapabilities !=
                     m_membershipEvents[end - 1].initialCapabilities))
                return finish(AIObjectMembershipStatus::ConflictingSequence);
            ++end;
        }
        m_effectiveMembershipEvents.push_back(m_membershipEvents[end - 1]);
        cursor = end;
    }

    report.effectiveEvents = m_effectiveMembershipEvents.size();
    report.eventsCoalesced = report.eventsRead - report.effectiveEvents;
    size_t desiredCount = m_subjects.size();
    for (const AIObjectMembershipEvent& event : m_effectiveMembershipEvents)
    {
        const bool exists = find(event.subject).has_value();
        if (event.operation == AIObjectMembershipOperation::Add && !exists)
            ++desiredCount;
        else if (event.operation == AIObjectMembershipOperation::Remove && exists)
            --desiredCount;
    }
    if (desiredCount > m_config.maximumActors)
        return finish(AIObjectMembershipStatus::ActorCapacityExceeded);

    // Prepare validates every affected existing slot before any production
    // membership mutation. With these invariants and sufficient prepared
    // capacity, the apply phase below is allocation-free and cannot fail for
    // an expected gameplay condition.
    if (m_batches.size() != m_shadowBatches.size() ||
        m_batches.size() != m_orderAdmissions.size() ||
        m_recipeBindings.size() != m_subjects.size())
        return finish(AIObjectMembershipStatus::StorageRejected);
    for (const AIObjectMembershipEvent& event : m_effectiveMembershipEvents)
    {
        const std::optional<AIActorHandle> actor = find(event.subject);
        if (!actor)
            continue;
        const ConstSubjectIterator bindingPosition =
            static_cast<const ObjectAIRuntime&>(*this).lowerBound(
                event.subject);
        const size_t subjectIndex = static_cast<size_t>(
            std::distance(m_subjects.cbegin(), bindingPosition));
        if (subjectIndex >= m_recipeBindings.size() ||
            m_recipeBindings[subjectIndex].subject != event.subject)
            return finish(AIObjectMembershipStatus::StorageRejected);
        if (actor->batch >= m_batches.size() ||
            m_batches[actor->batch].resolve(*actor) != event.subject ||
            actor->slot >= m_orderAdmissions[actor->batch].capacity())
            return finish(AIObjectMembershipStatus::StorageRejected);
        ObjectAIOrderAdmissionSlotView slot;
        if (m_orderAdmissions[actor->batch].readSlot(actor->slot, slot) !=
                ObjectAIOrderAdmissionStatus::Success ||
            !slot.bound || slot.handle.subject != event.subject ||
            slot.handle.generation != actor->generation)
            return finish(AIObjectMembershipStatus::StorageRejected);
    }

    size_t preparedCapacity = 0;
    for (const AIStateSoASlotRegistry& batch : m_batches)
        preparedCapacity += batch.capacity();
    const size_t requiredNewBatches = desiredCount > preparedCapacity
        ? 1 + ((desiredCount - preparedCapacity - 1) /
               m_config.slotsPerBatch)
        : 0;
    if (m_batches.size() + requiredNewBatches > m_maximumBatches)
        return finish(AIObjectMembershipStatus::ActorCapacityExceeded);

    container::Vector<AIStateSoASlotRegistry> preparedBatches;
    container::Vector<ObjectAIShadowBatch> preparedShadows;
    container::Vector<ObjectAIOrderAdmissionStorage> preparedAdmissions;
    preparedBatches.reserve(requiredNewBatches);
    preparedShadows.reserve(requiredNewBatches);
    preparedAdmissions.reserve(requiredNewBatches);
    for (size_t offset = 0; offset < requiredNewBatches; ++offset)
    {
        const size_t batchIndex = m_batches.size() + offset;
        const size_t consumed = batchIndex * m_config.slotsPerBatch;
        const size_t batchCapacity = std::min(
            m_config.slotsPerBatch, m_config.maximumActors - consumed);
        AIStateSoASlotRegistry batch;
        ObjectAIShadowBatch shadow;
        ObjectAIOrderAdmissionStorage admission;
        if (batch.initialize(batchCapacity, static_cast<uint32_t>(batchIndex)) !=
                AIStateSoASlotStatus::Success ||
            shadow.initialize(batchCapacity, shadowBatchConfig()) !=
                ObjectAIShadowBatchStatus::Success ||
            admission.initialize(batchCapacity) !=
                ObjectAIOrderAdmissionStatus::Success)
            return finish(AIObjectMembershipStatus::StorageRejected);
        shadow.setPathSequenceResolver(m_pathSequenceResolver);
        shadow.setWaypointGraphResolver(m_waypointGraphResolver);
        preparedBatches.push_back(std::move(batch));
        preparedShadows.push_back(std::move(shadow));
        preparedAdmissions.push_back(std::move(admission));
    }

    for (size_t index = 0; index < preparedBatches.size(); ++index)
    {
        m_batches.push_back(std::move(preparedBatches[index]));
        m_shadowBatches.push_back(std::move(preparedShadows[index]));
        m_orderAdmissions.push_back(std::move(preparedAdmissions[index]));
    }

    for (const AIObjectMembershipEvent& event : m_effectiveMembershipEvents)
    {
        if (event.operation != AIObjectMembershipOperation::Remove)
            continue;
        if (!find(event.subject))
        {
            ++report.unchanged;
            continue;
        }
        if (removeSubject(event.subject) != AIStateSoASlotStatus::Success)
            return finish(AIObjectMembershipStatus::StorageRejected);
        ++report.subjectsRemoved;
    }
    for (const AIObjectMembershipEvent& event : m_effectiveMembershipEvents)
    {
        if (event.operation != AIObjectMembershipOperation::Add)
            continue;
        if (find(event.subject))
        {
            ++report.unchanged;
            continue;
        }
        AIActorHandle handle;
        if (addSubject(event.subject, handle) != AIStateSoASlotStatus::Success)
            return finish(AIObjectMembershipStatus::StorageRejected);
        if (!setOrderCapabilities(event.subject, event.initialCapabilities)
                 .succeeded())
            return finish(AIObjectMembershipStatus::StorageRejected);
        AIStateFamilySoAStorage* actorStorage = storage(handle);
        if (!actorStorage ||
            !AIStateMachine::initialize(actorStorage->runtimes()[handle.slot], AIStateId::Idle, confirmedTick))
            return finish(AIObjectMembershipStatus::StorageRejected);
        actorStorage->activate(handle.slot, AIStateId::Idle, confirmedTick);
        ++report.subjectsAdded;
    }

    return finish(AIObjectMembershipStatus::Success);
}

void ObjectAIRuntime::discardMembershipJournal() noexcept
{
    m_membershipEvents.clear();
    m_effectiveMembershipEvents.clear();
    m_membershipJournalStatus = AIObjectMembershipStatus::Success;
    m_membershipJournalTick = 0;
    m_membershipJournalHasTick = false;
}

size_t ObjectAIRuntime::pendingMembershipEventCount() const noexcept
{
    return m_membershipEvents.size();
}

void ObjectAIRuntime::reset() noexcept
{
    m_config = {};
    m_maximumBatches = 0;
    m_batches.clear();
    m_shadowBatches.clear();
    m_parallelBatchReports.clear();
    m_runnableBatchIndices.clear();
    m_orderAdmissions.clear();
    m_subjects.clear();
    m_recipeBindings.clear();
    m_membershipEvents.clear();
    m_effectiveMembershipEvents.clear();
    m_membershipJournalStatus = AIObjectMembershipStatus::Success;
    m_membershipJournalTick = 0;
    m_membershipJournalHasTick = false;
    m_latestInput = {};
    m_wakeScratch.clear();
    m_attackCompletionCandidates.clear();
    m_waypointTeamProgress.clear();
    m_waypointTeamArrivals.clear();
    m_pendingInsertionEntryFeedback.clear();
    m_pendingContainmentEntryFeedback.clear();
    m_lastShadowReport = {};
    m_transients.reset();
    m_stagedTransients.reset();
    m_pathSequenceResolver = {};
    m_waypointGraphResolver = {};
    m_pathHandleReleaser = {};
    m_initialized = false;
}

AIStateSoASlotStatus ObjectAIRuntime::addSubject(ObjectId subject, AIActorHandle& output)
{
    output = {};
    if (!m_initialized)
        return AIStateSoASlotStatus::NotInitialized;
    if (!subject)
        return AIStateSoASlotStatus::InvalidSubject;

    const auto position = lowerBound(subject);
    if (position != m_subjects.end() && position->subject == subject)
        return AIStateSoASlotStatus::DuplicateSubject;
    if (m_subjects.size() == m_config.maximumActors)
        return AIStateSoASlotStatus::CapacityExceeded;

    size_t batchIndex = 0;
    while (batchIndex < m_batches.size() && m_batches[batchIndex].freeCount() == 0)
        ++batchIndex;
    if (batchIndex == m_batches.size())
    {
        if (m_batches.size() == m_maximumBatches)
            return AIStateSoASlotStatus::CapacityExceeded;
        const size_t remaining = m_config.maximumActors - (m_batches.size() * m_config.slotsPerBatch);
        const size_t batchCapacity = std::min(m_config.slotsPerBatch, remaining);
        AIStateSoASlotRegistry batch;
        const AIStateSoASlotStatus initialized = batch.initialize(batchCapacity, static_cast<uint32_t>(batchIndex));
        if (initialized != AIStateSoASlotStatus::Success)
            return initialized;
        ObjectAIShadowBatch shadow;
        if (shadow.initialize(batchCapacity, shadowBatchConfig()) != ObjectAIShadowBatchStatus::Success)
        {
            return AIStateSoASlotStatus::StorageRejected;
        }
        shadow.setPathSequenceResolver(m_pathSequenceResolver);
        shadow.setWaypointGraphResolver(m_waypointGraphResolver);
        ObjectAIOrderAdmissionStorage admission;
        if (admission.initialize(batchCapacity) != ObjectAIOrderAdmissionStatus::Success)
        {
            return AIStateSoASlotStatus::StorageRejected;
        }
        m_batches.push_back(std::move(batch));
        m_shadowBatches.push_back(std::move(shadow));
        m_orderAdmissions.push_back(std::move(admission));
    }

    AIActorHandle handle;
    const AIStateSoASlotStatus added = m_batches[batchIndex].addSubject(subject, handle);
    if (added != AIStateSoASlotStatus::Success)
        return added;
    const ObjectAIOrderAdmissionResult admission = m_orderAdmissions[batchIndex].bind(handle.slot, subject);
    if (!admission.succeeded())
    {
        static_cast<void>(m_batches[batchIndex].removeSubject(subject));
        return AIStateSoASlotStatus::StorageRejected;
    }

    const size_t subjectIndex = static_cast<size_t>(
        std::distance(m_subjects.begin(), position));
    m_subjects.insert(position, {subject, handle});
    m_recipeBindings.insert(
        m_recipeBindings.begin() + static_cast<std::ptrdiff_t>(subjectIndex),
        {.subject = subject,
         .recipe = AIRecipeId::Invalid,
         .state = ObjectAIRecipeBindingState::Unbound});
    output = handle;
    return AIStateSoASlotStatus::Success;
}

AIStateSoASlotStatus ObjectAIRuntime::removeSubject(ObjectId subject) noexcept
{
    if (!m_initialized)
        return AIStateSoASlotStatus::NotInitialized;
    if (!subject)
        return AIStateSoASlotStatus::InvalidSubject;

    const auto position = lowerBound(subject);
    if (position == m_subjects.end() || position->subject != subject)
        return AIStateSoASlotStatus::SubjectNotFound;
    const size_t subjectIndex = static_cast<size_t>(
        std::distance(m_subjects.begin(), position));
    if (m_recipeBindings.size() != m_subjects.size() ||
        subjectIndex >= m_recipeBindings.size() ||
        m_recipeBindings[subjectIndex].subject != subject)
        return AIStateSoASlotStatus::StorageRejected;
    const AIActorHandle handle = position->handle;
    if (handle.batch >= m_batches.size())
        return AIStateSoASlotStatus::StorageRejected;

    const ObjectAIOrderSlotHandle orderHandle = m_orderAdmissions[handle.batch].handle(handle.slot);
    const AIStateSoASlotStatus removed = m_batches[handle.batch].removeSubject(subject);
    if (removed != AIStateSoASlotStatus::Success)
        return removed;
    if (!m_orderAdmissions[handle.batch].release(orderHandle).succeeded())
        return AIStateSoASlotStatus::StorageRejected;
    static_cast<void>(clearTransientSubject(subject));
    m_subjects.erase(position);
    m_recipeBindings.erase(
        m_recipeBindings.begin() + static_cast<std::ptrdiff_t>(subjectIndex));
    return AIStateSoASlotStatus::Success;
}

std::optional<AIActorHandle> ObjectAIRuntime::find(ObjectId subject) const noexcept
{
    if (!m_initialized || !subject)
        return std::nullopt;
    const auto position = lowerBound(subject);
    return position != m_subjects.end() && position->subject == subject ? std::optional<AIActorHandle>{position->handle}
                                                                        : std::nullopt;
}

std::optional<ObjectAIActorStateView> ObjectAIRuntime::actorState(ObjectId subject) const noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
        return std::nullopt;
    const AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage || actor->slot >= actorStorage->runtimes().size())
        return std::nullopt;
    const AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    return ObjectAIActorStateView{
        .state = runtime.currentState,
        .revision = runtime.revision,
        .wakeTick = runtime.wakeTick,
        .idle = runtime.currentState == AIStateId::Idle,
    };
}

ObjectAIRecipeBindingResult ObjectAIRuntime::bindRecipe(
    ObjectId subject, AIRecipeId recipe) noexcept
{
    ObjectAIRecipeBindingResult result;
    if (!m_initialized)
        return result;
    const AIRecipeOwnerRoute* route = aiRecipeOwnerRouteFor(recipe);
    if (!subject)
    {
        result.status = ObjectAIRecipeBindingStatus::InvalidSubject;
        return result;
    }
    if (!route)
    {
        result.status = ObjectAIRecipeBindingStatus::InvalidRecipe;
        return result;
    }
    const ConstSubjectIterator subjectPosition = lowerBound(subject);
    if (subjectPosition == m_subjects.cend() ||
        subjectPosition->subject != subject)
    {
        result.status = ObjectAIRecipeBindingStatus::InvalidSubject;
        return result;
    }
    const size_t index = static_cast<size_t>(
        std::distance(m_subjects.cbegin(), subjectPosition));
    if (m_recipeBindings.size() != m_subjects.size() ||
        index >= m_recipeBindings.size() ||
        m_recipeBindings[index].subject != subject)
    {
        result.status = ObjectAIRecipeBindingStatus::StorageRejected;
        return result;
    }
    ObjectAIRecipeBindingSnapshot& binding = m_recipeBindings[index];
    if (binding.state != ObjectAIRecipeBindingState::Unbound)
    {
        if (binding.state != ObjectAIRecipeBindingState::Bound ||
            binding.recipe != recipe)
        {
            result.status = ObjectAIRecipeBindingStatus::RecipeConflict;
            return result;
        }
        result.status = ObjectAIRecipeBindingStatus::Success;
        return result;
    }

    const std::optional<AIActorHandle> actor = find(subject);
    ObjectAIOrderAdmissionSlotView slot;
    if (!actor || actor->batch >= m_orderAdmissions.size() ||
        m_orderAdmissions[actor->batch].readSlot(actor->slot, slot) !=
            ObjectAIOrderAdmissionStatus::Success ||
        !slot.bound || slot.handle.subject != subject)
    {
        result.status = ObjectAIRecipeBindingStatus::StorageRejected;
        return result;
    }
    if (!isObjectAIRecipeCapabilitySubset(recipe, slot.capabilities))
    {
        result.status = ObjectAIRecipeBindingStatus::CapabilityConflict;
        return result;
    }
    binding.recipe = recipe;
    binding.state = ObjectAIRecipeBindingState::Bound;
    result.status = ObjectAIRecipeBindingStatus::Success;
    result.changed = true;
    return result;
}

ObjectAIRecipeBindingResult ObjectAIRuntime::markRecipeContentUnavailable(
    ObjectId subject) noexcept
{
    ObjectAIRecipeBindingResult result;
    if (!m_initialized)
        return result;
    const ConstSubjectIterator subjectPosition = lowerBound(subject);
    if (!subject || subjectPosition == m_subjects.cend() ||
        subjectPosition->subject != subject)
    {
        result.status = ObjectAIRecipeBindingStatus::InvalidSubject;
        return result;
    }
    const size_t index = static_cast<size_t>(
        std::distance(m_subjects.cbegin(), subjectPosition));
    if (m_recipeBindings.size() != m_subjects.size() ||
        index >= m_recipeBindings.size() ||
        m_recipeBindings[index].subject != subject)
    {
        result.status = ObjectAIRecipeBindingStatus::StorageRejected;
        return result;
    }
    ObjectAIRecipeBindingSnapshot& binding = m_recipeBindings[index];
    if (binding.state == ObjectAIRecipeBindingState::Bound)
    {
        result.status = ObjectAIRecipeBindingStatus::RecipeConflict;
        return result;
    }
    if (binding.state == ObjectAIRecipeBindingState::ContentUnavailable)
    {
        result.status = ObjectAIRecipeBindingStatus::ContentUnavailable;
        return result;
    }
    const ObjectAIOrderAdmissionResult disabled =
        setOrderCapabilities(subject, ObjectAIOrderCapability::None);
    if (!disabled.succeeded())
    {
        result.status = ObjectAIRecipeBindingStatus::CapabilityConflict;
        return result;
    }
    binding.recipe = AIRecipeId::Invalid;
    binding.state = ObjectAIRecipeBindingState::ContentUnavailable;
    result.status = ObjectAIRecipeBindingStatus::ContentUnavailable;
    result.changed = true;
    return result;
}

std::optional<ObjectAIRecipeActorView> ObjectAIRuntime::recipeBinding(
    ObjectId subject) const noexcept
{
    const ConstSubjectIterator position = lowerBound(subject);
    if (!m_initialized || !subject || position == m_subjects.cend() ||
        position->subject != subject ||
        m_recipeBindings.size() != m_subjects.size())
        return std::nullopt;
    const size_t index = static_cast<size_t>(
        std::distance(m_subjects.cbegin(), position));
    if (index >= m_recipeBindings.size() ||
        m_recipeBindings[index].subject != subject)
        return std::nullopt;
    const ObjectAIRecipeBindingSnapshot& binding = m_recipeBindings[index];
    ObjectAIOrderAdmissionSlotView slot;
    const AIActorHandle actor = position->handle;
    if (actor.batch >= m_orderAdmissions.size() ||
        m_orderAdmissions[actor.batch].readSlot(actor.slot, slot) !=
            ObjectAIOrderAdmissionStatus::Success)
        return std::nullopt;
    const AIRecipeOwnerRoute* route = aiRecipeOwnerRouteFor(binding.recipe);
    return ObjectAIRecipeActorView{
        .recipe = binding.recipe,
        .owner = route ? route->owner : AIRecipeOwner::Unimplemented,
        .ownerClass = route ? route->ownerClass
                            : AIRecipeOwnerClass::Unsupported,
        .capabilities = slot.capabilities,
        .state = binding.state,
    };
}

container::Span<const ObjectAIRecipeBindingSnapshot>
ObjectAIRuntime::recipeBindings() const noexcept
{
    return m_recipeBindings;
}

std::optional<ObjectAIInsertionStateView> ObjectAIRuntime::insertionState(ObjectId subject) const noexcept
{
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
        return std::nullopt;
    const AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage || actor->slot >= actorStorage->runtimes().size())
        return std::nullopt;
    const AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    if (runtime.currentState != AIStateId::RappelInto && runtime.currentState != AIStateId::CombatDrop)
        return std::nullopt;
    return ObjectAIInsertionStateView{
        .state = runtime.currentState,
        .payload = actorStorage->insertion().load(actor->slot),
        .parameters = actorStorage->parameters()[actor->slot],
    };
}

ObjectAIFacingTransitionResult ObjectAIRuntime::stageFacingState(
    ObjectId subject, ObjectId targetObject,
    const std::optional<AIFixedPosition>& targetPosition,
    uint64_t activationTick, uint64_t externalOrderRevision)
{
    ObjectAIFacingTransitionResult result;
    if (!m_initialized)
        return result;
    if (!subject || static_cast<bool>(targetObject) ==
            targetPosition.has_value()) {
        result.status = ObjectAIFacingTransitionStatus::InvalidTarget;
        return result;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor) {
        result.status = ObjectAIFacingTransitionStatus::InvalidSubject;
        return result;
    }
    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage || actor->slot >= actorStorage->runtimes().size() ||
        actor->batch >= m_shadowBatches.size() ||
        actor->batch >= m_orderAdmissions.size()) {
        result.status = ObjectAIFacingTransitionStatus::InvalidSubject;
        return result;
    }
    ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (std::any_of(
            shadow.transitionRequests().begin(),
            shadow.transitionRequests().end(),
            [subject](const AIStateSoATransitionRequest& request) {
                return request.subject == subject;
            })) {
        result.status = ObjectAIFacingTransitionStatus::TransitionConflict;
        return result;
    }
    if (shadow.transitionRequests().size() >=
        shadow.capacity() *
            ObjectAIShadowBatch::TransitionRequestsPerSlot) {
        result.status = ObjectAIFacingTransitionStatus::CapacityExceeded;
        return result;
    }

    if (externalOrderRevision != 0) {
        ObjectAIOrderAdmissionStorage& admission =
            m_orderAdmissions[actor->batch];
        const ObjectAIOrderAdmissionResult synchronized =
            admission.synchronizeExternalRevision(
                admission.handle(actor->slot), externalOrderRevision);
        if (!synchronized.succeeded()) {
            result.status =
                ObjectAIFacingTransitionStatus::OrderCancellationFailed;
            return result;
        }
        static_cast<void>(clearTransientSubject(subject));
    }

    uint32_t sequence = actorStorage->activationSequences()[actor->slot] + 1;
    if (sequence == 0)
        ++sequence;
    result.request = {activationTick, sequence};
    const AIStateMachineRuntime& runtime =
        actorStorage->runtimes()[actor->slot];
    const AIStateId state = targetObject
        ? AIStateId::FaceObject : AIStateId::FacePosition;
    if (shadow.stageTransitionRequest({
            .slot = actor->slot,
            .subject = subject,
            .expectedState = runtime.currentState,
            .expectedRevision = runtime.revision,
            .operation = AIStateSoATransitionOperation::Direct,
            .target = state,
            .correlationIssuedTick = activationTick,
            .authority = AIStateTransitionAuthority::External,
            .reenter = runtime.currentState == state,
        }) != ObjectAIShadowBatchStatus::Success) {
        result.status = ObjectAIFacingTransitionStatus::CapacityExceeded;
        return result;
    }

    AIStateParameters parameters{};
    parameters.goalObject = targetObject;
    if (targetPosition) {
        parameters.goalPosition = *targetPosition;
        parameters.hasGoalPosition = true;
    }
    parameters.sourceOrderRevision = externalOrderRevision;
    actorStorage->parameters()[actor->slot] = parameters;
    result.status = ObjectAIFacingTransitionStatus::Success;
    return result;
}

ObjectAIInsertionTransitionResult ObjectAIRuntime::stageInsertionState(
    ObjectId subject,
    AIStateId state,
    const AIStateParameters& parameters,
    uint64_t activationTick,
    std::optional<AIInsertionMotionFeedback> entryFeedback)
{
    ObjectAIInsertionTransitionResult result;
    if (!m_initialized)
    {
        result.status = ObjectAIInsertionTransitionStatus::NotInitialized;
        return result;
    }
    if (state != AIStateId::RappelInto && state != AIStateId::CombatDrop)
    {
        result.status = ObjectAIInsertionTransitionStatus::InvalidState;
        return result;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        result.status = ObjectAIInsertionTransitionStatus::InvalidSubject;
        return result;
    }
    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage || actor->slot >= actorStorage->runtimes().size() || actor->batch >= m_shadowBatches.size())
    {
        result.status = ObjectAIInsertionTransitionStatus::InvalidSubject;
        return result;
    }
    ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (std::any_of(shadow.transitionRequests().begin(),
                    shadow.transitionRequests().end(),
                    [subject](const AIStateSoATransitionRequest& request) { return request.subject == subject; }))
    {
        result.status = ObjectAIInsertionTransitionStatus::TransitionConflict;
        return result;
    }
    if (entryFeedback && m_pendingInsertionEntryFeedback.size() >= m_config.maximumActors)
    {
        result.status = ObjectAIInsertionTransitionStatus::CapacityExceeded;
        return result;
    }

    uint32_t sequence = actorStorage->activationSequences()[actor->slot];
    ++sequence;
    if (sequence == 0)
        ++sequence;
    result.correlation = {
        .subject = subject,
        .stateRequest = {activationTick, sequence},
        .state = state,
    };
    if (entryFeedback)
    {
        entryFeedback->correlation = result.correlation;
    }

    const AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    if (shadow.stageTransitionRequest({
            .slot = actor->slot,
            .subject = subject,
            .expectedState = runtime.currentState,
            .expectedRevision = runtime.revision,
            .operation = AIStateSoATransitionOperation::Direct,
            .target = state,
            .correlationIssuedTick = activationTick,
            .authority = AIStateTransitionAuthority::External,
            .reenter = runtime.currentState == state,
        }) != ObjectAIShadowBatchStatus::Success)
    {
        result.status = ObjectAIInsertionTransitionStatus::CapacityExceeded;
        return result;
    }

    actorStorage->parameters()[actor->slot] = parameters;
    if (entryFeedback)
        m_pendingInsertionEntryFeedback.push_back(*entryFeedback);
    result.status = ObjectAIInsertionTransitionStatus::Success;
    return result;
}

ObjectAIInsertionTransitionStatus ObjectAIRuntime::cancelInsertionState(ObjectId subject, uint64_t activationTick)
{
    if (!m_initialized)
        return ObjectAIInsertionTransitionStatus::NotInitialized;
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
        return ObjectAIInsertionTransitionStatus::InvalidSubject;
    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage || actor->batch >= m_shadowBatches.size())
        return ObjectAIInsertionTransitionStatus::InvalidSubject;
    const AIStateMachineRuntime& runtime = actorStorage->runtimes()[actor->slot];
    if (runtime.currentState != AIStateId::RappelInto && runtime.currentState != AIStateId::CombatDrop)
        return ObjectAIInsertionTransitionStatus::InvalidState;
    ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (std::any_of(shadow.transitionRequests().begin(),
                    shadow.transitionRequests().end(),
                    [subject](const AIStateSoATransitionRequest& request) { return request.subject == subject; }))
        return ObjectAIInsertionTransitionStatus::TransitionConflict;
    if (shadow.stageTransitionRequest({
            .slot = actor->slot,
            .subject = subject,
            .expectedState = runtime.currentState,
            .expectedRevision = runtime.revision,
            .operation = AIStateSoATransitionOperation::Direct,
            .target = AIStateId::Idle,
            .authority = AIStateTransitionAuthority::External,
        }) != ObjectAIShadowBatchStatus::Success)
        return ObjectAIInsertionTransitionStatus::CapacityExceeded;
    static_cast<void>(activationTick);
    return ObjectAIInsertionTransitionStatus::Success;
}

ObjectAIContainmentTransitionResult ObjectAIRuntime::stageContainmentExitState(
    ObjectId subject, ObjectId container, AIStateId state,
    uint64_t activationTick, uint64_t externalOrderRevision,
    AIContainmentFeedback entryFeedback)
{
    ObjectAIContainmentTransitionResult result;
    if (!m_initialized)
        return result;
    if (state != AIStateId::Exit && state != AIStateId::ExitInstantly)
    {
        result.status = ObjectAIContainmentTransitionStatus::InvalidState;
        return result;
    }
    if (!container)
    {
        result.status = ObjectAIContainmentTransitionStatus::InvalidGoal;
        return result;
    }
    const std::optional<AIActorHandle> actor = find(subject);
    if (!actor)
    {
        result.status = ObjectAIContainmentTransitionStatus::InvalidSubject;
        return result;
    }
    AIStateFamilySoAStorage* actorStorage = storage(*actor);
    if (!actorStorage || actor->slot >= actorStorage->runtimes().size() ||
        actor->batch >= m_shadowBatches.size() ||
        actor->batch >= m_orderAdmissions.size())
    {
        result.status = ObjectAIContainmentTransitionStatus::InvalidSubject;
        return result;
    }
    ObjectAIShadowBatch& shadow = m_shadowBatches[actor->batch];
    if (std::any_of(shadow.transitionRequests().begin(),
                    shadow.transitionRequests().end(),
                    [subject](const AIStateSoATransitionRequest& request) {
                        return request.subject == subject;
                    }))
    {
        result.status = ObjectAIContainmentTransitionStatus::TransitionConflict;
        return result;
    }
    if (shadow.transitionRequests().size() >=
        shadow.capacity() * ObjectAIShadowBatch::TransitionRequestsPerSlot)
    {
        result.status = ObjectAIContainmentTransitionStatus::CapacityExceeded;
        return result;
    }
    if (m_pendingContainmentEntryFeedback.size() >= m_config.maximumActors)
    {
        result.status = ObjectAIContainmentTransitionStatus::CapacityExceeded;
        return result;
    }

    uint32_t sequence = actorStorage->activationSequences()[actor->slot] + 1;
    if (sequence == 0)
        ++sequence;
    result.correlation = {
        .subject = subject,
        .stateRequest = {activationTick, sequence},
        .state = state,
    };
    entryFeedback.correlation = result.correlation;
    entryFeedback.goal = container;
    if (entryFeedback.kind != AIContainmentFeedbackKind::ExitEntryReady)
    {
        result.status = ObjectAIContainmentTransitionStatus::InvalidGoal;
        return result;
    }

    if (externalOrderRevision != 0)
    {
        ObjectAIOrderAdmissionStorage& admission =
            m_orderAdmissions[actor->batch];
        const ObjectAIOrderAdmissionResult synchronized =
            admission.synchronizeExternalRevision(
                admission.handle(actor->slot), externalOrderRevision);
        if (!synchronized.succeeded())
        {
            result.status =
                ObjectAIContainmentTransitionStatus::OrderCancellationFailed;
            return result;
        }
        static_cast<void>(clearTransientSubject(subject));
    }

    const AIStateMachineRuntime& runtime =
        actorStorage->runtimes()[actor->slot];
    if (shadow.stageTransitionRequest({
            .slot = actor->slot,
            .subject = subject,
            .expectedState = runtime.currentState,
            .expectedRevision = runtime.revision,
            .operation = AIStateSoATransitionOperation::Direct,
            .target = state,
            .correlationIssuedTick = activationTick,
            .authority = AIStateTransitionAuthority::External,
            .reenter = runtime.currentState == state,
        }) != ObjectAIShadowBatchStatus::Success)
    {
        result.status = ObjectAIContainmentTransitionStatus::CapacityExceeded;
        return result;
    }

    AIStateParameters parameters = actorStorage->parameters()[actor->slot];
    parameters.goalObject = container;
    parameters.goalPosition = {};
    parameters.hasGoalPosition = false;
    actorStorage->parameters()[actor->slot] = parameters;
    m_pendingContainmentEntryFeedback.push_back(entryFeedback);
    result.status = ObjectAIContainmentTransitionStatus::Success;
    return result;
}

ObjectId ObjectAIRuntime::resolve(AIActorHandle handle) const noexcept
{
    return handle && handle.batch < m_batches.size() ? m_batches[handle.batch].resolve(handle) : INVALID_OBJECT_ID;
}

AIStateFamilySoAStorage* ObjectAIRuntime::storage(AIActorHandle handle) noexcept
{
    return resolve(handle) ? &m_batches[handle.batch].storage() : nullptr;
}

const AIStateFamilySoAStorage* ObjectAIRuntime::storage(AIActorHandle handle) const noexcept
{
    return resolve(handle) ? &m_batches[handle.batch].storage() : nullptr;
}

bool ObjectAIRuntime::initialized() const noexcept
{
    return m_initialized;
}

size_t ObjectAIRuntime::maximumActors() const noexcept
{
    return m_config.maximumActors;
}

size_t ObjectAIRuntime::slotsPerBatch() const noexcept
{
    return m_config.slotsPerBatch;
}

size_t ObjectAIRuntime::activeCount() const noexcept
{
    return m_subjects.size();
}

size_t ObjectAIRuntime::allocatedBatchCount() const noexcept
{
    return m_batches.size();
}

size_t ObjectAIRuntime::activeBatchCount() const noexcept
{
    return static_cast<size_t>(std::count_if(m_batches.begin(),
                                             m_batches.end(),
                                             [](const AIStateSoASlotRegistry& batch)
                                             { return batch.activeCount() != 0; }));
}

container::Span<const AIStateSoASubjectSlot> ObjectAIRuntime::orderedSubjects() const noexcept
{
    return m_subjects;
}

container::Span<AIStateSoASlotRegistry> ObjectAIRuntime::batches() noexcept
{
    return m_batches;
}

container::Span<const AIStateSoASlotRegistry> ObjectAIRuntime::batches() const noexcept
{
    return m_batches;
}

} // namespace engine::ai
