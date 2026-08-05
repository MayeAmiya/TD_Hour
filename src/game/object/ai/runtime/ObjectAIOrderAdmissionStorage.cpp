#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"

namespace engine::ai
{

    [[nodiscard]] ObjectAIOrderAdmissionStatus ObjectAIOrderAdmissionStorage::initialize(size_t capacity)
    {
        if (capacity == 0 ||
            capacity > static_cast<size_t>(ObjectAIOrderSlotHandle::InvalidSlot))
        {
            return ObjectAIOrderAdmissionStatus::InvalidCapacity;
        }

        m_subjects.assign(capacity, INVALID_OBJECT_ID);
        m_generations.assign(capacity, 1);
        m_capabilityMasks.assign(capacity, ObjectAIOrderCapability::None);
        m_observedQueueRevisions.assign(capacity, 0);
        m_observedExternalRevisions.assign(capacity, 0);
        m_orderQueueRevisions.assign(capacity, 0);
        m_orderExternalRevisions.assign(capacity, 0);
        m_orderIssuedTicks.assign(capacity, 0);
        m_orderSourceSequences.assign(capacity, 0);
        m_orderSourceScriptIds.assign(capacity, 0);
        m_orderPurposeInstances.assign(capacity, 0);
        m_orderKinds.assign(capacity, ObjectAIOrderKind::Invalid);
        m_orderAttackMoveMasks.assign(capacity, 0);
        m_orderMoveRouteSubtypes.assign(
            capacity, ObjectAIMoveRouteSubtype::Direct);
        m_orderWaypointStarts.assign(capacity, AIWaypointHandle{});
        m_orderWaypointGraphRevisions.assign(capacity, 0);
        m_orderWaypointTeams.assign(capacity, AITeamHandle{});
        m_orderTacticalAttackSubtypes.assign(
            capacity, ObjectAITacticalAttackSubtype::None);
        m_orderAllArmyHuntMasks.assign(capacity, 0);
        m_orderUseTeamCommonTargetMasks.assign(capacity, 0);
        m_orderSources.assign(capacity, ObjectAIOrderSource::System);
        m_orderPurposes.assign(
            capacity, ObjectAIOrderSystemPurpose::Generic);
        m_boundMasks.assign(capacity, 0);
        m_activeMasks.assign(capacity, 0);
        m_historyMasks.assign(capacity, 0);
        m_boundCount = 0;
        m_initialized = true;
        return ObjectAIOrderAdmissionStatus::Success;
    }

    [[nodiscard]] ObjectAIOrderAdmissionResult ObjectAIOrderAdmissionStorage::bind(
        uint32_t slot, ObjectId subject,
        ObjectAIOrderCapability capabilities,
        uint64_t observedQueueRevision,
        uint64_t observedExternalRevision) noexcept
    {
        ObjectAIOrderAdmissionResult result;
        if (!m_initialized)
            return fail(result, ObjectAIOrderAdmissionStatus::NotInitialized);
        if (!validSlot(slot))
            return fail(result, ObjectAIOrderAdmissionStatus::InvalidSlot);
        if (!subject)
            return fail(result, ObjectAIOrderAdmissionStatus::InvalidSubject);
        if (!isValidObjectAIOrderCapabilityMask(capabilities))
            return fail(result, ObjectAIOrderAdmissionStatus::InvalidCapabilityMask);
        if (m_boundMasks[slot] != 0)
            return fail(result, ObjectAIOrderAdmissionStatus::SlotAlreadyBound);
        for (size_t index = 0; index < m_subjects.size(); ++index)
        {
            if (m_boundMasks[index] != 0 && m_subjects[index] == subject)
                return fail(result, ObjectAIOrderAdmissionStatus::DuplicateSubject);
        }

        clearOrder(slot);
        m_subjects[slot] = subject;
        m_capabilityMasks[slot] = capabilities;
        m_observedQueueRevisions[slot] = observedQueueRevision;
        m_observedExternalRevisions[slot] = observedExternalRevision;
        m_boundMasks[slot] = 1;
        ++m_boundCount;

        result.action = ObjectAIOrderAdmissionAction::Bound;
        result.handle = makeHandle(slot);
        return result;
    }

