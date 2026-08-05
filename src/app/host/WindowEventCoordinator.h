#pragma once

union SDL_Event;

class RendererSubsystem;

namespace app {

// Main-thread owner for process/window SDL events. Gameplay input routing
// never updates renderer output state or mutable process content settings.
class WindowEventCoordinator final {
public:
    explicit WindowEventCoordinator(RendererSubsystem& renderer) noexcept
        : m_renderer(renderer) {}

    [[nodiscard]] bool dispatch(
        const SDL_Event& event, bool& running) const;

private:
    RendererSubsystem& m_renderer;
};

} // namespace app
