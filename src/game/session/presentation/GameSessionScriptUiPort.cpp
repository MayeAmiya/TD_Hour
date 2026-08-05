#include "game/session/presentation/GameSessionScriptUiPort.h"

#include "game/session/state/GameSessionDomainState.h"

#include <utility>

namespace engine {

uint64_t GameSessionScriptUiPort::presentationEpoch() const noexcept {
    return m_presentation->m_scriptPresentationEpoch;
}

script::ScriptUiPresentationState GameSessionScriptUiPort::state() const {
    return m_presentation->m_scriptUiPresentation;
}

script::ScriptLetterboxPresentationState GameSessionScriptUiPort::letterbox()
    const noexcept {
    return m_presentation->m_scriptLetterboxPresentation;
}

std::optional<int32_t> GameSessionScriptUiPort::counterValue(
    container::StringView name) const noexcept {
    return m_presentation->m_scriptRuntime.counterValue(name);
}

container::Vector<script::ScriptSessionEvent>
GameSessionScriptUiPort::takeSessionEvents() {
    container::Vector<script::ScriptSessionEvent> output =
        std::move(m_presentation->m_scriptSessionEvents);
    m_presentation->m_scriptSessionEvents.clear();
    return output;
}

container::Vector<script::ScriptCameoFlashPresentation>
GameSessionScriptUiPort::takeCameoFlashes() {
    return m_presentation->m_scriptUiPresentation.takeCameoFlashes();
}

} // namespace engine
