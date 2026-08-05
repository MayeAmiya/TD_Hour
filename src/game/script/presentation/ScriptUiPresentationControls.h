#pragma once

#include "game/script/contracts/ScriptPresentationValueTypes.h"

#include "core/container/container_types.h"

#include "ScriptCinematicPresentationControls.h"

#include <cstdint>
namespace engine::script {

// These controls are authored by confirmed map scripts, but they deliberately
// describe client presentation policy rather than a WND handle, SDL event, or
// renderer resource.  The runtime action/effect layer uses the same typed
// enum, then GameSession stamps the resulting state for a local UI consumer.

struct ScriptNamedIndicatorPresentation final {
    container::String counterName;
    // This is a localized StringTable key until the client UI resolves it.
    // Keeping the key here prevents locale/UI resources from crossing the
    // deterministic ScriptRuntime boundary.
    container::String label;
    ScriptNamedIndicatorKind kind = ScriptNamedIndicatorKind::Counter;
    ScriptPresentationControlStamp stamp{};
};

// INGAME_POPUP_MESSAGE replaces the old popup immediately.  Its authored
// coordinates remain raw legacy percentages/virtual-pixel width; clamping and
// layout are intentionally client UI work, matching InGameUI::popupMessage.
struct ScriptPopupMessagePresentation final {
    bool active = false;
    container::String text;
    bool localized = true;
    int32_t xPercent = 0;
    int32_t yPercent = 0;
    int32_t width = 50;
    // The UI consumer may honor this as a local campaign pause through its
    // explicit GameLogic presentation-pause authority. It is never a direct
    // script mutation of lockstep simulation and is rejected for network and
    // replay sessions.
    bool pauseRequested = false;
    ScriptPresentationControlStamp stamp{};
};

// LOCALDEFEAT is intentionally separate from mission defeat.  It presents a
// local window and blocks this client's gameplay input without mutating the
// shared Scenario mission outcome.
struct ScriptLocalDefeatPresentation final {
    bool active = false;
    ScriptPresentationControlStamp stamp{};
};

// CAMEO_FLASH targets a CommandButton name. The session carries only this
// stamped value; GameWndLayer resolves it to the currently materialized local
// ControlBar slot and applies the visual flash without making a button/widget
// part of ScriptRuntime or replicated state. A zero `flashCount` is a real
// source-order replacement: it clears an earlier same-name request, matching
// CommandButton::setFlashCount(0), and therefore produces no visible pulse.
struct ScriptCameoFlashPresentation final {
    container::String commandButton;
    uint32_t flashCount = 0;
    uint32_t framesPerFlash = 1;
    ScriptPresentationControlStamp stamp{};
};


// Session-owned value state plus a bounded transient cameo journal.  It has
// no dependency on WndRuntime, renderer objects, localization, simulation
// random, or ECS; all of those belong to their consumer-side boundary.
class ScriptUiPresentationState final {
public:
    static constexpr size_t kMaximumPendingCameoFlashes = 256;

    void reset(uint64_t presentationEpoch = 0) noexcept;
    void rebindPresentationEpoch(uint64_t presentationEpoch) noexcept;

    [[nodiscard]] bool setControl(ScriptUiControlKind control, bool enabled,
                                  ScriptPresentationControlStamp stamp) noexcept;
    [[nodiscard]] bool showNamedIndicator(container::String counterName, container::String label,
                                          ScriptNamedIndicatorKind kind,
                                          ScriptPresentationControlStamp stamp);
    [[nodiscard]] bool hideNamedIndicator(container::StringView counterName,
                                          ScriptPresentationControlStamp stamp) noexcept;
    void requestPopup(ScriptPopupMessagePresentation popup);
    // Presentation-side acknowledgement from the local modal consumer. The
    // expected stamp prevents a stale click from closing a newer script
    // popup that replaced it in the same UI frame.
    [[nodiscard]] bool dismissPopup(uint64_t presentationEpoch,
                                    uint64_t presentationSequence) noexcept;
    void requestLocalDefeat(ScriptPresentationControlStamp stamp) noexcept;
    void enqueueCameoFlash(ScriptCameoFlashPresentation flash);

    [[nodiscard]] bool gameplayInputEnabled() const noexcept { return m_gameplayInputEnabled; }
    [[nodiscard]] bool specialPowerDisplayEnabled() const noexcept {
        return m_specialPowerDisplayEnabled;
    }
    [[nodiscard]] bool namedTimerDisplayEnabled() const noexcept {
        return m_namedTimerDisplayEnabled;
    }
    [[nodiscard]] container::Span<const ScriptNamedIndicatorPresentation> namedIndicators() const noexcept {
        return m_namedIndicators;
    }
    [[nodiscard]] const ScriptPopupMessagePresentation& popup() const noexcept { return m_popup; }
    [[nodiscard]] const ScriptLocalDefeatPresentation& localDefeat() const noexcept {
        return m_localDefeat;
    }
    [[nodiscard]] const ScriptPresentationControlStamp& lastMutation() const noexcept {
        return m_lastMutation;
    }
    [[nodiscard]] container::Vector<ScriptCameoFlashPresentation> takeCameoFlashes();

private:
    bool m_gameplayInputEnabled = true;
    bool m_specialPowerDisplayEnabled = true;
    bool m_namedTimerDisplayEnabled = true;
    container::Vector<ScriptNamedIndicatorPresentation> m_namedIndicators;
    ScriptPopupMessagePresentation m_popup;
    ScriptLocalDefeatPresentation m_localDefeat;
    ScriptPresentationControlStamp m_lastMutation{};
    container::Vector<ScriptCameoFlashPresentation> m_pendingCameoFlashes;
};

} // namespace engine::script
