#include "game/object/ai/runtime/AIStateFamilySoAStorage.h"

namespace engine::ai
{

bool AIStateFamilySoAStorage::occupied(size_t slot) const noexcept
{
    return slot < size() && static_cast<bool>(m_subjects[slot]);
}

size_t AIStateFamilySoAStorage::activeSubjectCount() const noexcept
{
    return m_activeSubjectCount;
}

bool AIStateFamilySoAStorage::empty() const noexcept
{
    return m_activeSubjectCount == 0;
}

bool AIStateFamilySoAStorage::restorePayloadMetadata(size_t slot,
                                                     AIStateId state,
                                                     uint32_t activationSequence) noexcept
{
    if (slot >= size() || (state != AIStateId::Invalid && activationSequence == 0))
        return false;
    const AIStateId previousPayloadState = m_payloadStates[slot];
    if (isValidState(previousPayloadState))
        --m_activeStateCounts[static_cast<size_t>(previousPayloadState)];
    if (isValidState(state))
        ++m_activeStateCounts[static_cast<size_t>(state)];
    m_payloadStates[slot] = state;
    m_activationSequences[slot] = activationSequence;
    return true;
}

size_t AIStateFamilySoAStorage::activeStateCount(AIStateId state) const noexcept
{
    return isValidState(state) ? m_activeStateCounts[static_cast<size_t>(state)] : 0;
}

} // namespace engine::ai
