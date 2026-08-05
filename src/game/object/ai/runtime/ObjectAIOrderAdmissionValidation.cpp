#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"

namespace engine::ai
{

    [[nodiscard]] ObjectAIOrderAdmissionResult& ObjectAIOrderAdmissionStorage::fail(
        ObjectAIOrderAdmissionResult& result,
        ObjectAIOrderAdmissionStatus status) noexcept
    {
        result.status = status;
        result.action = ObjectAIOrderAdmissionAction::None;
        result.hasCurrentOrder = false;
        return result;
    }

    [[nodiscard]] bool ObjectAIOrderAdmissionStorage::validSlot(uint32_t slot) const noexcept
    {
        return static_cast<size_t>(slot) < m_subjects.size();
    }

    [[nodiscard]] ObjectAIOrderSlotHandle ObjectAIOrderAdmissionStorage::makeHandle(uint32_t slot) const noexcept
    {
        return {slot, m_generations[slot], m_subjects[slot]};
    }

    [[nodiscard]] ObjectAIOrderAdmissionStatus ObjectAIOrderAdmissionStorage::validateHandle(
        ObjectAIOrderSlotHandle handle) const noexcept
    {
        if (!m_initialized)
            return ObjectAIOrderAdmissionStatus::NotInitialized;
        if (!handle.isValid() || !validSlot(handle.slot))
            return ObjectAIOrderAdmissionStatus::InvalidSlot;
        if (m_generations[handle.slot] != handle.generation)
            return ObjectAIOrderAdmissionStatus::StaleGeneration;
        if (m_boundMasks[handle.slot] == 0)
            return ObjectAIOrderAdmissionStatus::SlotNotBound;
        if (m_subjects[handle.slot] != handle.subject)
            return ObjectAIOrderAdmissionStatus::SubjectMismatch;
        return ObjectAIOrderAdmissionStatus::Success;
    }

    [[nodiscard]] ObjectAIOrderAdmissionStatus ObjectAIOrderAdmissionStorage::validateMutation(
        ObjectAIOrderSlotHandle handle,
        const ObjectAIOrderAdmissionRequest& request,
        ObjectAIOrderAdmissionResult& result) const noexcept
    {
        const ObjectAIOrderAdmissionStatus handleStatus = validateHandle(handle);
        if (handleStatus != ObjectAIOrderAdmissionStatus::Success)
            return handleStatus;
        result.handle = handle;
        if (!isValidObjectAIOrderKind(request.kind))
            return ObjectAIOrderAdmissionStatus::InvalidOrderKind;
        if (request.attackMove && request.kind != ObjectAIOrderKind::Move)
            return ObjectAIOrderAdmissionStatus::InvalidOrderKind;
        if (!isValidObjectAIOrderSource(request.identity.source))
            return ObjectAIOrderAdmissionStatus::InvalidOrderSource;
        if (!isValidObjectAIOrderSystemPurpose(request.identity.systemPurpose))
            return ObjectAIOrderAdmissionStatus::InvalidSystemPurpose;
        if (!request.identity.subject)
            return ObjectAIOrderAdmissionStatus::InvalidSubject;
        if (request.identity.subject != handle.subject)
            return ObjectAIOrderAdmissionStatus::SubjectMismatch;
        if (!request.isValid())
            return ObjectAIOrderAdmissionStatus::InvalidOrderKind;

        result.owner = objectAIOrderOwner(
            request, m_capabilityMasks[handle.slot]);
        if (result.owner == ObjectAIOrderOwner::Unsupported)
            return ObjectAIOrderAdmissionStatus::UnsupportedOrder;
        if (result.owner != ObjectAIOrderOwner::ObjectAIRuntime)
            return ObjectAIOrderAdmissionStatus::NotOwnedByObjectAI;
        return ObjectAIOrderAdmissionStatus::Success;
    }

    [[nodiscard]] ObjectAIOrderAdmissionStatus ObjectAIOrderAdmissionStorage::validateNewerIdentity(
        uint32_t slot, const ObjectAIOrderIdentity& identity,
        bool synchronousStop) const noexcept
    {
        if (identity.queueRevision < m_observedQueueRevisions[slot])
            return ObjectAIOrderAdmissionStatus::StaleQueueRevision;
        if (identity.externalRevision < m_observedExternalRevisions[slot])
            return ObjectAIOrderAdmissionStatus::StaleExternalRevision;

        if (synchronousStop)
        {
            return identity.externalRevision == m_observedExternalRevisions[slot]
                ? ObjectAIOrderAdmissionStatus::StaleExternalRevision
                : ObjectAIOrderAdmissionStatus::Success;
        }
        if (m_historyMasks[slot] != 0 &&
            identity.queueRevision == m_observedQueueRevisions[slot] &&
            identity.externalRevision == m_observedExternalRevisions[slot])
        {
            return ObjectAIOrderAdmissionStatus::StaleIdentity;
        }
        return ObjectAIOrderAdmissionStatus::Success;
    }

    [[nodiscard]] ObjectAIOrderIdentity ObjectAIOrderAdmissionStorage::loadIdentity(uint32_t slot) const noexcept
    {
        return {
            .subject = m_subjects[slot],
            .queueRevision = m_orderQueueRevisions[slot],
            .externalRevision = m_orderExternalRevisions[slot],
            .issuedTick = m_orderIssuedTicks[slot],
            .sourceSequence = m_orderSourceSequences[slot],
            .sourceScriptId = m_orderSourceScriptIds[slot],
            .source = m_orderSources[slot],
            .systemPurpose = m_orderPurposes[slot],
            .systemPurposeInstance = m_orderPurposeInstances[slot],
        };
    }

    [[nodiscard]] bool ObjectAIOrderAdmissionStorage::canonicalUnboundSnapshot(
        const ObjectAIOrderAdmissionSlotSnapshot& value) noexcept
    {
        return !value.subject && value.capabilities == ObjectAIOrderCapability::None &&
               value.observedQueueRevision == 0 &&
               value.observedExternalRevision == 0 && !value.active &&
               !value.hasHistory;
    }

    [[nodiscard]] bool ObjectAIOrderAdmissionStorage::validateSnapshot(
        const ObjectAIOrderAdmissionSnapshot& snapshot) noexcept
    {
        for (size_t index = 0; index < snapshot.slots.size(); ++index)
        {
            const ObjectAIOrderAdmissionSlotSnapshot& value = snapshot.slots[index];
            if (value.generation == 0 ||
                !isValidObjectAIOrderCapabilityMask(value.capabilities))
            {
                return false;
            }
            if (!value.bound)
            {
                if (!canonicalUnboundSnapshot(value))
                    return false;
                continue;
            }
            if (!value.subject || value.active && !value.hasHistory)
                return false;
            for (size_t previous = 0; previous < index; ++previous)
            {
                if (snapshot.slots[previous].bound &&
                    snapshot.slots[previous].subject == value.subject)
                {
                    return false;
                }
            }
            if (!value.hasHistory)
                continue;
            if (!value.historicalOrder.isValid() ||
                value.historicalOrder.identity.subject != value.subject ||
                value.historicalOrder.kind == ObjectAIOrderKind::Stop ||
                value.historicalOrder.identity.queueRevision >
                    value.observedQueueRevision ||
                value.historicalOrder.identity.externalRevision >
                    value.observedExternalRevision)
            {
                return false;
            }
            if (value.active &&
                objectAIOrderOwner(value.historicalOrder, value.capabilities) !=
                    ObjectAIOrderOwner::ObjectAIRuntime)
            {
                return false;
            }
        }
        return true;
    }

} // namespace engine::ai
