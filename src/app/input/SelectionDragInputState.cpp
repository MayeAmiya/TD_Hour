#include "app/input/SelectionDragInputState.h"

namespace app::input {

bool SelectionDragInputState::begin(const SDL_Event& event) noexcept {
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.button.button != SDL_BUTTON_LEFT) {
        return false;
    }
    m_dragging = true;
    m_clickCount = event.button.clicks;
    m_startX = m_endX = event.button.x;
    m_startY = m_endY = event.button.y;
    return true;
}

bool SelectionDragInputState::update(const SDL_Event& event) noexcept {
    if (!m_dragging || event.type != SDL_EVENT_MOUSE_MOTION) return false;
    m_endX = event.motion.x;
    m_endY = event.motion.y;
    return true;
}

std::optional<SelectionDragCompletion>
SelectionDragInputState::complete(const SDL_Event& event) noexcept {
    if (!m_dragging || event.type != SDL_EVENT_MOUSE_BUTTON_UP ||
        event.button.button != SDL_BUTTON_LEFT) {
        return std::nullopt;
    }
    m_dragging = false;
    m_endX = event.button.x;
    m_endY = event.button.y;
    SelectionDragCompletion result{
        .startX = m_startX,
        .startY = m_startY,
        .endX = m_endX,
        .endY = m_endY,
        .clickCount = m_clickCount,
    };
    m_clickCount = 0;
    return result;
}

void SelectionDragInputState::cancel() noexcept {
    m_dragging = false;
    m_clickCount = 0;
}

} // namespace app::input
