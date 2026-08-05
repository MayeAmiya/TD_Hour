#include "app/input/WndInputArbiter.h"

#include "app/runtime/GameUiProjection.h"
#include "app/ui/ingame/InGameGuiSubsystem.h"
#include "presentation/render/PresentationDefaults.h"

namespace app::input {

WndDispatchResult WndInputArbiter::dispatch(
    const SDL_Event& event,
    const runtime::GameUiProjection& projection) {
    if (!projection.isGameDomain()) return {};
    const bool consumed = m_gui.handleEvent(event, m_textures);
    const auto& popup = projection.scriptUi.popup;
    const bool scriptPopupModal = popup.active &&
        popup.stamp.presentationEpoch ==
            projection.scriptUi.presentationEpoch;
    return {
        .consumed = consumed,
        .modalConsumer = consumed &&
            (m_gui.layer().hasOverlay() || scriptPopupModal),
    };
}

bool WndInputArbiter::blocksWorldPointer(
    const SDL_Event& event,
    engine::RendererInputViewport viewport) const noexcept {
    float screenX = 0.0f;
    float screenY = 0.0f;
    switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        screenX = event.button.x;
        screenY = event.button.y;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        screenX = event.motion.x;
        screenY = event.motion.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        screenX = event.wheel.mouse_x;
        screenY = event.wheel.mouse_y;
        break;
    default:
        return false;
    }
    if (!viewport.valid()) return false;
    const float virtualX = viewport.toUiX(screenX);
    const float virtualY = viewport.toUiY(screenY);
    return m_gui.layer().worldInputBlockedAtVirtual(virtualX, virtualY);
}

bool WndInputArbiter::updateOwnership(
    const runtime::GameUiProjection& projection) noexcept {
    const auto& popup = projection.scriptUi.popup;
    const bool modal = m_gui.layer().hasOverlay() ||
        (popup.active && popup.stamp.presentationEpoch ==
            projection.scriptUi.presentationEpoch);
    const bool text = projection.isGameDomain() &&
        m_gui.hasTextInputFocus();
    const bool newlyOwned =
        (modal && !m_modalOwned) || (text && !m_textOwned);
    m_modalOwned = modal;
    m_textOwned = text;
    return newlyOwned;
}

} // namespace app::input
