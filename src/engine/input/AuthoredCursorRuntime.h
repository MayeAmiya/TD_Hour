#pragma once

#include "core/container/hash_containers.h"

#include <SDL3/SDL_mouse.h>

#include <cstdint>

namespace engine::input {

// Main-thread owner for ZH MouseCursor resources. Logical cursor names are
// resolved through Data/INI/Mouse.ini and ANI bytes are read directly from
// VFS, so archived/modded content needs no temporary extraction directory.
class AuthoredCursorRuntime final {
public:
    AuthoredCursorRuntime() = default;
    ~AuthoredCursorRuntime();

    AuthoredCursorRuntime(const AuthoredCursorRuntime&) = delete;
    AuthoredCursorRuntime& operator=(const AuthoredCursorRuntime&) = delete;

    [[nodiscard]] SDL_Cursor* cursor(container::StringView logicalName);
    void clear() noexcept;

private:
    void synchronizeContent();
    [[nodiscard]] bool loadDefinitions();

    container::HashMap<container::String, container::String>
        m_textureByLogicalName;
    container::HashMap<container::String, SDL_Cursor*> m_cursorByTexture;
    uint64_t m_contentRevision = 0;
    bool m_definitionsLoaded = false;
};

} // namespace engine::input
