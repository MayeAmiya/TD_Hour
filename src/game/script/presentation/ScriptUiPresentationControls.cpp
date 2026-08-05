#include "core/container/container_types.h"
#include "ScriptUiPresentationControls.h"

#include <algorithm>
#include <utility>

namespace engine::script {
namespace {

[[nodiscard]] auto findNamedIndicator(
    container::Vector<ScriptNamedIndicatorPresentation>& indicators,
    container::StringView counterName) {
    return std::lower_bound(indicators.begin(), indicators.end(), counterName,
        [](const ScriptNamedIndicatorPresentation& entry, container::StringView name) {
            return entry.counterName < name;
        });
}

} // namespace

void ScriptUiPresentationState::reset(uint64_t presentationEpoch) noexcept {
    m_gameplayInputEnabled = true;
    m_specialPowerDisplayEnabled = true;
    m_namedTimerDisplayEnabled = true;
    m_namedIndicators.clear();
    m_popup = {};
    m_popup.stamp.presentationEpoch = presentationEpoch;
    m_localDefeat = {};
    m_localDefeat.stamp.presentationEpoch = presentationEpoch;
    m_lastMutation = {.presentationEpoch = presentationEpoch};
    m_pendingCameoFlashes.clear();
}

void ScriptUiPresentationState::rebindPresentationEpoch(
    uint64_t presentationEpoch) noexcept {
    for (ScriptNamedIndicatorPresentation& indicator : m_namedIndicators) {
        indicator.stamp.presentationEpoch = presentationEpoch;
    }
    m_popup.stamp.presentationEpoch = presentationEpoch;
    m_localDefeat.stamp.presentationEpoch = presentationEpoch;
    m_lastMutation.presentationEpoch = presentationEpoch;
    for (ScriptCameoFlashPresentation& flash : m_pendingCameoFlashes) {
        flash.stamp.presentationEpoch = presentationEpoch;
    }
}

bool ScriptUiPresentationState::setControl(ScriptUiControlKind control, bool enabled,
                                           ScriptPresentationControlStamp stamp) noexcept {
    bool* target = nullptr;
    switch (control) {
    case ScriptUiControlKind::GameplayInput:
        target = &m_gameplayInputEnabled;
        break;
    case ScriptUiControlKind::SpecialPowerDisplay:
        target = &m_specialPowerDisplayEnabled;
        break;
    case ScriptUiControlKind::NamedTimerDisplay:
        target = &m_namedTimerDisplayEnabled;
        break;
    }
    if (!target || *target == enabled) return false;
    *target = enabled;
    m_lastMutation = stamp;
    return true;
}

bool ScriptUiPresentationState::showNamedIndicator(container::String counterName, container::String label,
                                                    ScriptNamedIndicatorKind kind,
                                                    ScriptPresentationControlStamp stamp) {
    if (counterName.empty() || label.empty()) return false;

    auto found = findNamedIndicator(m_namedIndicators, counterName);
    if (found != m_namedIndicators.end() && found->counterName == counterName) {
        // InGameUI::addNamedTimer removes an existing same-name entry before
        // adding its replacement. Preserve that source-order replacement even
        // when only the display type changed from counter to countdown.
        found->label = std::move(label);
        found->kind = kind;
        found->stamp = stamp;
    } else {
        m_namedIndicators.insert(found, {
            .counterName = std::move(counterName),
            .label = std::move(label),
            .kind = kind,
            .stamp = stamp,
        });
    }
    m_lastMutation = stamp;
    return true;
}

bool ScriptUiPresentationState::hideNamedIndicator(container::StringView counterName,
                                                    ScriptPresentationControlStamp stamp) noexcept {
    if (counterName.empty()) return false;
    auto found = findNamedIndicator(m_namedIndicators, counterName);
    if (found == m_namedIndicators.end() || found->counterName != counterName) return false;
    m_namedIndicators.erase(found);
    m_lastMutation = stamp;
    return true;
}

void ScriptUiPresentationState::requestPopup(ScriptPopupMessagePresentation popup) {
    if (popup.text.empty()) return;
    popup.active = true;
    m_popup = std::move(popup);
    m_lastMutation = m_popup.stamp;
}

bool ScriptUiPresentationState::dismissPopup(uint64_t presentationEpoch,
                                              uint64_t presentationSequence) noexcept {
    if (!m_popup.active || m_popup.stamp.presentationEpoch != presentationEpoch ||
        m_popup.stamp.sequence != presentationSequence) {
        return false;
    }
    m_popup.active = false;
    // Retain the source stamp for diagnostics and to make the following
    // replacement unambiguous; only `active` controls presentation.
    m_lastMutation = m_popup.stamp;
    return true;
}

void ScriptUiPresentationState::requestLocalDefeat(ScriptPresentationControlStamp stamp) noexcept {
    // Repeating LOCALDEFEAT is an observable replacement request in RefCode:
    // it marks the local-window flag and creates/replaces the local layout.
    // Keep the newest stamp so a presentation client can reopen a new modal.
    m_localDefeat = {.active = true, .stamp = stamp};
    // The original closes ordinary in-game windows before it creates the
    // local-defeat layout. Keep local defeat client-local, but do not leave
    // camera/control-bar input live underneath that modal outcome.
    m_gameplayInputEnabled = false;
    m_lastMutation = stamp;
}

void ScriptUiPresentationState::enqueueCameoFlash(ScriptCameoFlashPresentation flash) {
    if (flash.commandButton.empty() || flash.framesPerFlash == 0) return;
    // Keep zero-count entries: they are stamped cancellation replacements
    // for a preceding same-name CAMEO_FLASH, not malformed visual requests.
    if (m_pendingCameoFlashes.size() >= kMaximumPendingCameoFlashes) {
        m_pendingCameoFlashes.erase(m_pendingCameoFlashes.begin());
    }
    m_lastMutation = flash.stamp;
    m_pendingCameoFlashes.push_back(std::move(flash));
}

container::Vector<ScriptCameoFlashPresentation> ScriptUiPresentationState::takeCameoFlashes() {
    container::Vector<ScriptCameoFlashPresentation> output = std::move(m_pendingCameoFlashes);
    m_pendingCameoFlashes.clear();
    return output;
}

} // namespace engine::script