    [[nodiscard]] ObjectAIOrderAdmissionResult ObjectAIOrderAdmissionStorage::release(
        ObjectAIOrderSlotHandle handle) noexcept
    {
        ObjectAIOrderAdmissionResult result;
        const ObjectAIOrderAdmissionStatus status = validateHandle(handle);
        if (status != ObjectAIOrderAdmissionStatus::Success)
            return fail(result, status);

        result.handle = handle;
        setPrevious(result, handle.slot);
        m_subjects[handle.slot] = INVALID_OBJECT_ID;
        m_capabilityMasks[handle.slot] = ObjectAIOrderCapability::None;
        m_observedQueueRevisions[handle.slot] = 0;
        m_observedExternalRevisions[handle.slot] = 0;
        m_boundMasks[handle.slot] = 0;
        clearOrder(handle.slot);
        uint32_t& generation = m_generations[handle.slot];
        ++generation;
        if (generation == 0)
            ++generation;
        --m_boundCount;
        result.action = ObjectAIOrderAdmissionAction::Released;
        return result;
    }

    [[nodiscard]] ObjectAIOrderAdmissionResult ObjectAIOrderAdmissionStorage::setCapabilities(
        ObjectAIOrderSlotHandle handle,
        ObjectAIOrderCapability capabilities) noexcept
    {
        ObjectAIOrderAdmissionResult result;
        const ObjectAIOrderAdmissionStatus status = validateHandle(handle);
        if (status != ObjectAIOrderAdmissionStatus::Success)
            return fail(result, status);
        result.handle = handle;
        if (!isValidObjectAIOrderCapabilityMask(capabilities))
            return fail(result, ObjectAIOrderAdmissionStatus::InvalidCapabilityMask);
        if (m_activeMasks[handle.slot] != 0 &&
            objectAIOrderOwner(loadOrder(handle.slot), capabilities) !=
                ObjectAIOrderOwner::ObjectAIRuntime)
        {
            return fail(
                result,
                ObjectAIOrderAdmissionStatus::ActiveOrderWouldLoseOwnership);
        }
        m_capabilityMasks[handle.slot] = capabilities;
        result.action = ObjectAIOrderAdmissionAction::CapabilitiesChanged;
        setCurrent(result, handle.slot);
        return result;
    }

    [[nodiscard]] ObjectAIOrderAdmissionResult ObjectAIOrderAdmissionStorage::admit(
        ObjectAIOrderSlotHandle handle,
        const ObjectAIOrderAdmissionRequest& request) noexcept
    {
        ObjectAIOrderAdmissionResult result;
        const ObjectAIOrderAdmissionStatus status =
            validateMutation(handle, request, result);
        if (status != ObjectAIOrderAdmissionStatus::Success)
            return fail(result, status);
        const uint32_t slot = handle.slot;
        if (m_activeMasks[slot] != 0 &&
            request.kind != ObjectAIOrderKind::Stop)
        {
            setPrevious(result, slot);
            return fail(result, ObjectAIOrderAdmissionStatus::ActiveOrderExists);
        }
        if (m_activeMasks[slot] != 0)
            setPrevious(result, slot);

        const ObjectAIOrderAdmissionStatus revisionStatus =
            validateNewerIdentity(slot, request.identity,
                                  request.kind == ObjectAIOrderKind::Stop);
        if (revisionStatus != ObjectAIOrderAdmissionStatus::Success)
            return fail(result, revisionStatus);

        if (request.kind == ObjectAIOrderKind::Stop)
            return applySynchronousStop(result, slot, request);

        storeOrder(slot, request);
        m_activeMasks[slot] = 1;
        result.action = ObjectAIOrderAdmissionAction::Admitted;
        setCurrent(result, slot);
        return result;
    }

    [[nodiscard]] ObjectAIOrderAdmissionResult ObjectAIOrderAdmissionStorage::replace(
        ObjectAIOrderSlotHandle handle,
        const ObjectAIOrderIdentity& expectedIdentity,
        const ObjectAIOrderAdmissionRequest& replacement) noexcept
    {
        ObjectAIOrderAdmissionResult result;
        const ObjectAIOrderAdmissionStatus status =
            validateMutation(handle, replacement, result);
        if (status != ObjectAIOrderAdmissionStatus::Success)
            return fail(result, status);
        const uint32_t slot = handle.slot;
        if (m_activeMasks[slot] == 0)
            return fail(result, ObjectAIOrderAdmissionStatus::NoActiveOrder);
        setPrevious(result, slot);
        if (loadIdentity(slot) != expectedIdentity)
            return fail(result, ObjectAIOrderAdmissionStatus::StaleIdentity);

        const ObjectAIOrderAdmissionStatus revisionStatus =
            validateNewerIdentity(slot, replacement.identity,
                                  replacement.kind == ObjectAIOrderKind::Stop);
        if (revisionStatus != ObjectAIOrderAdmissionStatus::Success)
            return fail(result, revisionStatus);

        if (replacement.kind == ObjectAIOrderKind::Stop)
            return applySynchronousStop(result, slot, replacement);

        storeOrder(slot, replacement);
        m_activeMasks[slot] = 1;
        result.action = ObjectAIOrderAdmissionAction::Replaced;
        setCurrent(result, slot);
        return result;
    }

