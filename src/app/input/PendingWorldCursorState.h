#pragma once

#include "core/container/container_types.h"

#include <SDL3/SDL_mouse.h>

#include <cstdint>

namespace engine::input {
class AuthoredCursorRuntime;
}

namespace app::input {

// Owns the temporary SDL cursor installed by a pending world-target command.
// InputCoordinator decides which cursor name is appropriate; this object
// alone manages replacement, fallback allocation and restoration.
class PendingWorldCursorState final {
public:
    PendingWorldCursorState() = default;
    ~PendingWorldCursorState();

    PendingWorldCursorState(const PendingWorldCursorState&) = delete;
    PendingWorldCursorState& operator=(const PendingWorldCursorState&) = delete;

    [[nodiscard]] bool matches(
        uint64_t modeRevision, container::StringView resource) const noexcept;
    void apply(uint64_t modeRevision, container::StringView resource,
               engine::input::AuthoredCursorRuntime& authoredCursors);
    void restore() noexcept;

private:
    SDL_Cursor* m_previous = nullptr;
    SDL_Cursor* m_active = nullptr;
    container::String m_resource;
    uint64_t m_modeRevision = 0;
    bool m_owned = false;
};

} // namespace app::input
