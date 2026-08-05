#pragma once

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <optional>

namespace app::input {

struct SelectionDragCompletion final {
    float startX = 0.0f;
    float startY = 0.0f;
    float endX = 0.0f;
    float endY = 0.0f;
    uint8_t clickCount = 0;
};

// Owns only the pointer gesture lifetime. Selection policy, world queries and
// authoritative intent publication remain outside this presentation-local
// state object.
class SelectionDragInputState final {
public:
    [[nodiscard]] bool begin(const SDL_Event& event) noexcept;
    [[nodiscard]] bool update(const SDL_Event& event) noexcept;
    [[nodiscard]] std::optional<SelectionDragCompletion>
    complete(const SDL_Event& event) noexcept;
    void cancel() noexcept;

    [[nodiscard]] bool dragging() const noexcept { return m_dragging; }
    [[nodiscard]] float startX() const noexcept { return m_startX; }
    [[nodiscard]] float startY() const noexcept { return m_startY; }
    [[nodiscard]] float endX() const noexcept { return m_endX; }
    [[nodiscard]] float endY() const noexcept { return m_endY; }

private:
    bool m_dragging = false;
    uint8_t m_clickCount = 0;
    float m_startX = 0.0f;
    float m_startY = 0.0f;
    float m_endX = 0.0f;
    float m_endY = 0.0f;
};

} // namespace app::input
