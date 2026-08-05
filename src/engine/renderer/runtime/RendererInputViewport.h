#pragma once

#include <algorithm>
#include <cstdint>

namespace engine {

// Main-thread value contract used by input hit-testing and command stamping.
// Input code never reaches into the process-global Renderer facade.
struct RendererInputViewport final {
    uint32_t width = 0;
    uint32_t height = 0;
    float uiScaleX = 1.0f;
    float uiScaleY = 1.0f;
    bool fullscreen = false;

    [[nodiscard]] bool valid() const noexcept {
        return width != 0 && height != 0;
    }
    [[nodiscard]] float toUiX(float x) const noexcept {
        return x / std::max(uiScaleX, 0.0001f);
    }
    [[nodiscard]] float toUiY(float y) const noexcept {
        return y / std::max(uiScaleY, 0.0001f);
    }
};

} // namespace engine
