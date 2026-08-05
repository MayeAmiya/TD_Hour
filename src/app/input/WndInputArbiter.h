#pragma once

#include "engine/renderer/runtime/RendererInputViewport.h"

#include <SDL3/SDL_events.h>

namespace engine {
class TextureManager;
}
class InGameGuiSubsystem;

namespace app {
namespace runtime {
struct GameUiProjection;
}
namespace input {

struct WndDispatchResult final {
    bool consumed = false;
    bool modalConsumer = false;
};

// Owns WND hit-testing and modal/text ownership state. It deliberately does
// not know about world gestures or pointer capture; the outer coordinator
// uses the returned decision to arbitrate those domains.
class WndInputArbiter final {
public:
    WndInputArbiter(InGameGuiSubsystem& gui,
                    engine::TextureManager& textures) noexcept
        : m_gui(gui), m_textures(textures) {}

    [[nodiscard]] WndDispatchResult dispatch(
        const SDL_Event& event,
        const runtime::GameUiProjection& projection);
    [[nodiscard]] bool blocksWorldPointer(
        const SDL_Event& event,
        engine::RendererInputViewport viewport) const noexcept;
    [[nodiscard]] bool updateOwnership(
        const runtime::GameUiProjection& projection) noexcept;

    [[nodiscard]] bool modalOwned() const noexcept { return m_modalOwned; }
    [[nodiscard]] bool textOwned() const noexcept { return m_textOwned; }

private:
    InGameGuiSubsystem& m_gui;
    engine::TextureManager& m_textures;
    bool m_modalOwned = false;
    bool m_textOwned = false;
};

} // namespace input
} // namespace app
