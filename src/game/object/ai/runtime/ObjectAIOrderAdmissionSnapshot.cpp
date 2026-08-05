#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"

#include <utility>

namespace engine::ai
{

    [[nodiscard]] ObjectAIOrderAdmissionStatus ObjectAIOrderAdmissionStorage::activeOrder(
        ObjectAIOrderSlotHandle handle,
        ObjectAIOrderAdmissionRequest& output) const noexcept
    {
        const ObjectAIOrderAdmissionStatus status = validateHandle(handle);
        if (status != ObjectAIOrderAdmissionStatus::Success)
            return status;
        if (m_activeMasks[handle.slot] == 0)
            return ObjectAIOrderAdmissionStatus::NoActiveOrder;
        output = loadOrder(handle.slot);
        return ObjectAIOrderAdmissionStatus::Success;
    }

    [[nodiscard]] ObjectAIOrderAdmissionStatus ObjectAIOrderAdmissionStorage::readSlot(
        uint32_t slot, ObjectAIOrderAdmissionSlotView& output) const noexcept
    {
        output = {};
        if (!m_initialized)
            return ObjectAIOrderAdmissionStatus::NotInitialized;
        if (!validSlot(slot))
            return ObjectAIOrderAdmissionStatus::InvalidSlot;

        output.handle = makeHandle(slot);
        output.capabilities = m_capabilityMasks[slot];
        output.observedQueueRevision = m_observedQueueRevisions[slot];
        output.observedExternalRevision = m_observedExternalRevisions[slot];
        output.bound = m_boundMasks[slot] != 0;
        output.active = m_activeMasks[slot] != 0;
        output.hasHistory = m_historyMasks[slot] != 0;
        if (output.hasHistory)
            output.historicalOrder = loadOrder(slot);
        return ObjectAIOrderAdmissionStatus::Success;
    }

    [[nodiscard]] ObjectAIOrderAdmissionStatus ObjectAIOrderAdmissionStorage::captureSnapshot(
        ObjectAIOrderAdmissionSnapshot& output) const
    {
        if (!m_initialized)
            return ObjectAIOrderAdmissionStatus::NotInitialized;

        ObjectAIOrderAdmissionSnapshot candidate;
        candidate.slots.resize(capacity());
        for (size_t slot = 0; slot < capacity(); ++slot)
        {
            ObjectAIOrderAdmissionSlotSnapshot& value = candidate.slots[slot];
            value.subject = m_subjects[slot];
            value.generation = m_generations[slot];
            value.capabilities = m_capabilityMasks[slot];
            value.observedQueueRevision = m_observedQueueRevisions[slot];
            value.observedExternalRevision = m_observedExternalRevisions[slot];
            value.bound = m_boundMasks[slot] != 0;
            value.active = m_activeMasks[slot] != 0;
            value.hasHistory = m_historyMasks[slot] != 0;
            if (value.hasHistory)
                value.historicalOrder = loadOrder(static_cast<uint32_t>(slot));
        }
        output = std::move(candidate);
        return ObjectAIOrderAdmissionStatus::Success;
    }

