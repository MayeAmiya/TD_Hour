#include "app/input/PendingWorldCursorState.h"

#include "core/container/string_utils.h"
#include "engine/input/AuthoredCursorRuntime.h"

namespace app::input {

PendingWorldCursorState::~PendingWorldCursorState() { restore(); }

bool PendingWorldCursorState::matches(
    uint64_t modeRevision, container::StringView resource) const noexcept {
    return m_modeRevision == modeRevision &&
        container::asciiEqualIgnoreCase(m_resource, resource);
}

void PendingWorldCursorState::apply(
    uint64_t modeRevision, container::StringView resource,
    engine::input::AuthoredCursorRuntime& authoredCursors) {
    if (matches(modeRevision, resource)) return;
    restore();
    m_previous = SDL_GetCursor();
    m_active = authoredCursors.cursor(resource);
    if (!m_active) {
        m_active = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
        m_owned = m_active != nullptr;
    }
    if (m_active) SDL_SetCursor(m_active);
    m_resource.assign(resource);
    m_modeRevision = modeRevision;
}

void PendingWorldCursorState::restore() noexcept {
    if (m_active) {
        SDL_SetCursor(m_previous ? m_previous : SDL_GetDefaultCursor());
        if (m_owned) SDL_DestroyCursor(m_active);
    }
    m_previous = nullptr;
    m_active = nullptr;
    m_resource.clear();
    m_modeRevision = 0;
    m_owned = false;
}

} // namespace app::input
