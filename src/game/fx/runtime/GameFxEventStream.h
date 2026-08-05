#pragma once

#include "core/container/container_types.h"

#include "GameFxEvents.h"

#include <cstddef>
#include <cstdint>
namespace game {

// Session-owned ordered handoff for one-shot FX. It is deliberately separate
// from WorldRenderFrameQueue: a newest-only render snapshot may be superseded,
// while every confirmed FX invocation must be delivered exactly once and in
// producer order when presentation next drains this stream.
class FxInvocationEventStream final {
public:
    void reset(uint64_t presentationEpoch = 0, uint64_t sessionSeed = 0) noexcept;
    [[nodiscard]] bool beginConfirmedFrame(
        uint64_t confirmedFrame, bool continueSameFrame = false) noexcept;
    [[nodiscard]] bool emit(FxInvocationEvent event);
    [[nodiscard]] container::Vector<FxInvocationEvent> take();

    [[nodiscard]] uint64_t presentationEpoch() const noexcept { return m_presentationEpoch; }
    [[nodiscard]] uint64_t confirmedFrame() const noexcept { return m_confirmedFrame; }
    [[nodiscard]] size_t pendingCount() const noexcept { return m_events.size(); }
    [[nodiscard]] uint64_t lastEmittedSequence() const noexcept {
        return m_lastEmittedSequence;
    }
    [[nodiscard]] uint64_t consumedSequence() const noexcept {
        return m_consumedSequence;
    }
    [[nodiscard]] uint64_t rejectedEventCount() const noexcept {
        return m_rejectedEventCount;
    }

private:
    uint64_t m_presentationEpoch = 0;
    uint64_t m_sessionSeed = 0;
    uint64_t m_confirmedFrame = 0;
    uint64_t m_lastEmittedSequence = 0;
    uint64_t m_consumedSequence = 0;
    uint64_t m_rejectedEventCount = 0;
    uint32_t m_eventOrdinal = 0;
    bool m_hasConfirmedFrame = false;
    container::Vector<FxInvocationEvent> m_events;
};

} // namespace game