    [[nodiscard]] ObjectAIOrderAdmissionResult ObjectAIOrderAdmissionStorage::cancel(
        ObjectAIOrderSlotHandle handle,
        const ObjectAIOrderIdentity& expectedIdentity) noexcept
    {
        ObjectAIOrderAdmissionResult result;
        const ObjectAIOrderAdmissionStatus status = validateHandle(handle);
        if (status != ObjectAIOrderAdmissionStatus::Success)
            return fail(result, status);
        result.handle = handle;
        const uint32_t slot = handle.slot;
        if (m_activeMasks[slot] == 0)
            return fail(result, ObjectAIOrderAdmissionStatus::NoActiveOrder);
        setPrevious(result, slot);
        if (loadIdentity(slot) != expectedIdentity)
            return fail(result, ObjectAIOrderAdmissionStatus::StaleIdentity);

        m_activeMasks[slot] = 0;
        result.action = ObjectAIOrderAdmissionAction::Cancelled;
        return result;
    }

    [[nodiscard]] ObjectAIOrderAdmissionResult ObjectAIOrderAdmissionStorage::complete(
        ObjectAIOrderSlotHandle handle,
        const ObjectAIOrderIdentity& expectedIdentity,
        ObjectAIOrderCompletion completion,
        bool outputCapacityAvailable) noexcept
    {
        ObjectAIOrderAdmissionResult result;
        const ObjectAIOrderAdmissionStatus status = validateHandle(handle);
        if (status != ObjectAIOrderAdmissionStatus::Success)
            return fail(result, status);
        result.handle = handle;
        const uint32_t slot = handle.slot;
        if (m_activeMasks[slot] == 0)
            return fail(result, ObjectAIOrderAdmissionStatus::NoActiveOrder);
        setPrevious(result, slot);
        if (loadIdentity(slot) != expectedIdentity)
            return fail(result, ObjectAIOrderAdmissionStatus::StaleIdentity);
        if (!outputCapacityAvailable)
        {
            return fail(
                result,
                ObjectAIOrderAdmissionStatus::CompletionOutputCapacityExceeded);
        }

        m_activeMasks[slot] = 0;
        switch (completion)
        {
        case ObjectAIOrderCompletion::Success:
            result.action = ObjectAIOrderAdmissionAction::CompletedSuccess;
            break;
        case ObjectAIOrderCompletion::Cancelled:
            result.action = ObjectAIOrderAdmissionAction::CompletedCancelled;
            break;
        case ObjectAIOrderCompletion::Failed:
            result.action = ObjectAIOrderAdmissionAction::CompletedFailed;
            break;
        }
        return result;
    }

    [[nodiscard]] ObjectAIOrderAdmissionResult ObjectAIOrderAdmissionStorage::synchronizeExternalRevision(
        ObjectAIOrderSlotHandle handle,
        uint64_t externalRevision) noexcept
    {
        ObjectAIOrderAdmissionResult result;
        const ObjectAIOrderAdmissionStatus status = validateHandle(handle);
        if (status != ObjectAIOrderAdmissionStatus::Success)
            return fail(result, status);
        result.handle = handle;
        const uint32_t slot = handle.slot;
        if (externalRevision < m_observedExternalRevisions[slot])
            return fail(result, ObjectAIOrderAdmissionStatus::StaleExternalRevision);
        if (externalRevision == m_observedExternalRevisions[slot])
        {
            setCurrent(result, slot);
            return result;
        }

        setPrevious(result, slot);
        m_observedExternalRevisions[slot] = externalRevision;
        m_activeMasks[slot] = 0;
        result.action = ObjectAIOrderAdmissionAction::ExternalRevisionSynchronized;
        return result;
    }

