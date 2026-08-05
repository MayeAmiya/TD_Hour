#include "game/object/ai/runtime/ObjectAIShadowBatch.h"

namespace engine::ai
{

[[nodiscard]] bool ObjectAIShadowBatch::initialized() const noexcept { return m_initialized; }

[[nodiscard]] size_t ObjectAIShadowBatch::capacity() const noexcept { return m_capacity; }

[[nodiscard]] container::Span<AIStateSoATransitionRequest> ObjectAIShadowBatch::transitionRequests() noexcept
{
    return m_transitionRequests;
}

[[nodiscard]] container::Span<const AIStateSoATransitionRequest> ObjectAIShadowBatch::transitionRequests() const noexcept
{
    return m_transitionRequests;
}

[[nodiscard]] bool ObjectAIShadowBatch::alignedWith(const AIStateFamilySoAStorage& storage) const noexcept
{
    return m_initialized && storage.size() == m_capacity;
}

[[nodiscard]] AIStateSoAMultiwaveInput ObjectAIShadowBatch::makeInput(AIStateFamilySoAStorage& storage,
                                                  uint64_t confirmedTick) noexcept
{
    return input(storage, confirmedTick);
}

[[nodiscard]] AIStateSoAMultiwaveScratch ObjectAIShadowBatch::makeScratch() noexcept { return scratch(); }

} // namespace engine::ai
