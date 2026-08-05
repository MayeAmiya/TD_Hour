#include "ScriptUiProjection.h"

#include "game/session/core/GameSession.h"

#include <memory>
#include <optional>
#include <utility>

namespace app::runtime {

ScriptUiProjectionCursor ScriptUiProjectionEventBatch::beginCursor() const noexcept {
    return {
        .sessionRevision = sessionRevision,
        .presentationEpoch = presentationEpoch,
        .nextSequence = firstSequence,
    };
}

ScriptUiProjectionCursor ScriptUiProjectionEventBatch::endCursor() const noexcept {
    return {
        .sessionRevision = sessionRevision,
        .presentationEpoch = presentationEpoch,
        .nextSequence = nextSequence,
    };
}

bool ScriptUiProjectionEventBatch::sameScope(
    const ScriptUiProjectionCursor& cursor) const noexcept {
    return cursor.sessionRevision == sessionRevision &&
        cursor.presentationEpoch == presentationEpoch;
}

bool ScriptUiProjectionEventBatch::canResume(
    const ScriptUiProjectionCursor& cursor) const noexcept {
    return sameScope(cursor) && cursor.nextSequence >= firstSequence &&
        cursor.nextSequence <= nextSequence;
}

container::Span<const ScriptUiProjectionEvent>
ScriptUiProjectionEventBatch::eventsFrom(
    const ScriptUiProjectionCursor& cursor) const noexcept {
    if (!canResume(cursor)) return {};
    const size_t offset = static_cast<size_t>(
        cursor.nextSequence - firstSequence);
    return container::Span<const ScriptUiProjectionEvent>{events}.subspan(offset);
}

ScriptUiProjection ScriptUiProjectionPublisher::build(
    engine::GameSession& session, uint64_t sessionRevision) {
    engine::GameSessionScriptUiPort source = session.scriptUiPort();
    const uint64_t presentationEpoch = source.presentationEpoch();
    if (!m_hasScope || m_sessionRevision != sessionRevision ||
        m_presentationEpoch != presentationEpoch) {
        resetScope(sessionRevision, presentationEpoch);
    }

    ScriptUiProjection projection;
    projection.revision = m_nextProjectionRevision++;
    if (m_nextProjectionRevision == 0) m_nextProjectionRevision = 1;
    projection.sessionRevision = sessionRevision;
    projection.confirmedTick = session.confirmedTick();
    projection.presentationEpoch = presentationEpoch;

    const engine::script::ScriptUiPresentationState scriptUi =
        source.state();
    projection.gameplayInputEnabled = scriptUi.gameplayInputEnabled();
    projection.specialPowerDisplayEnabled =
        scriptUi.specialPowerDisplayEnabled();
    projection.namedTimerDisplayEnabled =
        scriptUi.namedTimerDisplayEnabled();
    projection.letterbox = source.letterbox();
    projection.popup = scriptUi.popup();

    const auto indicators = scriptUi.namedIndicators();
    projection.namedIndicators.reserve(indicators.size());
    for (const engine::script::ScriptNamedIndicatorPresentation& indicator :
         indicators) {
        const std::optional<int32_t> value =
            source.counterValue(indicator.counterName);
        projection.namedIndicators.push_back({
            .presentation = indicator,
            .value = value.value_or(0),
            .valueResolved = value.has_value(),
        });
    }

    container::Vector<engine::script::ScriptSessionEvent> sessionEvents =
        source.takeSessionEvents();
    for (engine::script::ScriptSessionEvent& event : sessionEvents) {
        appendEvent(std::move(event));
    }

    container::Vector<engine::script::ScriptCameoFlashPresentation> cameos =
        source.takeCameoFlashes();
    for (engine::script::ScriptCameoFlashPresentation& cameo : cameos) {
        appendEvent(std::move(cameo));
    }

    const engine::script::ScriptLocalDefeatPresentation& localDefeat =
        scriptUi.localDefeat();
    if (localDefeat.active &&
        localDefeat.stamp.presentationEpoch == presentationEpoch &&
        localDefeat.stamp.sequence > m_lastLocalDefeatSequence) {
        appendEvent(localDefeat);
        m_lastLocalDefeatSequence = localDefeat.stamp.sequence;
    }

    if (m_retainedEvents.size() > kMaximumRetainedEvents) {
        const size_t overflow =
            m_retainedEvents.size() - kMaximumRetainedEvents;
        m_retainedEvents.erase(
            m_retainedEvents.begin(),
            m_retainedEvents.begin() + static_cast<std::ptrdiff_t>(overflow));
        m_droppedEventCount += static_cast<uint64_t>(overflow);
        m_eventBatchDirty = true;
    }
    if (m_eventBatchDirty || !m_eventBatch) publishEventBatch();
    projection.eventBatch = m_eventBatch;
    return projection;
}

void ScriptUiProjectionPublisher::reset() noexcept {
    m_hasScope = false;
    m_sessionRevision = 0;
    m_presentationEpoch = 0;
    m_lastLocalDefeatSequence = 0;
    m_droppedEventCount = 0;
    m_eventBatchDirty = true;
    m_retainedEvents.clear();
    m_eventBatch.reset();
}

void ScriptUiProjectionPublisher::resetScope(
    uint64_t sessionRevision, uint64_t presentationEpoch) {
    m_hasScope = true;
    m_sessionRevision = sessionRevision;
    m_presentationEpoch = presentationEpoch;
    m_lastLocalDefeatSequence = 0;
    m_droppedEventCount = 0;
    m_retainedEvents.clear();
    m_eventBatchDirty = true;
}

void ScriptUiProjectionPublisher::appendEvent(
    ScriptUiProjectionEventPayload payload) {
    uint64_t sequence = m_nextEventSequence++;
    if (sequence == 0) {
        sequence = m_nextEventSequence++;
    }
    m_retainedEvents.push_back({
        .sequence = sequence,
        .payload = std::move(payload),
    });
    m_eventBatchDirty = true;
}

void ScriptUiProjectionPublisher::publishEventBatch() {
    ScriptUiProjectionEventBatch batch;
    batch.sessionRevision = m_sessionRevision;
    batch.presentationEpoch = m_presentationEpoch;
    batch.firstSequence = m_retainedEvents.empty()
        ? m_nextEventSequence
        : m_retainedEvents.front().sequence;
    batch.nextSequence = m_nextEventSequence;
    batch.droppedEventCount = m_droppedEventCount;
    batch.events = m_retainedEvents;
    m_eventBatch =
        std::make_shared<const ScriptUiProjectionEventBatch>(std::move(batch));
    m_eventBatchDirty = false;
}

} // namespace app::runtime
