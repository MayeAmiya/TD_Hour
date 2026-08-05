#pragma once

#include "core/container/container_types.h"
#include "core/platform/runtime_mailbox.h"
#include "game/script/bridge/ScriptSessionEvents.h"
#include "game/script/presentation/ScriptCinematicPresentationControls.h"
#include "game/script/presentation/ScriptUiPresentationControls.h"

#include <cstddef>
#include <cstdint>
#include <variant>

namespace engine {
class GameSession;
}

namespace app::runtime {

// Value-only named-indicator projection. The authored presentation value is
// retained for its label/type/stamp, while the ScriptRuntime counter lookup is
// completed on the logic thread.
struct ScriptUiNamedIndicatorProjection final {
    engine::script::ScriptNamedIndicatorPresentation presentation;
    int32_t value = 0;
    bool valueResolved = false;
};

using ScriptUiProjectionEventPayload = std::variant<
    engine::script::ScriptSessionEvent,
    engine::script::ScriptCameoFlashPresentation,
    engine::script::ScriptLocalDefeatPresentation>;

// `sequence` is the projection journal's total delivery order. Presentation
// payloads which already carry a source stamp retain it unchanged.
struct ScriptUiProjectionEvent final {
    uint64_t sequence = 0;
    ScriptUiProjectionEventPayload payload;
};

// A cursor names the next projection-journal event a consumer expects.
// Session/epoch are part of the identity so a cursor cannot cross Next/Retry
// or a reused GameSession's presentation reset.
struct ScriptUiProjectionCursor final {
    uint64_t sessionRevision = 0;
    uint64_t presentationEpoch = 0;
    uint64_t nextSequence = 0;
};

// Immutable retained tail published with every newest-value snapshot. A UI
// consumer can reconnect using its cursor without relying on observing every
// intermediate ScriptUiProjection. Bounded-tail expiry is explicit through
// `canResume()` and `droppedEventCount`, never a silent newest-only loss.
struct ScriptUiProjectionEventBatch final {
    uint64_t sessionRevision = 0;
    uint64_t presentationEpoch = 0;
    uint64_t firstSequence = 1;
    uint64_t nextSequence = 1;
    uint64_t droppedEventCount = 0;
    container::Vector<ScriptUiProjectionEvent> events;

    [[nodiscard]] ScriptUiProjectionCursor beginCursor() const noexcept;
    [[nodiscard]] ScriptUiProjectionCursor endCursor() const noexcept;
    [[nodiscard]] bool sameScope(
        const ScriptUiProjectionCursor& cursor) const noexcept;
    [[nodiscard]] bool canResume(
        const ScriptUiProjectionCursor& cursor) const noexcept;
    [[nodiscard]] container::Span<const ScriptUiProjectionEvent> eventsFrom(
        const ScriptUiProjectionCursor& cursor) const noexcept;
};

// Logic-thread-built, pointer-free view of script-owned in-game UI state.
// `eventBatch` points only to immutable value data owned by this projection
// module; it never owns or aliases GameSession, ECS, registry, or content data.
struct ScriptUiProjection final {
    uint64_t revision = 0;
    uint64_t sessionRevision = 0;
    uint64_t confirmedTick = 0;
    uint64_t presentationEpoch = 0;

    bool gameplayInputEnabled = true;
    bool specialPowerDisplayEnabled = true;
    bool namedTimerDisplayEnabled = true;
    engine::script::ScriptLetterboxPresentationState letterbox;
    engine::script::ScriptPopupMessagePresentation popup;
    container::Vector<ScriptUiNamedIndicatorProjection> namedIndicators;

    container::SharedPtr<const ScriptUiProjectionEventBatch> eventBatch;
};

class ScriptUiProjectionPublisher final {
public:
    // This is deliberately larger than each session-owned source journal.
    // A slow UI can reconnect from a newest-value mailbox; if it falls behind
    // even this retained tail, the batch reports cursor expiry explicitly.
    static constexpr size_t kMaximumRetainedEvents = 4096;

    // Logic-thread only: drains ScriptSessionEvent and cameo queues, samples
    // local-defeat replacements, then builds a fully detached value
    // projection. Movies are synchronously complete and never cross this
    // presentation boundary.
    [[nodiscard]] ScriptUiProjection build(
        engine::GameSession& session, uint64_t sessionRevision);

    // Drops the retained scope without rewinding revision/sequence counters,
    // so an old cursor can never alias a later publication from this object.
    void reset() noexcept;

private:
    void resetScope(uint64_t sessionRevision, uint64_t presentationEpoch);
    void appendEvent(ScriptUiProjectionEventPayload payload);
    void publishEventBatch();

    bool m_hasScope = false;
    uint64_t m_sessionRevision = 0;
    uint64_t m_presentationEpoch = 0;
    uint64_t m_nextProjectionRevision = 1;
    uint64_t m_nextEventSequence = 1;
    uint64_t m_lastLocalDefeatSequence = 0;
    uint64_t m_droppedEventCount = 0;
    bool m_eventBatchDirty = true;
    container::Vector<ScriptUiProjectionEvent> m_retainedEvents;
    container::SharedPtr<const ScriptUiProjectionEventBatch> m_eventBatch;
};

using ScriptUiProjectionMailbox =
    platform::runtime::LatestValueMailbox<ScriptUiProjection>;

} // namespace app::runtime