    [[nodiscard]] ObjectAIOrderAdmissionResult ObjectAIOrderAdmissionStorage::applySynchronousStop(
        ObjectAIOrderAdmissionResult& result, uint32_t slot,
        const ObjectAIOrderAdmissionRequest& request) noexcept
    {
        if (!result.hadPreviousOrder)
            setPrevious(result, slot);
        m_observedQueueRevisions[slot] = request.identity.queueRevision;
        m_observedExternalRevisions[slot] = request.identity.externalRevision;
        m_activeMasks[slot] = 0;
        result.action = ObjectAIOrderAdmissionAction::SynchronousStop;
        result.hasCurrentOrder = false;
        return result;
    }

    void ObjectAIOrderAdmissionStorage::storeOrder(uint32_t slot,
                    const ObjectAIOrderAdmissionRequest& request) noexcept
    {
        m_orderKinds[slot] = request.kind;
        m_orderAttackMoveMasks[slot] =
            request.attackMove ? uint8_t{1} : uint8_t{0};
        m_orderMoveRouteSubtypes[slot] = request.moveRouteSubtype;
        m_orderWaypointStarts[slot] = request.waypointStart;
        m_orderWaypointGraphRevisions[slot] = request.waypointGraphRevision;
        m_orderWaypointTeams[slot] = request.waypointTeam;
        m_orderTacticalAttackSubtypes[slot] =
            request.tacticalAttackSubtype;
        m_orderAllArmyHuntMasks[slot] =
            request.allArmyHunt ? uint8_t{1} : uint8_t{0};
        m_orderUseTeamCommonTargetMasks[slot] =
            request.useTeamCommonTarget ? uint8_t{1} : uint8_t{0};
        m_orderQueueRevisions[slot] = request.identity.queueRevision;
        m_orderExternalRevisions[slot] = request.identity.externalRevision;
        m_orderIssuedTicks[slot] = request.identity.issuedTick;
        m_orderSourceSequences[slot] = request.identity.sourceSequence;
        m_orderSourceScriptIds[slot] = request.identity.sourceScriptId;
        m_orderSources[slot] = request.identity.source;
        m_orderPurposes[slot] = request.identity.systemPurpose;
        m_orderPurposeInstances[slot] = request.identity.systemPurposeInstance;
        m_observedQueueRevisions[slot] = request.identity.queueRevision;
        m_observedExternalRevisions[slot] = request.identity.externalRevision;
        m_historyMasks[slot] = 1;
    }

    void ObjectAIOrderAdmissionStorage::clearOrder(uint32_t slot) noexcept
    {
        m_orderKinds[slot] = ObjectAIOrderKind::Invalid;
        m_orderAttackMoveMasks[slot] = 0;
        m_orderMoveRouteSubtypes[slot] =
            ObjectAIMoveRouteSubtype::Direct;
        m_orderWaypointStarts[slot] = {};
        m_orderWaypointGraphRevisions[slot] = 0;
        m_orderWaypointTeams[slot] = {};
        m_orderTacticalAttackSubtypes[slot] =
            ObjectAITacticalAttackSubtype::None;
        m_orderAllArmyHuntMasks[slot] = 0;
        m_orderUseTeamCommonTargetMasks[slot] = 0;
        m_orderQueueRevisions[slot] = 0;
        m_orderExternalRevisions[slot] = 0;
        m_orderIssuedTicks[slot] = 0;
        m_orderSourceSequences[slot] = 0;
        m_orderSourceScriptIds[slot] = 0;
        m_orderSources[slot] = ObjectAIOrderSource::System;
        m_orderPurposes[slot] = ObjectAIOrderSystemPurpose::Generic;
        m_orderPurposeInstances[slot] = 0;
        m_activeMasks[slot] = 0;
        m_historyMasks[slot] = 0;
    }

    void ObjectAIOrderAdmissionStorage::setPrevious(ObjectAIOrderAdmissionResult& result,
                     uint32_t slot) const noexcept
    {
        if (m_activeMasks[slot] == 0)
            return;
        result.previousOrder = loadOrder(slot);
        result.hadPreviousOrder = true;
    }

    void ObjectAIOrderAdmissionStorage::setCurrent(ObjectAIOrderAdmissionResult& result,
                    uint32_t slot) const noexcept
    {
        if (m_activeMasks[slot] == 0)
            return;
        result.currentOrder = loadOrder(slot);
        result.hasCurrentOrder = true;
    }

} // namespace engine::ai