    [[nodiscard]] ObjectAIOrderAdmissionResult ObjectAIOrderAdmissionStorage::restoreSnapshot(
        const ObjectAIOrderAdmissionSnapshot& snapshot)
    {
        ObjectAIOrderAdmissionResult result;
        if (snapshot.schemaVersion != ObjectAIOrderAdmissionSnapshot::SchemaVersion)
            return fail(result, ObjectAIOrderAdmissionStatus::InvalidSnapshotSchema);
        if (snapshot.slots.empty() ||
            snapshot.slots.size() >
                static_cast<size_t>(ObjectAIOrderSlotHandle::InvalidSlot))
        {
            return fail(result, ObjectAIOrderAdmissionStatus::SnapshotSizeMismatch);
        }
        if (m_initialized && snapshot.slots.size() != capacity())
            return fail(result, ObjectAIOrderAdmissionStatus::SnapshotSizeMismatch);
        if (!validateSnapshot(snapshot))
            return fail(result, ObjectAIOrderAdmissionStatus::InvalidSnapshot);

        ObjectAIOrderAdmissionStorage candidate;
        if (candidate.initialize(snapshot.slots.size()) !=
            ObjectAIOrderAdmissionStatus::Success)
        {
            return fail(result, ObjectAIOrderAdmissionStatus::InvalidSnapshot);
        }
        for (size_t index = 0; index < snapshot.slots.size(); ++index)
        {
            const uint32_t slot = static_cast<uint32_t>(index);
            const ObjectAIOrderAdmissionSlotSnapshot& value = snapshot.slots[index];
            candidate.m_generations[slot] = value.generation;
            if (!value.bound)
                continue;
            candidate.m_subjects[slot] = value.subject;
            candidate.m_capabilityMasks[slot] = value.capabilities;
            candidate.m_boundMasks[slot] = 1;
            ++candidate.m_boundCount;
            if (value.hasHistory)
                candidate.storeOrder(slot, value.historicalOrder);
            candidate.m_observedQueueRevisions[slot] = value.observedQueueRevision;
            candidate.m_observedExternalRevisions[slot] =
                value.observedExternalRevision;
            candidate.m_activeMasks[slot] = value.active ? uint8_t{1} : uint8_t{0};
        }
        *this = std::move(candidate);
        result.action = ObjectAIOrderAdmissionAction::Restored;
        return result;
    }

    [[nodiscard]] bool ObjectAIOrderAdmissionStorage::initialized() const noexcept { return m_initialized; }

    [[nodiscard]] size_t ObjectAIOrderAdmissionStorage::capacity() const noexcept { return m_subjects.size(); }

    [[nodiscard]] size_t ObjectAIOrderAdmissionStorage::boundCount() const noexcept { return m_boundCount; }

    [[nodiscard]] bool ObjectAIOrderAdmissionStorage::bound(uint32_t slot) const noexcept
    {
        return m_initialized && validSlot(slot) && m_boundMasks[slot] != 0;
    }

    [[nodiscard]] bool ObjectAIOrderAdmissionStorage::active(uint32_t slot) const noexcept
    {
        return m_initialized && validSlot(slot) && m_activeMasks[slot] != 0;
    }

    [[nodiscard]] uint32_t ObjectAIOrderAdmissionStorage::generation(uint32_t slot) const noexcept
    {
        return m_initialized && validSlot(slot) ? m_generations[slot] : 0;
    }

    [[nodiscard]] ObjectAIOrderSlotHandle ObjectAIOrderAdmissionStorage::handle(uint32_t slot) const noexcept
    {
        return m_initialized && validSlot(slot) && m_boundMasks[slot] != 0
            ? makeHandle(slot)
            : ObjectAIOrderSlotHandle{};
    }

    [[nodiscard]] container::Span<const ObjectId> ObjectAIOrderAdmissionStorage::subjects() const noexcept
    {
        return m_subjects;
    }

    [[nodiscard]] container::Span<const uint8_t> ObjectAIOrderAdmissionStorage::boundMask() const noexcept
    {
        return m_boundMasks;
    }

    [[nodiscard]] container::Span<const uint8_t> ObjectAIOrderAdmissionStorage::activeMask() const noexcept
    {
        return m_activeMasks;
    }

    [[nodiscard]] ObjectAIOrderAdmissionRequest ObjectAIOrderAdmissionStorage::loadOrder(
        uint32_t slot) const noexcept
    {
        return {
            .kind = m_orderKinds[slot],
            .identity = loadIdentity(slot),
            .attackMove = m_orderAttackMoveMasks[slot] != 0,
            .moveRouteSubtype = m_orderMoveRouteSubtypes[slot],
            .waypointStart = m_orderWaypointStarts[slot],
            .waypointGraphRevision = m_orderWaypointGraphRevisions[slot],
            .waypointTeam = m_orderWaypointTeams[slot],
            .tacticalAttackSubtype =
                m_orderTacticalAttackSubtypes[slot],
            .allArmyHunt = m_orderAllArmyHuntMasks[slot] != 0,
            .useTeamCommonTarget =
                m_orderUseTeamCommonTargetMasks[slot] != 0,
        };
    }

} // namespace engine::ai
