#include "app/host/WindowEventCoordinator.h"

#include "game/ini/GameDataLoader.h"
#include "system/RendererSubsystem.h"

#include <SDL3/SDL_events.h>

#include <algorithm>
#include <cstdint>

namespace app {

bool WindowEventCoordinator::dispatch(
    const SDL_Event& event, bool& running) const {
    switch (event.type) {
    case SDL_EVENT_QUIT:
        running = false;
        return true;
    case SDL_EVENT_WINDOW_RESIZED: {
        const uint32_t width = static_cast<uint32_t>(
            std::max(1, event.window.data1));
        const uint32_t height = static_cast<uint32_t>(
            std::max(1, event.window.data2));
        game::GameDataLoader::instance().updateRenderWindowExtent(
            width, height);
        m_renderer.requestResize(width, height);
        return true;
    }
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        m_renderer.requestResize(
            static_cast<uint32_t>(std::max(1, event.window.data1)),
            static_cast<uint32_t>(std::max(1, event.window.data2)));
        return true;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        game::GameDataLoader::instance().setRenderDisplayCapabilities(
            m_renderer.renderDisplayCapabilities());
        return true;
    default:
        return false;
    }
}

} // namespace app
