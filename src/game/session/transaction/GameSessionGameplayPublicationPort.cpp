#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

#include "game/session/state/GameSessionDomainState.h"
#include "game/session/presentation/GameSessionPresentationDetail.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "core/container/string_utils.h"

#include <optional>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] bool playableAudioEvent(
    container::StringView eventName) noexcept {
    // `NoSound` is the authored sentinel used throughout the original INIs.
    // It is not an AudioEvent name and must never reach the renderer/audio
    // queue, whether it originated from a voice, an Upgrade or an FX rule.
    return !eventName.empty() &&
        !container::asciiEqualIgnoreCase(eventName, "NoSound");
}

} // namespace

std::optional<PlayerId>
GameSessionGameplayPublicationPort::ownerPlayerFor(
    ObjectId object) const noexcept {
    if (!m_world || !object) return std::nullopt;
    const std::optional<ecs::entity> entity =
        m_world->m_objects.entityFromIdIncludingPending(object);
    const OwnerComponent* owner = entity
        ? ecs::try_get<OwnerComponent>(m_world->m_registry, *entity)
        : nullptr;
    return owner && owner->player
        ? std::optional<PlayerId>{owner->player}
        : std::nullopt;
}

bool GameSessionGameplayPublicationPort::raiseSimulationFault(
    SimulationFault fault) noexcept {
    if (!fault || !m_frame || !m_presentation) return false;
    if (fault.confirmedTick == 0) {
        fault.confirmedTick = m_frame->m_open
            ? m_frame->m_result.confirmedTick
            : m_presentation->m_confirmedTick;
    }
    if (!m_frame->m_open) {
        const bool installed = !m_frame->m_pendingFault;
        if (installed) m_frame->m_pendingFault = fault;
        else m_frame->m_pendingAdditionalFaultCount =
            game_session_presentation_detail::saturatingFrameCount(
                m_frame->m_pendingAdditionalFaultCount, 1);
        m_frame->m_result = {
            .state = FrameCommitState::Faulted,
            .confirmedTick = fault.confirmedTick,
            .additionalFaultCount = m_frame->m_pendingAdditionalFaultCount,
            .fault = m_frame->m_pendingFault,
        };
        return installed;
    }
    if (!m_frame->m_result.fault) {
        m_frame->m_result.fault = fault;
        return true;
    }
    m_frame->m_result.additionalFaultCount =
        game_session_presentation_detail::saturatingFrameCount(
            m_frame->m_result.additionalFaultCount, 1);
    return false;
}

bool GameSessionGameplayPublicationPort::emitAudioEvent(
    game::GameAudioEvent event) {
    if (m_world && !event.sourcePlayer) {
        const std::optional<ObjectId> source = event.owner
            ? event.owner : event.emitter;
        if (source && *source) {
            event.sourcePlayer = ownerPlayerFor(*source);
        }
    }
    return playableAudioEvent(event.eventName) &&
        m_content && m_presentation && m_content->m_active &&
        m_presentation->m_audioJournal.emit(
            std::move(event), m_content->m_startInfo.seed);
}

bool GameSessionGameplayPublicationPort::emitAudioControlEvent(
    game::GameAudioControlEvent event) {
    if (m_world && !event.sourcePlayer && event.object && *event.object) {
        event.sourcePlayer = ownerPlayerFor(*event.object);
    }
    // Audio controls are not all event-scoped: music/bus/EVA controls carry
    // no AudioEvent name.  GameSessionAudioJournal validates the fields that
    // each individual control kind actually requires (including object-loop
    // enable/disable), so do not reject valid global controls here.
    return m_content && m_presentation && m_content->m_active &&
        m_presentation->m_audioJournal.emit(std::move(event));
}

bool GameSessionGameplayPublicationPort::emitFxInvocationEvent(
    game::FxInvocationEvent event) {
    if (m_world && !event.sourcePlayer) {
        ObjectId source = event.primary.object;
        if (!source && event.secondary) source = event.secondary->object;
        event.sourcePlayer = ownerPlayerFor(source);
    }
    return m_content && m_presentation && m_content->m_active &&
        m_presentation->m_fxInvocations.emit(std::move(event));
}

void GameSessionGameplayPublicationPort::emitScriptSessionEvent(
    script::ScriptSessionEvent event) {
    if (!m_content || !m_presentation || !m_content->m_active ||
        event.text.empty()) return;
    m_presentation->m_scriptSessionEvents.push_back(std::move(event));
}

} // namespace engine
